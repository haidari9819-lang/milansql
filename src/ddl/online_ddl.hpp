#pragma once
#include <string>
#include <vector>
#include <ctime>

namespace milansql {

struct SchemaChange {
    std::string timestamp;
    std::string sql;
    int         version = 0;

    static std::string now() {
        time_t t = time(nullptr);
        char buf[24];
        struct tm* ti = localtime(&t);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ti);
        return buf;
    }
};

class OnlineDdl {
    int                       version_ = 0;
    std::vector<SchemaChange> history_;
    bool                      inDdlTxn_   = false;
    std::vector<SchemaChange> pendingOps_;

public:
    int getVersion() const { return version_; }

    void recordChange(const std::string& sql) {
        SchemaChange sc;
        sc.sql       = sql;
        sc.timestamp = SchemaChange::now();
        sc.version   = ++version_;
        if (inDdlTxn_) pendingOps_.push_back(sc);
        else            history_.push_back(sc);
    }

    const std::vector<SchemaChange>& getHistory()    const { return history_;    }
    const std::vector<SchemaChange>& getPendingOps() const { return pendingOps_; }

    bool isInDdlTransaction() const { return inDdlTxn_; }

    void beginDdlTransaction()  { inDdlTxn_ = true;  pendingOps_.clear(); }

    void commitDdlTransaction() {
        for (auto& sc : pendingOps_) history_.push_back(sc);
        pendingOps_.clear();
        inDdlTxn_ = false;
    }

    void rollbackDdlTransaction() {
        version_ -= static_cast<int>(pendingOps_.size());
        if (version_ < 0) version_ = 0;
        pendingOps_.clear();
        inDdlTxn_ = false;
    }
};

} // namespace milansql
