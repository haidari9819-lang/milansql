#pragma once
#include <string>
#include <vector>
#include <map>
#include <ctime>

namespace milansql {

// Forward-compatible: store ops as opaque strings (SQL text) for simplicity
struct PreparedTransaction {
    std::string xid;
    std::string preparedAt;
    bool hasOps = false;
};

class DistributedTxManager {
    std::map<std::string, PreparedTransaction> prepared_;

public:
    void prepare(const std::string& xid) {
        PreparedTransaction tx;
        tx.xid    = xid;
        tx.hasOps = true;
        time_t t = time(nullptr); char buf[24];
        struct tm* ti = localtime(&t);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ti);
        tx.preparedAt = buf;
        prepared_[xid] = tx;
    }

    bool hasPrepared(const std::string& xid) const { return prepared_.count(xid) > 0; }

    bool removePrepared(const std::string& xid) {
        return prepared_.erase(xid) > 0;
    }

    const std::map<std::string, PreparedTransaction>& getAllPrepared() const {
        return prepared_;
    }
};

} // namespace milansql
