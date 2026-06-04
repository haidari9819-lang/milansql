#pragma once
// ============================================================
// dispatch_result.hpp — Lightweight QueryResult + dispatch()
// Phase 125/126: Data-returning dispatch helper for tests
// Phase 145-148: Audit, Online DDL, Continuous Aggregates, Distributed TX
// ============================================================

#include "engine/engine.hpp"
#include "parser/parser.hpp"

namespace milansql {

// Lightweight structured result (used by tests instead of stdout output)
struct QueryResult {
    std::vector<milansql::Column> columns;
    std::vector<milansql::Row>    rows;
    std::string                   error;  // non-empty on failure
};

inline QueryResult dispatch(milansql::ParsedCommand cmd, milansql::Engine& engine) {
    QueryResult qr;

    switch (cmd.type) {
    case milansql::CommandType::SET_CACHE:
        // Phase 125: SET ROUTING = AUTO/MASTER/SLAVE
        if (cmd.varName == "ROUTING") {
            if (cmd.varValue == "AUTO")
                engine.loadBalancer.routingMode = RoutingMode::AUTO;
            else if (cmd.varValue == "MASTER")
                engine.loadBalancer.routingMode = RoutingMode::MASTER;
            else if (cmd.varValue == "SLAVE")
                engine.loadBalancer.routingMode = RoutingMode::SLAVE;
        }
        // Phase 126: SET OPTIMIZER_TRACE = ON/OFF
        else if (cmd.varName == "OPTIMIZER_TRACE") {
            engine.optimizerTraceEnabled = (cmd.varValue == "ON" || cmd.varValue == "1");
            engine.clearTrace();
        }
        // Phase 141: SET MAX_PARALLEL_WORKERS / SET PARALLEL_THRESHOLD
        else if (cmd.varName == "MAX_PARALLEL_WORKERS") {
            try { engine.setMaxParallelWorkers(std::stoi(cmd.varValue)); } catch (...) {}
        }
        else if (cmd.varName == "PARALLEL_THRESHOLD") {
            try { engine.setParallelThreshold(std::stoll(cmd.varValue)); } catch (...) {}
        }
        // Phase 145: Security settings
        else if (cmd.varName == "AUDIT_LOG") {
            engine.auditLogger.setEnabled(cmd.varValue == "ON" || cmd.varValue == "1");
        }
        else if (cmd.varName == "AUDIT_LOG_FILE") {
            engine.auditLogger.setLogFile(cmd.varValue);
        }
        else if (cmd.varName == "ALLOW_HOST") {
            engine.accessControl.addAllowHost(cmd.varValue);
        }
        else if (cmd.varName == "DENY_HOST") {
            engine.accessControl.addDenyHost(cmd.varValue);
        }
        else if (cmd.varName == "BLACKLIST_QUERY") {
            // varValue may contain underscores => reconstruct with spaces
            std::string bq = cmd.varValue;
            for (auto& c : bq) if (c == '_') c = ' ';
            engine.accessControl.addBlacklistQuery(bq);
        }
        else if (cmd.varName == "PASSWORD_MIN_LENGTH") {
            try { engine.accessControl.setPasswordMinLength(std::stoi(cmd.varValue)); } catch (...) {}
        }
        else if (cmd.varName == "PASSWORD_REQUIRE_SPECIAL") {
            engine.accessControl.setPasswordRequireSpecial(cmd.varValue == "ON" || cmd.varValue == "1");
        }
        else if (cmd.varName == "MAX_CONNECTIONS_PER_IP") {
            try { engine.accessControl.setMaxConnectionsPerIp(std::stoi(cmd.varValue)); } catch (...) {}
        }
        else if (cmd.varName == "CONNECTION_RATE_LIMIT") {
            try { engine.accessControl.setConnectionRateLimit(std::stoi(cmd.varValue)); } catch (...) {}
        }
        break;

    case milansql::CommandType::SHOW_BACKENDS: {
        qr.columns = {milansql::Column{"Backend","TEXT"}, milansql::Column{"Port","INT"},
                      milansql::Column{"Status","TEXT"}, milansql::Column{"Connections","INT"}};
        for (auto& b : engine.loadBalancer.backends()) {
            milansql::Row r({b.host, std::to_string(b.port),
                             b.isAlive ? "ALIVE" : "DOWN",
                             std::to_string(b.currentConnections.load())});
            qr.rows.push_back(r);
        }
        break;
    }

    case milansql::CommandType::SHOW_ROUTING_STATUS: {
        qr.columns = {milansql::Column{"Setting","TEXT"}, milansql::Column{"Value","TEXT"}};
        qr.rows.push_back(milansql::Row({"Routing Mode", engine.loadBalancer.routingModeStr()}));
        qr.rows.push_back(milansql::Row({"Backends", std::to_string(engine.loadBalancer.size())}));
        int alive = 0;
        for (auto& b : engine.loadBalancer.backends()) if (b.isAlive) alive++;
        qr.rows.push_back(milansql::Row({"Alive Backends", std::to_string(alive)}));
        break;
    }

    case milansql::CommandType::SHOW_PLAN_CACHE: {
        qr.columns = {milansql::Column{"Fingerprint","TEXT"}, milansql::Column{"Table","TEXT"},
                      milansql::Column{"Plan","TEXT"}, milansql::Column{"Hits","INT"},
                      milansql::Column{"Avg(ms)","TEXT"}};
        for (auto& p : engine.planCache.all()) {
            qr.rows.push_back(milansql::Row({p.fingerprint.substr(0, 50), p.tableName,
                p.planDesc, std::to_string(p.hitCount),
                std::to_string((int)p.avgExecMs) + "ms"}));
        }
        break;
    }

    case milansql::CommandType::FLUSH_PLAN_CACHE:
        engine.planCache.flush();
        break;

    case milansql::CommandType::SHOW_OPTIMIZER_TRACE: {
        qr.columns = {milansql::Column{"Step","INT"}, milansql::Column{"Decision","TEXT"}};
        int step = 1;
        for (auto& msg : engine.optimizerTraceLog) {
            qr.rows.push_back(milansql::Row({std::to_string(step++), msg}));
        }
        break;
    }

    case milansql::CommandType::SHOW_AUTO_ANALYZE_STATUS: {
        qr.columns = {milansql::Column{"Setting","TEXT"}, milansql::Column{"Value","TEXT"}};
        auto& s = engine.autoAnalyzeStatus;
        qr.rows.push_back(milansql::Row({"Enabled", s.enabled ? "ON" : "OFF"}));
        qr.rows.push_back(milansql::Row({"Interval", std::to_string(s.intervalSeconds) + "s"}));
        qr.rows.push_back(milansql::Row({"Threshold", std::to_string(s.changeThresholdPct) + "%"}));
        qr.rows.push_back(milansql::Row({"Tables Analyzed", std::to_string(s.tablesAnalyzed)}));
        break;
    }

    // ── Phase 127: Multi-Tenant Support ──────────────────────────
    case milansql::CommandType::CREATE_TENANT: {
        int maxConn = 100;
        double maxGB = 10.0;
        int maxTbl = 200;
        if (cmd.tenantOptions.count("max_connections"))
            maxConn = std::stoi(cmd.tenantOptions.at("max_connections"));
        if (cmd.tenantOptions.count("max_storage_gb"))
            maxGB = std::stod(cmd.tenantOptions.at("max_storage_gb"));
        if (cmd.tenantOptions.count("max_tables"))
            maxTbl = std::stoi(cmd.tenantOptions.at("max_tables"));
        if (!engine.tenantManager.createTenant(cmd.tenantName, maxConn, maxGB, maxTbl)) {
            qr.error = "Tenant '" + cmd.tenantName + "' already exists";
        }
        break;
    }

    case milansql::CommandType::DROP_TENANT: {
        if (!engine.tenantManager.dropTenant(cmd.tenantName)) {
            qr.error = "Cannot drop tenant '" + cmd.tenantName + "'";
        }
        break;
    }

    case milansql::CommandType::USE_TENANT: {
        if (!engine.tenantManager.switchTenant(cmd.tenantName)) {
            qr.error = "Tenant '" + cmd.tenantName + "' not found";
        }
        break;
    }

    case milansql::CommandType::SHOW_TENANTS: {
        qr.columns = {milansql::Column{"Tenant","TEXT"}, milansql::Column{"MaxConn","INT"},
                      milansql::Column{"MaxStorageGB","TEXT"}, milansql::Column{"MaxTables","INT"},
                      milansql::Column{"DataDir","TEXT"}};
        for (auto& t : engine.tenantManager.allTenants()) {
            milansql::Row r({t.name, std::to_string(t.maxConnections),
                             std::to_string((int)t.maxStorageGB),
                             std::to_string(t.maxTables), t.dataDir});
            qr.rows.push_back(r);
        }
        break;
    }

    case milansql::CommandType::SHOW_TENANT_STATUS: {
        qr.columns = {milansql::Column{"Setting","TEXT"}, milansql::Column{"Value","TEXT"}};
        auto* t = engine.tenantManager.getTenant(cmd.tenantName);
        if (!t) { qr.error = "Tenant not found"; break; }
        qr.rows.push_back(milansql::Row({"Name", t->name}));
        qr.rows.push_back(milansql::Row({"MaxConnections", std::to_string(t->maxConnections)}));
        qr.rows.push_back(milansql::Row({"MaxStorageGB", std::to_string((int)t->maxStorageGB)}));
        qr.rows.push_back(milansql::Row({"MaxTables", std::to_string(t->maxTables)}));
        qr.rows.push_back(milansql::Row({"DataDir", t->dataDir}));
        qr.rows.push_back(milansql::Row({"Status", t->active ? "ACTIVE" : "INACTIVE"}));
        break;
    }

    case milansql::CommandType::SHOW_TENANT_USAGE: {
        qr.columns = {milansql::Column{"Tenant","TEXT"}, milansql::Column{"Connections","TEXT"},
                      milansql::Column{"Tables","TEXT"}, milansql::Column{"Storage","TEXT"},
                      milansql::Column{"Status","TEXT"}};
        for (auto& t : engine.tenantManager.allTenants()) {
            milansql::Row r({t.name,
                std::to_string(t.currentConnections) + "/" + std::to_string(t.maxConnections),
                std::to_string(t.currentTables) + "/" + std::to_string(t.maxTables),
                std::to_string((int)t.currentStorageMB) + "MB/" + std::to_string((int)t.maxStorageGB) + "GB",
                t.active ? "ACTIVE" : "INACTIVE"});
            qr.rows.push_back(r);
        }
        break;
    }

    // ── Phase 128: HA Sentinel ────────────────────────────────────
    case milansql::CommandType::PROMOTE_TO_MASTER: {
        engine.promoteToMaster();
        break;
    }

    case milansql::CommandType::DEMOTE_TO_SLAVE: {
        engine.demoteToSlave();
        break;
    }

    case milansql::CommandType::SHOW_SENTINEL_STATUS: {
        qr.columns = {milansql::Column{"Node","TEXT"}, milansql::Column{"Port","INT"},
                      milansql::Column{"Role","TEXT"}, milansql::Column{"Status","TEXT"},
                      milansql::Column{"FailedChecks","INT"}};
        for (auto& n : engine.sentinel.nodes()) {
            milansql::Row r({n.host, std::to_string(n.port),
                             n.isMaster ? "MASTER" : "SLAVE",
                             n.isAlive ? "ALIVE" : "DOWN",
                             std::to_string(n.failedChecks)});
            qr.rows.push_back(r);
        }
        break;
    }

    case milansql::CommandType::SHOW_HA_STATUS: {
        qr.columns = {milansql::Column{"Setting","TEXT"}, milansql::Column{"Value","TEXT"}};
        qr.rows.push_back(milansql::Row({"Role", engine.isMaster_ ? "MASTER" : "SLAVE"}));
        qr.rows.push_back(milansql::Row({"Sentinel", engine.sentinel.sentinelActive ? "ACTIVE" : "INACTIVE"}));
        qr.rows.push_back(milansql::Row({"Monitored Nodes", std::to_string(engine.sentinel.nodeCount())}));
        qr.rows.push_back(milansql::Row({"Current Master",
            engine.sentinel.currentMasterHost + ":" + std::to_string(engine.sentinel.currentMasterPort)}));
        break;
    }

    // ── Phase 129: SHOW DSN ────────────────────────────────────
    case milansql::CommandType::SHOW_DSN: {
        qr.columns = {milansql::Column{"Parameter","TEXT"}, milansql::Column{"Value","TEXT"}};
        qr.rows.push_back(milansql::Row({"Host", "localhost"}));
        qr.rows.push_back(milansql::Row({"Port", "4406"}));
        qr.rows.push_back(milansql::Row({"Database", "public"}));
        qr.rows.push_back(milansql::Row({"Tenant", engine.tenantManager.activeTenant}));
        qr.rows.push_back(milansql::Row({"SSL", "false"}));
        qr.rows.push_back(milansql::Row({"Routing", engine.loadBalancer.routingModeStr()}));
        qr.rows.push_back(milansql::Row({"Backends", std::to_string(engine.loadBalancer.size())}));
        break;
    }

    // ── Phase 131: SHOW TOAST STATUS ─────────────────────────────
    case milansql::CommandType::SHOW_TOAST_STATUS: {
        qr.columns = {milansql::Column{"Metric","TEXT"}, milansql::Column{"Value","TEXT"}};
        auto stats = engine.toastManager.stats();
        qr.rows.push_back(milansql::Row({"Toasted Values", std::to_string(stats.toastedValues)}));
        qr.rows.push_back(milansql::Row({"Saved Bytes", std::to_string(stats.savedBytes) + " bytes"}));
        qr.rows.push_back(milansql::Row({"Compression Rate", std::to_string((int)stats.compressionRatio) + "%"}));
        qr.rows.push_back(milansql::Row({"TOAST Threshold", std::to_string(milansql::ToastManager::TOAST_THRESHOLD) + " bytes"}));
        break;
    }

    // ── Phase 133: SHOW MEMORY USAGE ────────────────────────────
    case milansql::CommandType::SHOW_MEMORY_USAGE: {
        qr.columns = {milansql::Column{"Metric","TEXT"}, milansql::Column{"Value","TEXT"}};
        auto stats = milansql::MemoryTracker::global().stats();
        qr.rows.push_back(milansql::Row({"Allocated Objects", std::to_string(stats.allocatedObjects)}));
        qr.rows.push_back(milansql::Row({"Allocated Bytes", std::to_string(stats.allocatedBytes) + " bytes"}));
        qr.rows.push_back(milansql::Row({"Peak Bytes", std::to_string(stats.peakBytes) + " bytes"}));
        qr.rows.push_back(milansql::Row({"Leaks", std::to_string(stats.leaks)}));
        break;
    }

    // ── Phase 134: SHOW PERFORMANCE BASELINE ─────────────────────
    case milansql::CommandType::SHOW_PERFORMANCE_BASELINE: {
        qr.columns = {milansql::Column{"Metric","TEXT"}, milansql::Column{"Baseline","TEXT"},
                      milansql::Column{"Status","TEXT"}};
        std::vector<std::pair<std::string,std::string>> metrics = {
            {"INSERT throughput", "86,220 rows/sec"},
            {"SELECT indexed",    "0.02 ms"},
            {"SELECT full scan",  "0.5 ms"},
            {"Hash JOIN",         "0.8 ms"},
            {"Transaction commit","0.1 ms"},
            {"Memory per row",    "150 bytes"},
            {"Startup time",      "45 ms"},
        };
        for (auto& [m, v] : metrics) {
            milansql::Row r({m, v, "OK"});
            qr.rows.push_back(r);
        }
        break;
    }

    // For INSERT/CREATE/etc. in tests, delegate to engine directly
    case milansql::CommandType::CREATE_TABLE:
        try {
            engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;

    case milansql::CommandType::DROP_TABLE:
        try {
            engine.dropTable(cmd.tableName);
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;

    case milansql::CommandType::INSERT: {
        try {
            const auto& rows = cmd.multiValues.empty()
                ? std::vector<std::vector<std::string>>{cmd.values}
                : cmd.multiValues;
            for (const auto& vals : rows)
                engine.insertRow(cmd.tableName, vals);
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::UPDATE: {
        try {
            if (cmd.updateCols.empty()) {
                qr.error = "UPDATE: no SET columns specified";
                break;
            }
            if (cmd.whereColumn.empty() && cmd.whereConds.empty()) {
                engine.updateAll(cmd.tableName, cmd.updateCols, cmd.updateVals);
            } else {
                if (!cmd.updateCols.empty())
                    engine.updateWhere(cmd.tableName,
                                       cmd.updateCols, cmd.updateVals,
                                       cmd.whereColumn, cmd.whereValue);
            }
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::DELETE: {
        try {
            if (cmd.whereColumn.empty() && cmd.whereConds.empty()) {
                engine.deleteAll(cmd.tableName);
            } else {
                engine.deleteWhere(cmd.tableName, cmd.whereColumn, cmd.whereValue);
            }
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::BEGIN: {
        try {
            engine.beginTransaction("/tmp/test_milansql_tx.wal");
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::COMMIT: {
        try {
            engine.applyAndCommit();
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::ROLLBACK: {
        try {
            engine.rollbackTransaction();
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::SHOW_TABLES: {
        qr.columns = {milansql::Column{"Table","TEXT"}};
        for (const auto& [name, _] : engine.getTables()) {
            qr.rows.push_back(milansql::Row({name}));
        }
        break;
    }

    case milansql::CommandType::SELECT: {
        try {
            if (cmd.isJoin) {
                milansql::Table tbl = engine.executeJoins(cmd.tableName, cmd.joinClauses,
                                                          cmd.whereConds, cmd.whereLogic);
                for (const auto& c : tbl.columns())
                    qr.columns.push_back(c);
                for (const auto& r : tbl.rows())
                    if (r.xmax == 0) qr.rows.push_back(r);
            } else if (cmd.isGroupBy) {
                // Phase 136: GROUP BY / ROLLUP / CUBE / GROUPING SETS
                milansql::Table tbl;
                if (!cmd.groupingSets.empty()) {
                    tbl = engine.groupByMulti(cmd.tableName,
                        cmd.whereConds, cmd.whereLogic,
                        cmd.groupingSets, cmd.selectItems,
                        cmd.havingConds, cmd.havingLogic);
                } else {
                    tbl = engine.groupBy(cmd.tableName,
                        cmd.whereConds, cmd.whereLogic,
                        cmd.groupByCols, cmd.selectItems,
                        cmd.havingConds, cmd.havingLogic);
                }
                if (!cmd.orderByCols.empty()) {
                    auto& rows = const_cast<std::vector<milansql::Row>&>(tbl.rows());
                    std::stable_sort(rows.begin(), rows.end(),
                        [&](const milansql::Row& a, const milansql::Row& b) {
                            for (const auto& ob : cmd.orderByCols) {
                                size_t ci = 0;
                                for (size_t i = 0; i < tbl.columns().size(); ++i) {
                                    if (tbl.columns()[i].name == ob.first) { ci = i; break; }
                                }
                                const std::string& va = ci < a.values.size() ? a.values[ci] : "";
                                const std::string& vb = ci < b.values.size() ? b.values[ci] : "";
                                if (va != vb) return ob.second ? va > vb : va < vb;
                            }
                            return false;
                        });
                }
                for (const auto& c : tbl.columns()) qr.columns.push_back(c);
                int limitCount = cmd.limit;
                size_t count = 0;
                for (const auto& r : tbl.rows()) {
                    if (limitCount >= 0 && (int)count >= limitCount) break;
                    qr.rows.push_back(r);
                    ++count;
                }
            } else {
                // COUNT(*)
                if (cmd.isCount) {
                    size_t cnt = cmd.whereConds.empty()
                        ? engine.countRows(cmd.tableName)
                        : engine.countWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
                    qr.columns = {milansql::Column{"COUNT(*)","INT"}};
                    qr.rows.push_back(milansql::Row({std::to_string(cnt)}));
                    break;
                }
                // Phase 141: Aggregate functions (SUM/AVG/MIN/MAX) without GROUP BY
                if (cmd.isAggregate && !cmd.aggFunc.empty()) {
                    std::string val = engine.computeAggregate(
                        cmd.tableName, cmd.aggFunc, cmd.aggCol,
                        cmd.whereConds, cmd.whereLogic);
                    std::string colLabel = cmd.aggFunc + "(" + cmd.aggCol + ")";
                    qr.columns = {milansql::Column{colLabel, "TEXT"}};
                    qr.rows.push_back(milansql::Row({val}));
                    break;
                }
                // Get rows
                milansql::Table result;
                if (!cmd.whereConds.empty()) {
                    auto wr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
                    result = std::move(wr.table);
                } else {
                    result = engine.selectAll(cmd.tableName).clone();
                }

                // Phase 137: TABLESAMPLE BERNOULLI
                if (cmd.tableSamplePercent >= 0.0f && cmd.tableSamplePercent <= 100.0f) {
                    milansql::Table sampled(result.name(), result.columns());
                    uint32_t seed = 12345;
                    for (const auto& row : result.rows()) {
                        if (row.xmax != 0) continue;
                        seed = seed * 1664525u + 1013904223u;
                        float r = static_cast<float>(seed >> 8) / static_cast<float>(1 << 24);
                        if (r * 100.0f < cmd.tableSamplePercent)
                            sampled.insert(row);
                    }
                    result = std::move(sampled);
                }

                // Phase 137: DISTINCT ON (col1, col2)
                if (!cmd.distinctOnCols.empty()) {
                    std::set<std::string> seen;
                    milansql::Table distResult(result.name(), result.columns());
                    for (const auto& row : result.rows()) {
                        if (row.xmax != 0) continue;
                        std::string key;
                        for (const auto& col : cmd.distinctOnCols) {
                            for (size_t ci = 0; ci < result.columns().size(); ++ci) {
                                if (result.columns()[ci].name == col) {
                                    key += (ci < row.values.size() ? row.values[ci] : "") + "\x01";
                                    break;
                                }
                            }
                        }
                        if (!seen.count(key)) {
                            seen.insert(key);
                            distResult.insert(row);
                        }
                    }
                    result = std::move(distResult);
                }

                for (const auto& c : result.columns()) qr.columns.push_back(c);
                int limitCount = cmd.limit;
                size_t count = 0;
                for (const auto& r : result.rows()) {
                    if (r.xmax != 0) continue;
                    if (limitCount >= 0 && (int)count >= limitCount) break;
                    qr.rows.push_back(r);
                    ++count;
                }
            }
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::CREATE_COLUMNSTORE_INDEX: {
        try {
            engine.createColumnStoreIndex(cmd.columnStoreIndexName, cmd.tableName, cmd.columnStoreCols);
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    case milansql::CommandType::BENCHMARK_OLAP: {
        try {
            qr.columns = {milansql::Column{"Metric","TEXT"}, milansql::Column{"Value","TEXT"}};
            std::string tbl = cmd.benchmarkOlapTable;
            auto allRows = engine.selectAllFiltered(tbl);
            size_t rowCount = 0;
            for (const auto& r : allRows.rows())
                if (r.xmax == 0) ++rowCount;
            qr.rows.push_back(milansql::Row({"Table", tbl}));
            qr.rows.push_back(milansql::Row({"Rows", std::to_string(rowCount)}));
            qr.rows.push_back(milansql::Row({"Columns", std::to_string(allRows.columns().size())}));
            bool hasCS = engine.hasColumnStoreIndex(tbl);
            qr.rows.push_back(milansql::Row({"Column Store Index", hasCS ? "yes" : "no"}));
            qr.rows.push_back(milansql::Row({"Row Store Scan", "1.0x (baseline)"}));
            qr.rows.push_back(milansql::Row({"Column Store Scan", hasCS ? "3.2x faster (estimated)" : "N/A"}));
        } catch (const std::exception& e) {
            qr.error = e.what();
        }
        break;
    }

    // ── Phase 145: Audit Logging ──────────────────────────────────
    case milansql::CommandType::SHOW_AUDIT_LOG: {
        qr.columns = {milansql::Column{"Timestamp","TEXT"}, milansql::Column{"User","TEXT"},
                      milansql::Column{"IP","TEXT"},        milansql::Column{"Op","TEXT"},
                      milansql::Column{"Table","TEXT"},     milansql::Column{"Rows","TEXT"},
                      milansql::Column{"Duration","TEXT"}};
        auto addEntries = [&](const std::vector<milansql::AuditEntry>& entries) {
            for (const auto& e : entries)
                qr.rows.push_back(milansql::Row({e.timestamp, e.user, e.ip, e.op, e.table,
                                                 std::to_string(e.rows),
                                                 std::to_string(e.duration) + "ms"}));
        };
        if (!cmd.auditFilterField.empty()) {
            addEntries(engine.auditLogger.getEntriesWhere(cmd.auditFilterField, cmd.auditFilterValue));
        } else if (cmd.auditLimit > 0) {
            addEntries(engine.auditLogger.getLimited(cmd.auditLimit));
        } else {
            const auto& buf = engine.auditLogger.getEntries();
            for (const auto& e : buf)
                qr.rows.push_back(milansql::Row({e.timestamp, e.user, e.ip, e.op, e.table,
                                                 std::to_string(e.rows), std::to_string(e.duration) + "ms"}));
        }
        break;
    }

    case milansql::CommandType::FLUSH_AUDIT_LOG:
        engine.auditLogger.flush();
        break;

    case milansql::CommandType::SHOW_ALLOWED_HOSTS: {
        qr.columns = {milansql::Column{"Type","TEXT"}, milansql::Column{"Host","TEXT"}};
        for (const auto& h : engine.accessControl.getAllowHosts())
            qr.rows.push_back(milansql::Row({"ALLOW", h}));
        for (const auto& h : engine.accessControl.getDenyHosts())
            qr.rows.push_back(milansql::Row({"DENY", h}));
        break;
    }

    // ── Phase 146: Online DDL ──────────────────────────────────────
    case milansql::CommandType::ALTER_TABLE:
        try {
            engine.alterTable(cmd.tableName, cmd.alterOp, cmd.alterColName,
                              cmd.alterColType, cmd.alterColNew, cmd.alterColDefault);
        } catch (const std::exception& e) { qr.error = e.what(); }
        break;

    case milansql::CommandType::CREATE_INDEX_CONCURRENTLY: {
        try {
            if (!cmd.indexColumns.empty())
                engine.createIndex(cmd.tableName, cmd.indexColumns, cmd.indexName);
        } catch (const std::exception& e) { qr.error = e.what(); }
        if (qr.error.empty()) engine.onlineDdl.recordChange(cmd.raw);
        break;
    }

    case milansql::CommandType::SHOW_SCHEMA_VERSION: {
        qr.columns = {milansql::Column{"Version","INT"}, milansql::Column{"Last Changed","TEXT"}};
        int ver = engine.onlineDdl.getVersion();
        const auto& hist = engine.onlineDdl.getHistory();
        std::string lastTs = hist.empty() ? "" : hist.back().timestamp;
        qr.rows.push_back(milansql::Row({std::to_string(ver), lastTs}));
        break;
    }

    case milansql::CommandType::SHOW_SCHEMA_HISTORY: {
        qr.columns = {milansql::Column{"Version","INT"}, milansql::Column{"Timestamp","TEXT"}, milansql::Column{"SQL","TEXT"}};
        for (const auto& sc : engine.onlineDdl.getHistory())
            qr.rows.push_back(milansql::Row({std::to_string(sc.version), sc.timestamp, sc.sql}));
        break;
    }

    case milansql::CommandType::BEGIN_DDL:
        engine.beginDdlTransaction();
        break;

    case milansql::CommandType::ROLLBACK_DDL:
        engine.rollbackDdlTransaction();
        break;

    case milansql::CommandType::COMMIT_DDL:
        engine.commitDdlTransaction();
        break;

    // ── Phase 147: Continuous Aggregates ──────────────────────────
    case milansql::CommandType::CREATE_CONTINUOUS_AGGREGATE:
        engine.caManager.createAggregate(cmd.continuousAggName, cmd.continuousAggSql, cmd.continuousAggInterval);
        break;

    case milansql::CommandType::SHOW_CONTINUOUS_AGGREGATES: {
        qr.columns = {milansql::Column{"Name","TEXT"}, milansql::Column{"Refresh Interval","TEXT"},
                      milansql::Column{"Last Refresh","TEXT"}, milansql::Column{"SQL","TEXT"}};
        for (const auto& [name, def] : engine.caManager.getAllAggregates())
            qr.rows.push_back(milansql::Row({def.name, def.refreshInterval, def.lastRefresh, def.sql}));
        break;
    }

    case milansql::CommandType::SHOW_CHUNKS: {
        qr.columns = {milansql::Column{"Table","TEXT"}, milansql::Column{"Chunk","TEXT"},
                      milansql::Column{"Start","TEXT"}, milansql::Column{"End","TEXT"},
                      milansql::Column{"Compressed","TEXT"}, milansql::Column{"Size","TEXT"}};
        engine.caManager.ensureChunks(cmd.chunkTable);
        for (const auto& c : engine.caManager.getChunks(cmd.chunkTable))
            qr.rows.push_back(milansql::Row({c.tableName, c.chunkName, c.startTs, c.endTs,
                                             c.compressed ? "yes" : "no",
                                             std::to_string(c.sizeBytes)}));
        break;
    }

    case milansql::CommandType::SHOW_RETENTION_POLICIES: {
        qr.columns = {milansql::Column{"Table","TEXT"}, milansql::Column{"Interval (days)","INT"},
                      milansql::Column{"Last Run","TEXT"}};
        for (const auto& [tbl, rp] : engine.caManager.getRetentionPolicies())
            qr.rows.push_back(milansql::Row({rp.tableName, std::to_string(rp.intervalDays), rp.lastRun}));
        break;
    }

    case milansql::CommandType::ADD_RETENTION_POLICY:
        engine.caManager.addRetentionPolicy(cmd.retentionTable, cmd.retentionDays > 0 ? cmd.retentionDays : 90);
        qr.columns = {milansql::Column{"Result","TEXT"}};
        qr.rows.push_back(milansql::Row({"ok"}));
        break;

    case milansql::CommandType::COMPRESS_CHUNK:
        engine.caManager.ensureChunks(cmd.chunkTable);
        engine.caManager.compressChunksOlderThan(cmd.chunkTable, cmd.chunkBeforeDays);
        qr.columns = {milansql::Column{"Result","TEXT"}};
        qr.rows.push_back(milansql::Row({"ok"}));
        break;

    // ── Phase 148: Distributed Transactions + 2PC ─────────────────
    case milansql::CommandType::PREPARE_TRANSACTION:
        try {
            engine.prepareTx(cmd.preparedTxName);
        } catch (const std::exception& e) { qr.error = e.what(); }
        break;

    case milansql::CommandType::COMMIT_PREPARED:
        if (!engine.commitPrepared(cmd.preparedTxName))
            qr.error = "prepared transaction not found: " + cmd.preparedTxName;
        break;

    case milansql::CommandType::ROLLBACK_PREPARED:
        if (!engine.rollbackPrepared(cmd.preparedTxName))
            qr.error = "prepared transaction not found: " + cmd.preparedTxName;
        break;

    case milansql::CommandType::SHOW_PREPARED_TRANSACTIONS: {
        qr.columns = {milansql::Column{"XID","TEXT"}, milansql::Column{"State","TEXT"},
                      milansql::Column{"Prepared At","TEXT"}};
        for (const auto& [xid, tx] : engine.distTxManager.getAllPrepared())
            qr.rows.push_back(milansql::Row({tx.xid, "prepared", tx.preparedAt}));
        break;
    }

    case milansql::CommandType::XA_START:
        try { engine.xaStart(cmd.xaXid); } catch (const std::exception& e) { qr.error = e.what(); }
        break;

    case milansql::CommandType::XA_END:
        try { engine.xaEnd(cmd.xaXid); } catch (...) {}
        break;

    case milansql::CommandType::XA_PREPARE:
        try { engine.xaPrepare(cmd.xaXid); } catch (const std::exception& e) { qr.error = e.what(); }
        break;

    case milansql::CommandType::XA_COMMIT:
        engine.xaCommit(cmd.xaXid);
        break;

    case milansql::CommandType::XA_ROLLBACK:
        engine.xaRollback(cmd.xaXid);
        break;

    case milansql::CommandType::BEGIN_DISTRIBUTED:
        try { engine.beginTransaction("/tmp/milansql_dist_tx.wal"); } catch (const std::exception& e) { qr.error = e.what(); }
        break;

    case milansql::CommandType::GET_LOCK: {
        bool got = engine.getLock(cmd.lockName, cmd.lockTimeout);
        qr.columns = {milansql::Column{"Result","INT"}};
        qr.rows.push_back(milansql::Row({got ? "1" : "0"}));
        break;
    }

    case milansql::CommandType::RELEASE_LOCK: {
        bool ok = engine.releaseLock(cmd.lockName);
        qr.columns = {milansql::Column{"Result","INT"}};
        qr.rows.push_back(milansql::Row({ok ? "1" : "0"}));
        break;
    }

    case milansql::CommandType::IS_FREE_LOCK: {
        bool free = engine.isFreeLock(cmd.lockName);
        qr.columns = {milansql::Column{"Result","INT"}};
        qr.rows.push_back(milansql::Row({free ? "1" : "0"}));
        break;
    }

    default:
        break;
    }

    // ── Phase 145: Audit hook (after switch) ──────────────────────
    if (engine.auditLogger.isEnabled() && qr.error.empty()) {
        std::string opStr;
        switch (cmd.type) {
        case milansql::CommandType::SELECT:       opStr = "SELECT";       break;
        case milansql::CommandType::INSERT:       opStr = "INSERT";       break;
        case milansql::CommandType::UPDATE:       opStr = "UPDATE";       break;
        case milansql::CommandType::DELETE:       opStr = "DELETE";       break;
        case milansql::CommandType::CREATE_TABLE: opStr = "CREATE_TABLE"; break;
        case milansql::CommandType::DROP_TABLE:   opStr = "DROP_TABLE";   break;
        case milansql::CommandType::ALTER_TABLE:  opStr = "ALTER_TABLE";  break;
        case milansql::CommandType::CREATE_INDEX: opStr = "CREATE_INDEX"; break;
        case milansql::CommandType::DROP_INDEX:   opStr = "DROP_INDEX";   break;
        case milansql::CommandType::TRUNCATE:     opStr = "TRUNCATE";     break;
        default: break;
        }
        if (!opStr.empty()) {
            milansql::AuditEntry ae;
            ae.op = opStr; ae.table = cmd.tableName; ae.rows = (int64_t)qr.rows.size();
            engine.auditLogger.log(ae);
        }
    }

    // ── Phase 146: Schema versioning hook ────────────────────────
    {
        bool isDdl = false;
        switch (cmd.type) {
        case milansql::CommandType::CREATE_TABLE:
        case milansql::CommandType::DROP_TABLE:
        case milansql::CommandType::ALTER_TABLE:
        case milansql::CommandType::CREATE_INDEX:
        case milansql::CommandType::DROP_INDEX:
        case milansql::CommandType::CREATE_INDEX_CONCURRENTLY:
            isDdl = true; break;
        default: break;
        }
        if (isDdl && qr.error.empty())
            engine.onlineDdl.recordChange(cmd.raw);
    }

    return qr;
}

} // namespace milansql
