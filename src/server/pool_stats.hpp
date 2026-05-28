#pragma once
// Phase 58: Globale Pool-Statistiken
// Gemeinsam genutzt von server.hpp (Schreiben) und dispatch.hpp (Lesen)
// Inline-Variable → kein ODR-Problem in Header-Only-Projekten (C++17)

#include <atomic>

namespace milansql {

struct PoolStats {
    std::atomic<int>       poolSize        {10};
    std::atomic<int>       maxQueueSize    {100};
    std::atomic<int>       activeWorkers   {0};
    std::atomic<int>       queuedRequests  {0};
    std::atomic<long long> totalRequests   {0};
    std::atomic<long long> totalQueryTimeUs{0};  // Mikrosekunden gesamt
};

// Globale Instanz (inline since C++17 — eine Definition, mehrere TUs)
inline PoolStats g_poolStats;

} // namespace milansql
