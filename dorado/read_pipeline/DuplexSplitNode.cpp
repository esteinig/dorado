#include "DuplexSplitNode.h"

#include "utils/sequence_utils.h"
#include "utils/duplex_utils.h"
#include "3rdparty/edlib/edlib/include/edlib.h"

#include <spdlog/spdlog.h>
#include <optional>
#include <cmath>

#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <array>
#include <chrono>
#include <openssl/sha.h>

namespace {

using namespace dorado;

typedef DuplexSplitNode::PosRange PosRange;
typedef DuplexSplitNode::PosRanges PosRanges;

template<class FilterF>
auto filter_ranges(const PosRanges& ranges, FilterF filter_f) {
    PosRanges filtered;
    std::copy_if(ranges.begin(), ranges.end(), std::back_inserter(filtered), filter_f);
    return filtered;
}

//FIXME ask if we can have some copy constructor?
std::shared_ptr<Read> copy_read(const Read &read) {
    auto copy = std::make_shared<Read>();
    copy->raw_data = read.raw_data;
    copy->digitisation = read.digitisation;
    copy->range = read.range;
    copy->offset = read.offset;
    copy->sample_rate = read.sample_rate;

    copy->shift = read.shift;
    copy->scale = read.scale;

    copy->scaling = read.scaling;

    copy->num_chunks = read.num_chunks;
    copy->num_modbase_chunks = read.num_modbase_chunks;

    copy->model_stride = read.model_stride;

    copy->read_id = read.read_id;
    copy->seq = read.seq;
    copy->qstring = read.qstring;
    copy->moves = read.moves;
    copy->base_mod_probs = read.base_mod_probs;
    copy->run_id = read.run_id;
    copy->model_name = read.model_name;

    copy->base_mod_info = read.base_mod_info;

    copy->num_trimmed_samples = read.num_trimmed_samples;

    copy->attributes = read.attributes;
    return copy;
}

//FIXME copied from DataLoader.cpp
std::string get_string_timestamp_from_unix_time(time_t time_stamp_ms) {
    static std::mutex timestamp_mtx;
    std::unique_lock lock(timestamp_mtx);
    //Convert a time_t (seconds from UNIX epoch) to a timestamp in %Y-%m-%dT%H:%M:%S format
    auto time_stamp_s = time_stamp_ms / 1000;
    int num_ms = time_stamp_ms % 1000;
    char buf[32];
    struct tm ts;
    ts = *gmtime(&time_stamp_s);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.", &ts);
    std::string time_stamp_str = std::string(buf);
    time_stamp_str += std::to_string(num_ms);  // add ms
    time_stamp_str += "+00:00";                //add zero timezone
    return time_stamp_str;
}

// Expects the time to be encoded like "2017-09-12T9:50:12.456+00:00".
time_t get_unix_time_from_string_timestamp(const std::string& time_stamp) {
    static std::mutex timestamp_mtx;
    std::unique_lock lock(timestamp_mtx);
    std::tm base_time = {};
    strptime(time_stamp.c_str(), "%Y-%m-%dT%H:%M:%S.", &base_time);
    auto num_ms = std::stoi(time_stamp.substr(20, time_stamp.size()-26));
    return mktime(&base_time) * 1000 + num_ms;
}

std::string adjust_time_ms(const std::string& time_stamp, uint64_t offset_ms) {
    return get_string_timestamp_from_unix_time(
                get_unix_time_from_string_timestamp(time_stamp)
                + offset_ms);
}

//                           T  A     T        T  C     A     G        T     A  C
//std::vector<uint8_t> moves{1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0};
std::vector<uint64_t> move_cum_sums(const std::vector<uint8_t> moves) {
    std::vector<uint64_t> ans(moves.size(), 0);
    if (!moves.empty()) {
        ans[0] = moves[0];
    }
    for (size_t i = 1, n = moves.size(); i < n; i++) {
        ans[i] = ans[i-1] + moves[i];
    }
    return ans;
}

std::string derive_uuid(const std::string& input_uuid, const std::string& desc) {
    // Hash the input UUID using SHA-256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, input_uuid.c_str(), input_uuid.size());
    SHA256_Update(&sha256, desc.c_str(), desc.size());
    SHA256_Final(hash, &sha256);

    // Truncate the hash to 16 bytes (128 bits) to match the size of a UUID
    std::array<unsigned char, 16> truncated_hash;
    std::copy(std::begin(hash), std::begin(hash) + 16, std::begin(truncated_hash));

    // Set the UUID version to 4 (random)
    truncated_hash[6] = (truncated_hash[6] & 0x0F) | 0x40;

    // Set the UUID variant to the RFC 4122 specified value (10)
    truncated_hash[8] = (truncated_hash[8] & 0x3F) | 0x80;

    // Convert the truncated hash to a UUID string
    std::stringstream ss;
    for (size_t i = 0; i < truncated_hash.size(); ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(truncated_hash[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << "-";
        }
    }

    return ss.str();
}

//ranges supposed to be sorted by start coordinate
PosRanges merge_ranges(const PosRanges& ranges, size_t merge_dist) {
    PosRanges merged;
    for (auto r : ranges) {
        assert(merged.empty() || r.first >= merged.back().first);
        if (merged.empty() || r.first > merged.back().second + merge_dist) {
            merged.push_back(r);
        } else {
            merged.back().second = r.second;
        }
    }
    return merged;
}

std::vector<std::pair<size_t, size_t>> detect_pore_signal(torch::Tensor pa_signal,
                                                          float threshold,
                                                          size_t cluster_dist,
                                                          size_t ignore_prefix) {
    //FIXME remove
    spdlog::info("Max raw signal {} pA", pa_signal.index({torch::indexing::Slice(ignore_prefix, torch::indexing::None)}).max().item<float>());

    std::vector<std::pair<size_t, size_t>> ans;
    //FIXME what type to use here?
    auto pore_a = pa_signal.accessor<float, 1>();
    size_t start = 0;
    size_t end = 0;

    for (size_t i = ignore_prefix; i < pore_a.size(0); i++) {
        if (pore_a[i] > threshold) {
            if (end == 0 || i > end + cluster_dist) {
                if (end > 0) {
                    ans.push_back({start, end});
                }
                start = i;
                end = i + 1;
            }
            end = i + 1;
        }
    }
    if (end > 0) {
        ans.push_back(std::pair{start, end});
    }
    assert(start < pore_a.size(0) && end <= pore_a.size(0));
    return ans;
}

std::string print_alignment(const char* query, const char* target, const EdlibAlignResult& result) {
    std::stringstream ss;
    int tpos = result.startLocations[0];

    int qpos = 0;
    for (int i = 0; i < result.alignmentLength; i++) {
        if (result.alignment[i] == EDLIB_EDOP_DELETE) {
            ss << "-";
        } else {
            ss << query[qpos];
            qpos++;
        }
    }

    ss << '\n';

    for (int i = 0; i < result.alignmentLength; i++) {
        if (result.alignment[i] == EDLIB_EDOP_MATCH) {
            ss << "|";
        } else if (result.alignment[i] == EDLIB_EDOP_INSERT) {
            ss << " ";
        } else if (result.alignment[i] == EDLIB_EDOP_DELETE) {
            ss << " ";
        } else if (result.alignment[i] == EDLIB_EDOP_MISMATCH) {
            ss << "*";
        }
    }

    ss << '\n';

    for (int i = 0; i < result.alignmentLength; i++) {
        if (result.alignment[i] == EDLIB_EDOP_INSERT) {
            ss << "-";
        } else {
            ss << target[tpos];
            tpos++;
        }
    }

    return ss.str();
}

//[inc, excl)
//FIXME remove debug
std::optional<PosRange>
find_best_adapter_match(const std::string& adapter,
                        const std::string& seq,
                        int dist_thr,
                        std::optional<PosRange> subrange = std::nullopt,
                        bool debug = false) {
    uint64_t shift = 0;
    uint64_t span = seq.size();
    assert(subrange);
    if (subrange) {
        assert(subrange->first <= subrange->second && subrange->second <= seq.size());
        shift = subrange->first;
        span = subrange->second - subrange->first;
    }
    //might be unnecessary, depending on edlib's empty sequence handling
    if (span == 0) return std::nullopt;

    auto edlib_result = edlibAlign(adapter.c_str(), adapter.size(),
                             seq.c_str() + shift, span,
                             edlibNewAlignConfig(-1, EDLIB_MODE_HW, EDLIB_TASK_PATH, NULL, 0));
                             //TODO edlibNewAlignConfig(-1, EDLIB_MODE_HW, EDLIB_TASK_LOC, NULL, 0));
    assert(edlib_result.status == EDLIB_STATUS_OK);
    std::optional<PosRange> res = std::nullopt;
    if (edlib_result.status == EDLIB_STATUS_OK && edlib_result.editDistance != -1) {
        //FIXME REMOVE and use dist_thr instead of -1
        if (debug/*expect_adapter_prefix*/) {
            spdlog::info("Best adapter match edit distance: {} ; is middle {}",
                        edlib_result.editDistance, abs(int(span / 2) - edlib_result.startLocations[0]) < 1000);
            spdlog::info("Match location: ({}, {})", edlib_result.startLocations[0] + shift, edlib_result.endLocations[0] + shift + 1);
            spdlog::info("\n{}", print_alignment(adapter.c_str(), seq.c_str()+shift, edlib_result));
        }
        if (edlib_result.editDistance <= dist_thr) {
            res = {edlib_result.startLocations[0] + shift, edlib_result.endLocations[0] + shift + 1};
        }
    }
    edlibFreeAlignResult(edlib_result);
    return res;
}

std::vector<PosRange> find_adapter_matches(const std::string& adapter,
                                           const std::string& seq,
                                           int dist_thr,
                                           std::optional<PosRange> opt_subrange = std::nullopt) {
    PosRange subrange = opt_subrange ? *opt_subrange : PosRange{0, seq.size()};
    std::vector<PosRange> answer;
    assert(subrange.first <= subrange.second && subrange.second <= seq.size());

    if (auto best_match = find_best_adapter_match(adapter, seq, dist_thr, subrange, true)) {
        //Try to split again each side
        if (auto left_match = find_best_adapter_match(adapter, seq, dist_thr,
                                        PosRange{subrange.first, best_match->first})) {
            answer.push_back(*left_match);
        }
        answer.push_back(*best_match);
        if (auto right_match = find_best_adapter_match(adapter, seq, dist_thr,
                                        PosRange{best_match->second, subrange.second})) {
            answer.push_back(*right_match);
        }
    }
    return answer;
}

//semi-global alignment of "template region" to "complement region"
bool check_rc_match(const std::string& seq, PosRange templ_r, PosRange compl_r, int dist_thr) {
    assert(templ_r.second > templ_r.first && compl_r.second > compl_r.first && dist_thr >= 0);
    const char* c_seq = seq.c_str();
    std::vector<char> rc_compl(c_seq + compl_r.first, c_seq + compl_r.second);
    dorado::utils::reverse_complement(rc_compl);

    auto edlib_result = edlibAlign(c_seq + templ_r.first,
                            templ_r.second - templ_r.first,
                            rc_compl.data(), rc_compl.size(),
                            //edlibNewAlignConfig(dist_thr, EDLIB_MODE_HW, EDLIB_TASK_DISTANCE, NULL, 0));
                            edlibNewAlignConfig(-1, EDLIB_MODE_HW, EDLIB_TASK_PATH, NULL, 0));
    assert(edlib_result.status == EDLIB_STATUS_OK); //&& edlib_result.editDistance <= dist_thr);
    std::optional<PosRange> res = std::nullopt;
    spdlog::info("Checking ranges [{}, {}] vs [{}, {}]: edist={}\n{}",
                    templ_r.first, templ_r.second,
                    compl_r.first, compl_r.second,
                    edlib_result.editDistance,
                    print_alignment(c_seq + templ_r.first, rc_compl.data(), edlib_result));

    //FIXME integrate dist_thr check right into align call after tweaking the settings
    bool match = (edlib_result.status == EDLIB_STATUS_OK)
                    && (edlib_result.editDistance != -1)
                    && (edlib_result.editDistance <= dist_thr);
    edlibFreeAlignResult(edlib_result);
    return match;
}

//TODO end_reason access?
//signal_range should already be 'adjusted' to stride (e.g. probably gotten from seq_range)
//NB: doesn't set parent_read_id
std::shared_ptr<Read> subread(const Read& read, PosRange seq_range, PosRange signal_range) {
    const int stride = read.model_stride;
    assert(signal_range.first % stride == 0);
    assert(signal_range.second % stride == 0 || (signal_range.second == read.raw_data.size(0) && seq_range.second == read.seq.size()));

    //assert(read.called_chunks.empty() && read.num_chunks_called == 0 && read.num_modbase_chunks_called == 0);
    auto subread = copy_read(read);

    //TODO is it ok, or do we want subread number here?
    const auto subread_id = derive_uuid(read.read_id,
                        std::to_string(seq_range.first) + "-" + std::to_string(seq_range.second));
    subread->read_id = subread_id;
    subread->raw_data = subread->raw_data.index({torch::indexing::Slice(signal_range.first, signal_range.second)});
    subread->attributes.read_number = uint32_t(-1);
    subread->attributes.start_time = adjust_time_ms(subread->attributes.start_time,
                                        (subread->num_trimmed_samples + signal_range.first)
                                            * 1000. / subread->sample_rate);
    //we adjust for it in new start time above
    subread->num_trimmed_samples = 0;
    ////fixme update?
    //subread->range = ???;
    ////fixme update?
    //subread->offset = ???;

    subread->seq = subread->seq.substr(seq_range.first, seq_range.second - seq_range.first);
    subread->qstring = subread->qstring.substr(seq_range.first, seq_range.second - seq_range.first);
    subread->moves = std::vector<uint8_t>(subread->moves.begin() + signal_range.first / stride,
        subread->moves.begin() + signal_range.second / stride);
    assert(signal_range.second == read.raw_data.size(0) || subread->moves.size() * stride == subread->raw_data.size(0));
    return subread;
}

}

namespace dorado {

PosRanges
DuplexSplitNode::possible_pore_regions(const Read& read, float pore_thr) const {
    PosRanges pore_regions;

    const auto move_sums = move_cum_sums(read.moves);
    assert(move_sums.back() == read.seq.length());

    //pA formula before scaling:
    //pA = read->scaling * (raw + read->offset);
    //pA formula after scaling:
    //pA = read->scale * raw + read->shift
    spdlog::info("Analyzing signal in read {}", read.read_id);
    for (auto pore_signal_region : detect_pore_signal(
                                   read.raw_data.to(torch::kFloat) * read.scale + read.shift,
                                   pore_thr,
                                   m_settings.pore_cl_dist,
                                   m_settings.expect_pore_prefix)) {
        auto move_start = pore_signal_region.first / read.model_stride;
        auto move_end = pore_signal_region.second / read.model_stride;
        assert(move_end >= move_start);
        //NB move_start can get to move_sums.size(), because of the stride rounding?
        if (move_start >= move_sums.size() || move_end >= move_sums.size() || move_sums.at(move_start) == 0) {
            //either at very end of the signal or basecalls have not started yet
            continue;
        }
        auto start_pos = move_sums.at(move_start) - 1;
        //NB. adding adapter length
        auto end_pos = move_sums.at(move_end);
        assert(end_pos > start_pos);
        pore_regions.push_back({start_pos, end_pos});
    }
    std::ostringstream oss;
    std::copy(pore_regions.begin(), pore_regions.end(), std::ostream_iterator<PosRange>(oss, "; "));
    spdlog::info("{} regions to check: {}", pore_regions.size(), oss.str());
    return pore_regions;
}

bool
DuplexSplitNode::check_nearby_adapter(const Read& read, PosRange r, int adapter_edist) const {
    return find_best_adapter_match(m_settings.adapter,
                read.seq,
                adapter_edist,
                //including spacer region in search
                PosRange{r.first, std::min(r.second + m_settings.pore_adapter_range, (uint64_t) read.seq.size())},
                true).has_value();
}

//r is potential spacer region
bool
DuplexSplitNode::check_flank_match(const Read& read, PosRange r, int dist_thr) const {
    return r.first >= m_settings.query_flank
            && r.second + m_settings.target_flank <= read.seq.length()
            && check_rc_match(read.seq,
                    {r.first - m_settings.query_flank, r.first - m_settings.query_trim},
                    //including spacer region in search
                    {r.first, r.second + m_settings.target_flank},
                    dist_thr);
}

//std::vector<ReadRange>
std::optional<DuplexSplitNode::PosRange>
DuplexSplitNode::identify_extra_middle_split(const Read& read) const {
    //FIXME parameterize
    const auto adapter_search_span = 1000;

    const auto r_l = read.seq.size();
    if (r_l < m_settings.query_flank + m_settings.target_flank) {
        return std::nullopt;
    }

    spdlog::info("Checking start/end match");
    if (check_rc_match(read.seq, {r_l - m_settings.query_flank, r_l},
            {0, m_settings.target_flank}, m_settings.relaxed_flank_edist)) {
        spdlog::info("Searching for adapter match");
        if (auto adapter_match = find_best_adapter_match(m_settings.adapter, read.seq,
                                m_settings.relaxed_adapter_edist,
                                PosRange{r_l / 2 - adapter_search_span / 2, r_l / 2 + adapter_search_span / 2},
                                true)) {
            auto adapter_start = adapter_match->first;
            spdlog::info("Checking middle match");
            if (check_flank_match(read,
                                {adapter_start, adapter_start},
                                m_settings.relaxed_flank_edist)) {
                return PosRange{adapter_start - 1, adapter_start};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::shared_ptr<Read>>
DuplexSplitNode::split(std::shared_ptr<Read> read, const std::vector<PosRange> &spacers) const {
    if (spacers.empty()) {
        return {read};
    }

    std::vector<std::shared_ptr<Read>> subreads;
    subreads.reserve(spacers.size() + 1);

    const auto seq_to_sig_map = utils::moves_to_map(
            read->moves, read->model_stride, read->raw_data.size(0), read->seq.size() + 1);

    //TODO maybe simplify by adding begin/end stubs?
    uint64_t start_pos = 0;
    uint64_t signal_start = seq_to_sig_map[0];
    for (auto r: spacers) {
        //FIXME Don't forget to correctly process end_reasons!
        subreads.push_back(subread(*read, {start_pos, r.first}, {signal_start, seq_to_sig_map[r.first]}));
        start_pos = r.second;
        signal_start = seq_to_sig_map[r.second];
    }
    assert(read->raw_data.size(0) == seq_to_sig_map[read->seq.size()]);
    subreads.push_back(subread(*read, {start_pos, read->seq.size()}, {signal_start, read->raw_data.size(0)}));
    return subreads;
}

std::vector<std::pair<std::string, DuplexSplitNode::SplitFinderF>>
DuplexSplitNode::build_split_finders() const {
    std::vector<std::pair<std::string, SplitFinderF>> split_finders;
    split_finders.push_back({"PORE_ADAPTER",
        [&](const Read &read) {
            return filter_ranges(
                    possible_pore_regions(read, m_settings.pore_thr),
                    [&](PosRange r) {
                        return check_nearby_adapter(read, r, m_settings.adapter_edist);
                    });
        }});

    if (!m_settings.simplex_mode) {
        split_finders.push_back({"PORE_FLANK",
            [&](const Read &read) {
                return merge_ranges(filter_ranges(
                    possible_pore_regions(read, m_settings.pore_thr),
                    [&](PosRange r) {
                        return check_flank_match(read, r, m_settings.flank_edist);
                    }), m_settings.query_flank + m_settings.target_flank);
            }});

        split_finders.push_back({"PORE_ALL",
            [&](const Read &read) {
                return merge_ranges(filter_ranges(
                    possible_pore_regions(read, m_settings.relaxed_pore_thr),
                    [&](PosRange r) {
                        return check_nearby_adapter(read, r, m_settings.relaxed_adapter_edist)
                                && check_flank_match(read, r, m_settings.relaxed_flank_edist);
                    }), m_settings.query_flank + m_settings.target_flank);
            }});

        split_finders.push_back({"ADAPTER_FLANK",
            [&](const Read &read) {
                return filter_ranges(
                    find_adapter_matches(m_settings.adapter,
                                        read.seq,
                                        m_settings.adapter_edist,
                                        PosRange{m_settings.expect_adapter_prefix, read.seq.size()}),
                    [&](PosRange r) {
                        return check_flank_match(read, {r.first, r.first}, m_settings.flank_edist);
                    });
            }});

        split_finders.push_back({"ADAPTER_MIDDLE",
            [&](const Read &read) {
                if (auto split = identify_extra_middle_split(read)) {
                    return PosRanges{*split};
                } else {
                    return PosRanges();
                }
            }});
    }

    return split_finders;
}

void DuplexSplitNode::worker_thread() {
    using namespace std::chrono;
    spdlog::info("DSN: Hello from worker thread");
    Message message;

    while (m_work_queue.try_pop(message)) {
        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto init_read = std::get<std::shared_ptr<Read>>(message);
        spdlog::info("Processing read {}; length {}", init_read->read_id, init_read->seq.size());

        std::vector<std::shared_ptr<Read>> to_split{init_read};
        for (const auto &[description, split_f] : m_split_finders) {
            spdlog::info("Running {}", description);
            std::vector<std::shared_ptr<Read>> split_round_result;
            for (auto r : to_split) {
                auto start = high_resolution_clock::now();
                auto spacers = split_f(*r);
                auto stop = high_resolution_clock::now();
                spdlog::info("{} duration: {} microseconds", description, duration_cast<microseconds>(stop - start).count());

                //logging begin
                std::ostringstream spacer_oss;
                std::copy(spacers.begin(), spacers.end(), std::ostream_iterator<PosRange>(spacer_oss, "; "));
                //logging end

                start = high_resolution_clock::now();
                auto subreads = split(r, spacers);
                stop = high_resolution_clock::now();
                spdlog::info("Split duration: {} microseconds", duration_cast<microseconds>(stop - start).count());

                //logging begin
                //if (subreads.size() > 1) {
                std::ostringstream id_oss;
                std::transform(subreads.begin(), subreads.end(),
                    std::ostream_iterator<std::string>(id_oss, "; "),
                    [](std::shared_ptr<Read> sr) {return sr->read_id;});
                spdlog::info("DSN: {} strategy {} splits in read {} (subread {}). Spacing regions (in subread coordinates): {}. New read ids: {}",
                                description, spacers.size(), init_read->read_id, r->read_id, spacer_oss.str(), id_oss.str());
                //}
                //logging end

                split_round_result.insert(split_round_result.end(), subreads.begin(), subreads.end());
            }
            to_split = split_round_result;
            spdlog::info("At the end of {} overall number of subreads: {}", description, to_split.size());
        }

        for (auto subread : to_split) {
            subread->parent_read_id = init_read->read_id;
            m_sink.push_message(std::move(subread));
            //FIXME add debug mode which would not do any splitting
            //m_sink.push_message(std::move(message));
        }

    }
}

DuplexSplitNode::DuplexSplitNode(MessageSink& sink, DuplexSplitSettings settings,
                                int num_worker_threads, size_t max_reads)
        : MessageSink(max_reads), m_sink(sink),
            m_settings(settings),
            m_num_worker_threads(num_worker_threads) {
    m_split_finders = build_split_finders();
    for (int i = 0; i < m_num_worker_threads; i++) {
        worker_threads.push_back(std::make_unique<std::thread>(&DuplexSplitNode::worker_thread, this));
    }
}

DuplexSplitNode::~DuplexSplitNode() {
    terminate();

    // Wait for all the Node's worker threads to terminate
    for (auto& t : worker_threads) {
        t->join();
    }

    // Notify the sink that the Node has terminated
    m_sink.terminate();
}

}