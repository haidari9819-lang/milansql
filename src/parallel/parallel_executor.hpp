#pragma once
// Phase 77: Parallel Query Execution configuration

namespace milansql {

struct ParallelExecutor {
    long long threshold  = 1000;  // min rows to trigger parallel
    int       numWorkers = 4;     // max worker threads
};

} // namespace milansql
