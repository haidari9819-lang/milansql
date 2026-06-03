#pragma once
// ============================================================
// dispatch_result.hpp — Lightweight QueryResult + dispatch()
// Phase 125/126: Data-returning dispatch helper for tests
// ============================================================

#include "engine/engine.hpp"
#include "parser/parser.hpp"

namespace milansql {

// Lightweight structured result (used by tests instead of stdout output)
struct QueryResult {
    std::vector<milansql::Column> columns;
    std::vector<milansql::Row>    rows;
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

    // For INSERT/CREATE/etc. in tests, delegate to engine directly
    case milansql::CommandType::CREATE_TABLE:
        engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
        break;

    case milansql::CommandType::INSERT: {
        const auto& rows = cmd.multiValues.empty()
            ? std::vector<std::vector<std::string>>{cmd.values}
            : cmd.multiValues;
        for (const auto& vals : rows)
            engine.insertRow(cmd.tableName, vals);
        break;
    }

    case milansql::CommandType::SELECT:
        if (cmd.isJoin) {
            milansql::Table tbl = engine.executeJoins(cmd.tableName, cmd.joinClauses,
                                                      cmd.whereConds, cmd.whereLogic);
            for (const auto& c : tbl.columns())
                qr.columns.push_back(c);
            for (const auto& r : tbl.rows())
                if (r.xmax == 0) qr.rows.push_back(r);
        } else {
            const milansql::Table& tbl = engine.selectAll(cmd.tableName);
            for (const auto& c : tbl.columns())
                qr.columns.push_back(c);
            for (const auto& r : tbl.rows())
                if (r.xmax == 0) qr.rows.push_back(r);
        }
        break;

    default:
        break;
    }

    return qr;
}

} // namespace milansql
