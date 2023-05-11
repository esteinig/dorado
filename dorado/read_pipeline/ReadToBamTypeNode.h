#pragma once

#include "ReadPipeline.h"

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace dorado {

class ReadToBamType : public MessageSink {
public:
    ReadToBamType(MessageSink& sink,
                  bool emit_moves,
                  bool rna,
                  bool duplex,
                  size_t num_worker_threads,
                  float modbase_threshold_frac = 0,
                  size_t max_reads = 1000);
    ~ReadToBamType();

private:
    MessageSink& m_sink;
    void worker_thread();

    // Async worker for writing.
    std::vector<std::unique_ptr<std::thread>> m_workers;
    std::atomic<size_t> m_active_threads;

    bool m_emit_moves;
    bool m_rna;
    bool m_duplex;
    uint8_t m_modbase_threshold;
};

}  // namespace dorado
