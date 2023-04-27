#include "Version.h"
#include "data_loader/DataLoader.h"
#include "decode/CPUDecoder.h"
#include "read_pipeline/BaseSpaceDuplexCallerNode.h"
#include "read_pipeline/BasecallerNode.h"
#include "read_pipeline/ReadFilterNode.h"
#include "read_pipeline/ScalerNode.h"
#include "read_pipeline/StereoDuplexEncoderNode.h"
#include "read_pipeline/WriterNode.h"
#include "utils/bam_utils.h"
#include "utils/duplex_utils.h"
#include "utils/log_utils.h"
#if DORADO_GPU_BUILD
#ifdef __APPLE__
#include "nn/MetalCRFModel.h"
#else
#include "nn/CudaCRFModel.h"
#include "utils/cuda_utils.h"
#endif
#endif  // DORADO_GPU_BUILD

#include "utils/models.h"
#include "utils/parameters.h"

#include <argparse.hpp>
#include <spdlog/spdlog.h>

#include <set>
#include <thread>

namespace dorado {

int duplex(int argc, char* argv[]) {
    using dorado::utils::default_parameters;
    utils::InitLogging();

    argparse::ArgumentParser parser("dorado", DORADO_VERSION, argparse::default_arguments::help);
    parser.add_argument("model").help("Model");
    parser.add_argument("reads").help("Reads in Pod5 format or BAM/SAM format for basespace.");
    parser.add_argument("--pairs").help("Space-delimited csv containing read ID pairs.");
    parser.add_argument("--emit-fastq").default_value(false).implicit_value(true);
    parser.add_argument("-t", "--threads").default_value(0).scan<'i', int>();

    parser.add_argument("-x", "--device")
            .help("device string in format \"cuda:0,...,N\", \"cuda:all\", \"metal\" etc..")
            .default_value(utils::default_parameters.device);

    parser.add_argument("-b", "--batchsize")
            .default_value(default_parameters.batchsize)
            .scan<'i', int>()
            .help("if 0 an optimal batchsize will be selected");

    parser.add_argument("-c", "--chunksize")
            .default_value(default_parameters.chunksize)
            .scan<'i', int>();

    parser.add_argument("-o", "--overlap")
            .default_value(default_parameters.overlap)
            .scan<'i', int>();

    parser.add_argument("-r", "--recursive")
            .default_value(false)
            .implicit_value(true)
            .help("Recursively scan through directories to load FAST5 and POD5 files");

    parser.add_argument("--min-qscore").default_value(0).scan<'i', int>();

    try {
        parser.parse_args(argc, argv);

        auto device(parser.get<std::string>("-x"));
        auto model(parser.get<std::string>("model"));
        auto reads(parser.get<std::string>("reads"));
        auto pairs_file(parser.get<std::string>("--pairs"));
        auto threads = static_cast<size_t>(parser.get<int>("--threads"));
        bool emit_fastq = parser.get<bool>("--emit-fastq");
        auto min_qscore(parser.get<int>("--min-qscore"));
        std::vector<std::string> args(argv, argv + argc);

        spdlog::info("> Loading pairs file");
        auto template_complement_map = utils::load_pairs_file(pairs_file);
        spdlog::info("> Pairs file loaded");

        bool emit_moves = false, rna = false, duplex = true;
        WriterNode writer_node(std::move(args), emit_fastq, emit_moves, rna, duplex, 4);
        ReadFilterNode read_filter_node(writer_node, min_qscore, 1);

        torch::set_num_threads(1);

        if (model.compare("basespace") == 0) {  // Execute a Basespace duplex pipeline.
            // create a set of the read_ids
            std::set<std::string> read_ids;
            for (const auto& pair : template_complement_map) {
                read_ids.insert(pair.first);
                read_ids.insert(pair.second);
            }

            spdlog::info("> Loading reads");
            auto read_map = utils::read_bam(reads, read_ids);

            spdlog::info("> Starting Basespace Duplex Pipeline");
            threads = threads == 0 ? std::thread::hardware_concurrency() : threads;
            BaseSpaceDuplexCallerNode duplex_caller_node(read_filter_node, template_complement_map,
                                                         read_map, threads);
        } else {  // Execute a Stereo Duplex pipeline.

            const auto model_path = std::filesystem::canonical(std::filesystem::path(model));

            auto stereo_model_name = utils::get_stereo_model_name(model);
            const auto stereo_model_path =
                    model_path.parent_path() / std::filesystem::path(stereo_model_name);

            if (!std::filesystem::exists(stereo_model_path)) {
                utils::download_models(model_path.parent_path().u8string(), stereo_model_name);
            }

            std::vector<Runner> runners;
            std::vector<Runner> stereo_runners;

            // Default is 1 device.  CUDA path may alter this.
            int num_devices = 1;
            int batch_size(parser.get<int>("-b"));
            int chunk_size(parser.get<int>("-c"));
            int overlap(parser.get<int>("-o"));
            const size_t num_runners = default_parameters.num_runners;

            size_t stereo_batch_size;

            if (device == "cpu") {
                if (batch_size == 0) {
                    batch_size = std::thread::hardware_concurrency();
                    spdlog::debug("- set batch size to {}", batch_size);
                }
                for (size_t i = 0; i < num_runners; i++) {
                    runners.push_back(std::make_shared<ModelRunner<CPUDecoder>>(
                            model_path, device, chunk_size, batch_size));
                }
            }
#if DORADO_GPU_BUILD
#ifdef __APPLE__
            else if (device == "metal") {
                auto simplex_caller = create_metal_caller(model_path, chunk_size, batch_size);
                for (int i = 0; i < num_runners; i++) {
                    runners.push_back(std::make_shared<MetalModelRunner>(simplex_caller));
                }
                if (runners.back()->batch_size() != batch_size) {
                    spdlog::debug("- set batch size to {}", runners.back()->batch_size());
                }

                // For now, the minimal batch size is used for the duplex model.
                stereo_batch_size = 48;

                auto duplex_caller =
                        create_metal_caller(stereo_model_path, chunk_size, stereo_batch_size);
                for (size_t i = 0; i < num_runners; i++) {
                    stereo_runners.push_back(std::make_shared<MetalModelRunner>(duplex_caller));
                }
            } else {
                throw std::runtime_error(std::string("Unsupported device: ") + device);
            }
#else   // ifdef __APPLE__
            else {
                auto devices = utils::parse_cuda_device_string(device);
                num_devices = devices.size();
                if (num_devices == 0) {
                    throw std::runtime_error("CUDA device requested but no devices found.");
                }
                for (auto device_string : devices) {
                    auto caller = create_cuda_caller(model_path, chunk_size, batch_size,
                                                     device_string, 0.5f);  // Use half the GPU mem
                    for (size_t i = 0; i < num_runners; i++) {
                        runners.push_back(std::make_shared<CudaModelRunner>(caller));
                    }
                    if (runners.back()->batch_size() != batch_size) {
                        spdlog::debug("- set batch size for {} to {}", device_string,
                                      runners.back()->batch_size());
                    }
                }

                stereo_batch_size = 1024;

                for (auto device_string : devices) {
                    auto caller = create_cuda_caller(stereo_model_path, chunk_size,
                                                     stereo_batch_size, device_string);
                    for (size_t i = 0; i < num_runners; i++) {
                        stereo_runners.push_back(std::make_shared<CudaModelRunner>(caller));
                    }
                }
            }
#endif  // __APPLE__
#endif  // DORADO_GPU_BUILD
            spdlog::info("> Starting Stereo Duplex pipeline");

            auto stereo_model_stride = stereo_runners.front()->model_stride();

            auto adjusted_stereo_overlap = (overlap / stereo_model_stride) * stereo_model_stride;

            const int kStereoBatchTimeoutMS = 500;
            auto stereo_basecaller_node = std::make_unique<BasecallerNode>(
                    read_filter_node, std::move(stereo_runners), adjusted_stereo_overlap,
                    kStereoBatchTimeoutMS);

            std::unordered_set<std::string> read_list =
                    utils::get_read_list_from_pairs(template_complement_map);

            auto simplex_model_stride = runners.front()->model_stride();

            StereoDuplexEncoderNode stereo_node = StereoDuplexEncoderNode(
                    *stereo_basecaller_node, std::move(template_complement_map),
                    simplex_model_stride);

            auto adjusted_simplex_overlap = (overlap / simplex_model_stride) * simplex_model_stride;

            const int kSimplexBatchTimeoutMS = 100;
            auto basecaller_node = std::make_unique<BasecallerNode>(stereo_node, std::move(runners),
                                                                    adjusted_simplex_overlap,
                                                                    kSimplexBatchTimeoutMS);

            ScalerNode scaler_node(*basecaller_node, num_devices * 2);

            DataLoader loader(scaler_node, "cpu", num_devices, 0, std::move(read_list));
            loader.load_reads(reads, parser.get<bool>("--recursive"));
        }
    } catch (const std::exception& e) {
        spdlog::error(e.what());
        std::exit(1);
    }
    return 0;
}
}  // namespace dorado
