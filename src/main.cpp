// server/server.hpp and client.hpp must come FIRST to include winsock2.h
// before windows.h, and to undefine the DELETE macro conflict.
#include "server/server.hpp"
#include "server/client.hpp"
#include "server/http_server.hpp"
#include "server/mysql_server.hpp"  // Phase 74: MySQL Wire Protocol

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

#include "engine/engine.hpp"
#include "engine/btree.hpp"
#include "parser/parser.hpp"
#include "storage/storage.hpp"
#include "dispatch.hpp"

// Phase 59: Replication
#include "replication/binlog.hpp"
#include "replication/repl_state.hpp"
#include "replication/master_repl.hpp"
#include "replication/slave_repl.hpp"

// Phase 61: Event Scheduler
#include "scheduler/event_scheduler.hpp"

// Phase 72: WAL Crash Recovery
#include "wal/wal_recovery.hpp"

// ============================================================
// main.cpp — REPL für MilanSQL (Phase 47)
// Neu: --server / --client / --port N Modi
// ============================================================

static void printBanner() {
    std::cout << "\n"
              << "  \u2554\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2557\n"
              << "  \u2551        === MilanSQL v2.5.0 ===           \u2551\n"
              << "  \u2551   Built with <3 by Mirwais Haidari       \u2551\n"
              << "  \u2551  Type 'help' for commands, 'exit' to quit\u2551\n"
              << "  \u255a\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u255d\n"
              << "\n";
}

int main(int argc, char* argv[]) {
    // ── Phase 47/52: Parse command-line arguments ─────────────
    bool serverMode = false;
    bool clientMode = false;
    bool httpMode   = false;
    bool mysqlMode  = false;   // Phase 74: MySQL Wire Protocol
    int  port       = 4406;
    int  httpPort   = 8080;
    int  mysqlPort  = 4407;    // Phase 74: MySQL protocol port
    int  poolSize   = 10;   // Phase 58: Connection Pool Größe
    int  maxQueue   = 100;  // Phase 58: max. Queue-Länge
    // Phase 59: Replication
    bool masterMode  = false;
    bool slaveMode   = false;
    std::string masterHost = "localhost";
    int  masterPort  = 4408;   // Replication port (shifted to avoid conflict with mysql)
    int  replPort    = 4408;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--server")      serverMode = true;
        else if (arg == "--client")      clientMode = true;
        else if (arg == "--http")        httpMode   = true;
        else if (arg == "--mysql")       mysqlMode  = true;
        else if (arg == "--master")      masterMode = true;
        else if (arg == "--slave")       slaveMode  = true;
        else if (arg == "--port"          && i + 1 < argc) port        = std::stoi(argv[++i]);
        else if (arg == "--http-port"     && i + 1 < argc) httpPort    = std::stoi(argv[++i]);
        else if (arg == "--mysql-port"    && i + 1 < argc) mysqlPort   = std::stoi(argv[++i]);
        else if (arg == "--pool-size"     && i + 1 < argc) poolSize    = std::stoi(argv[++i]);
        else if (arg == "--max-queue"     && i + 1 < argc) maxQueue    = std::stoi(argv[++i]);
        else if (arg == "--master-host"   && i + 1 < argc) masterHost  = argv[++i];
        else if (arg == "--master-port"   && i + 1 < argc) masterPort  = std::stoi(argv[++i]);
        else if (arg == "--repl-port"     && i + 1 < argc) replPort    = std::stoi(argv[++i]);
    }

    // ── MySQL Wire Protocol mode (Phase 74) ───────────────────
    if (mysqlMode) {
        std::cout << "MilanSQL MySQL-Protocol Server startet auf Port "
                  << mysqlPort << "...\n";
        milansql::MysqlServer mysqlServer(mysqlPort, "database.milan");
        mysqlServer.run();
        return 0;
    }

    // ── HTTP mode (Phase 52) ──────────────────────────────────
    if (httpMode) {
        std::cout << "MilanSQL HTTP Server startet auf Port " << httpPort << "...\n";
        MilanHttpServer httpServer(httpPort, "database.milan");
        httpServer.run();
        return 0;
    }

    // ── Server mode (Phase 58: Pool-Parameter übergeben) ────────
    if (serverMode) {
        std::string modeTag;
        if (masterMode) modeTag = " [MASTER, Repl-Port " + std::to_string(replPort) + "]";
        else if (slaveMode) modeTag = " [SLAVE -> " + masterHost + ":" + std::to_string(masterPort) + "]";

        std::cout << "MilanSQL Server startet auf Port " << port
                  << " (Pool: " << poolSize << " Threads)" << modeTag << "...\n";

        MilanServer server(port, "database.milan", poolSize, maxQueue);

        // ── Phase 59: Master setup ────────────────────────────
        std::unique_ptr<milansql::BinlogWriter>      binlogWriter;
        std::unique_ptr<milansql::MasterReplication> masterRepl;

        if (masterMode) {
            milansql::g_replState.isMaster.store(true);
            milansql::g_replState.binlogFile = "database.binlog";

            binlogWriter = std::make_unique<milansql::BinlogWriter>("database.binlog");

            milansql::g_binlogHook = [&binlogWriter](const std::string& sql) {
                try { binlogWriter->write(sql); }
                catch (...) {}
            };
            milansql::g_binlogGetPosFn = [&binlogWriter]() -> long long {
                return binlogWriter->getCurrentPos();
            };
            milansql::g_binlogReadLastFn = [&binlogWriter](int n)
                    -> std::vector<milansql::ReplBinlogEntry> {
                auto raw = binlogWriter->readLast(n);
                std::vector<milansql::ReplBinlogEntry> out;
                out.reserve(raw.size());
                for (auto& e : raw)
                    out.push_back({e.pos, e.timestamp, e.sql});
                return out;
            };

            masterRepl = std::make_unique<milansql::MasterReplication>(
                replPort, *binlogWriter);
            masterRepl->start();
        }

        // ── Phase 59: Slave setup ─────────────────────────────
        std::unique_ptr<milansql::SlaveReplication> slaveRepl;

        if (slaveMode) {
            milansql::g_replState.isSlave.store(true);
            milansql::g_replState.masterHost = masterHost;
            milansql::g_replState.masterPort = masterPort;

            slaveRepl = std::make_unique<milansql::SlaveReplication>(
                masterHost, masterPort,
                [&server](const std::string& sql) {
                    milansql::tl_binlogReplay = true;
                    try { server.replaySql(sql); }
                    catch (...) {}
                    milansql::tl_binlogReplay = false;
                });

            milansql::g_stopSlaveHook  = [&slaveRepl]() {
                if (slaveRepl) slaveRepl->stop();
            };
            milansql::g_startSlaveHook = [&slaveRepl]() {
                if (slaveRepl) slaveRepl->resume();
            };

            slaveRepl->start();
        }

        // ── Phase 61: Event Scheduler (server mode) ───────────
        auto srvEventExecFn = [&server](const std::string& sql) {
            try { server.replaySql(sql); } catch (...) {}
        };
        milansql::EventScheduler srvEventScheduler(srvEventExecFn, "database.events");
        milansql::g_eventScheduler = &srvEventScheduler;
        srvEventScheduler.loadEvents();
        srvEventScheduler.start();

        server.run();
        return 0;
    }

    // ── Client mode ───────────────────────────────────────────
    if (clientMode) {
        MilanClient client("127.0.0.1", port);
        if (!client.connect()) {
            std::cerr << "Verbindung fehlgeschlagen.\n";
            return 1;
        }
        client.runREPL();
        return 0;
    }

    // ── REPL mode (unchanged) ─────────────────────────────────
    printBanner();

    milansql::Engine             engine;
    milansql::Parser             parser;
    milansql::MilanBinaryStorage storage;
    std::string                  eingabe;

    // ── Phase 72: WAL Crash Recovery ─────────────────────────────
    // Step 1: Load binary database (last persisted state)
    try {
        std::size_t tableCount = storage.loadWithCount(engine);
        if (tableCount == 0) {
            std::cout << "  Neue Datenbank gestartet.\n\n";
        } else {
            std::size_t rowCount = 0;
            for (const auto& t : engine.getAllTableNamesInternal())
                rowCount += engine.selectAll(t).rowCount();
            std::cout << "  Binary format v" << milansql::MilanBinaryStorage::FORMAT_VERSION
                      << " geladen \u2014 " << tableCount << " Tabelle(n), "
                      << rowCount << " Zeile(n) total.\n\n";
        }
    } catch (const std::exception& ex) {
        std::cout << "  WARNUNG: Laden fehlgeschlagen: " << ex.what()
                  << "\n  Starte mit leerer Datenbank.\n\n";
    }

    // Step 2: WAL Recovery — replay committed ops not yet persisted
    {
        milansql::WalRecovery walRecovery;
        auto wr = walRecovery.recover(engine, "database.milan.wal");
        engine.setRecoveryStatus(wr.hadWal, wr.recoveredTxCount,
                                 wr.discardedTxCount, wr.replayedOpCount);
        if (wr.hadWal) {
            if (wr.recoveredTxCount > 0) {
                std::cout << "  WAL Recovery: " << wr.recoveredTxCount
                          << " Transaktion(en) wiederhergestellt ("
                          << wr.replayedOpCount << " Op(s)), "
                          << wr.discardedTxCount << " verworfen.\n";
                // Persist recovered state
                try { storage.save(engine); }
                catch (...) {}
                std::cout << "  Datenbank nach Recovery gespeichert.\n\n";
            } else if (wr.discardedTxCount > 0) {
                std::cout << "  WAL Recovery: " << wr.discardedTxCount
                          << " uncommittete Transaktion(en) verworfen.\n\n";
            }
        }
    }

    // Phase 51: Load schemas from separate file
    {
        std::ifstream sf("database.schemas");
        if (sf) {
            std::string line;
            while (std::getline(sf, line)) {
                if (!line.empty()) engine.loadSchema(line);
            }
        }
    }

    // Phase 46: Load users from separate file
    engine.loadUsers("database.users");

    // Phase 43: Load triggers from separate file
    {
        std::ifstream tf("database.triggers");
        if (tf) {
            std::string line;
            while (std::getline(tf, line)) {
                if (line.empty()) continue;
                std::vector<std::string> parts;
                size_t pos = 0;
                for (int field = 0; field < 4; ++field) {
                    size_t tab = line.find('\t', pos);
                    if (tab == std::string::npos) { pos = line.size(); break; }
                    parts.push_back(line.substr(pos, tab - pos));
                    pos = tab + 1;
                }
                parts.push_back(line.substr(pos));
                if (parts.size() == 5) {
                    milansql::TriggerDef def;
                    def.name      = parts[0];
                    def.timing    = parts[1];
                    def.event     = parts[2];
                    def.tableName = parts[3];
                    def.body      = parts[4];
                    engine.createTrigger(def);
                }
            }
        }
    }

    // Phase 44: Load procedures from separate file
    {
        std::ifstream pf("database.procedures");
        if (pf) {
            std::string line;
            while (std::getline(pf, line)) {
                if (line.empty()) continue;
                milansql::ProcedureDef def;
                size_t tabPos = line.find('\t');
                if (tabPos == std::string::npos) continue;
                def.name = line.substr(0, tabPos);
                int paramCount = 0;
                try { paramCount = std::stoi(line.substr(tabPos + 1)); }
                catch (...) { continue; }
                for (int pi = 0; pi < paramCount; ++pi) {
                    std::string pline;
                    if (!std::getline(pf, pline)) break;
                    size_t pt = pline.find('\t');
                    if (pt != std::string::npos)
                        def.params.push_back({pline.substr(0, pt), pline.substr(pt + 1)});
                }
                std::string bodyLine;
                if (!std::getline(pf, bodyLine)) { engine.createProcedure(def); continue; }
                std::string decoded;
                for (size_t bi = 0; bi < bodyLine.size(); ++bi) {
                    if (bi + 1 < bodyLine.size() &&
                        bodyLine[bi] == '\\' && bodyLine[bi+1] == 'n') {
                        decoded += ' '; ++bi;
                    } else decoded += bodyLine[bi];
                }
                def.body = decoded;
                engine.createProcedure(def);
            }
        }
    }

    // Phase 75: Load RLS policies
    engine.loadRls("database.rls");

    // Phase 62: Load partitions from separate file
    milansql::dispatch_loadPartitions(engine, "database.partitions");

    // Phase 72: Load Materialized View definitions and rebuild data
    {
        std::ifstream mf("database.matviews");
        if (mf) {
            std::string line;
            while (std::getline(mf, line)) {
                if (line.empty()) continue;
                size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                std::string mvName = line.substr(0, tab);
                std::string mvSql  = line.substr(tab + 1);
                if (mvName.empty() || mvSql.empty()) continue;
                engine.createMaterializedViewEmpty(mvName, mvSql);
                // Refresh: execute the SQL to populate data
                try {
                    milansql::ParsedCommand inner = parser.parse(mvSql);
                    milansql::Table result = milansql::dispatch_executeSelectToTable(
                        engine, parser, inner);
                    engine.setMaterializedViewData(mvName, result.columns(), result.rows());
                } catch (...) {
                    // If refresh fails (e.g. table doesn't exist yet), leave data empty
                }
            }
        }
    }

    // Helper lambda: save procedures to file
    auto saveProcedures = [&]() {
        std::ofstream pf("database.procedures");
        if (!pf) return;
        for (const auto& [n, p] : engine.getAllProcedures()) {
            pf << p.name << "\t" << p.params.size() << "\n";
            for (const auto& param : p.params)
                pf << param.first << "\t" << param.second << "\n";
            std::string enc;
            for (char c : p.body) {
                if (c == '\n') enc += "\\n";
                else enc += c;
            }
            pf << enc << "\n";
        }
    };

    auto saveTriggers = [&]() {
        std::ofstream tf("database.triggers");
        if (tf) {
            for (const auto& [n, t] : engine.getAllTriggers()) {
                tf << t.name << "\t" << t.timing << "\t" << t.event
                   << "\t" << t.tableName << "\t" << t.body << "\n";
            }
        }
    };

    auto persist = [&]() {
        if (engine.isInTransaction()) return;
        try { storage.save(engine); }
        catch (const std::exception& ex) {
            std::cout << "  WARNUNG: Speichern fehlgeschlagen: " << ex.what() << "\n";
        }
        engine.saveRls("database.rls");
    };

    // ── Phase 59: REPL-mode Replication setup ─────────────────
    std::unique_ptr<milansql::BinlogWriter>      replBinlogWriter;
    std::unique_ptr<milansql::MasterReplication> replMasterRepl;
    std::unique_ptr<milansql::SlaveReplication>  replSlaveRepl;

    if (masterMode) {
        milansql::g_replState.isMaster.store(true);
        milansql::g_replState.binlogFile = "database.binlog";
        replBinlogWriter = std::make_unique<milansql::BinlogWriter>("database.binlog");
        milansql::g_binlogHook = [&replBinlogWriter](const std::string& sql) {
            try { replBinlogWriter->write(sql); } catch (...) {}
        };
        milansql::g_binlogGetPosFn = [&replBinlogWriter]() -> long long {
            return replBinlogWriter->getCurrentPos();
        };
        milansql::g_binlogReadLastFn = [&replBinlogWriter](int n)
                -> std::vector<milansql::ReplBinlogEntry> {
            auto raw = replBinlogWriter->readLast(n);
            std::vector<milansql::ReplBinlogEntry> out;
            for (auto& e : raw) out.push_back({e.pos, e.timestamp, e.sql});
            return out;
        };
        replMasterRepl = std::make_unique<milansql::MasterReplication>(
            replPort, *replBinlogWriter);
        replMasterRepl->start();
        std::cout << "  [Master] Replikation aktiv auf Port " << replPort << ".\n\n";
    }

    if (slaveMode) {
        milansql::g_replState.isSlave.store(true);
        milansql::g_replState.masterHost = masterHost;
        milansql::g_replState.masterPort = masterPort;
        replSlaveRepl = std::make_unique<milansql::SlaveReplication>(
            masterHost, masterPort,
            [&](const std::string& sql) {
                milansql::tl_binlogReplay = true;
                try {
                    milansql::ParsedCommand rcmd = parser.parse(sql);
                    milansql::dispatchCommand(rcmd, engine, parser, sql,
                        persist, saveProcedures, saveTriggers);
                } catch (...) {}
                milansql::tl_binlogReplay = false;
            });
        milansql::g_stopSlaveHook  = [&replSlaveRepl]() {
            if (replSlaveRepl) replSlaveRepl->stop();
        };
        milansql::g_startSlaveHook = [&replSlaveRepl]() {
            if (replSlaveRepl) replSlaveRepl->resume();
        };
        replSlaveRepl->start();
        std::cout << "  [Slave] Verbinde zu " << masterHost << ":" << masterPort << "...\n\n";
    }

    // ── Phase 61: Event Scheduler (REPL mode) ────────────────
    auto eventExecFn = [&](const std::string& sql) {
        try {
            milansql::ParsedCommand ecmd = parser.parse(sql);
            milansql::dispatchCommand(ecmd, engine, parser, sql,
                persist, saveProcedures, saveTriggers);
        } catch (...) {}
    };
    milansql::EventScheduler eventScheduler(eventExecFn, "database.events");
    milansql::g_eventScheduler = &eventScheduler;
    eventScheduler.loadEvents();
    eventScheduler.start();

    // ── Phase 73: Buffer Pool Write-Behind Thread ─────────────
    // Flushes dirty pages to disk every 5 seconds (background)
    std::atomic<bool> bufferPoolStop{false};
    std::thread bufferPoolThread([&]() {
        while (!bufferPoolStop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (bufferPoolStop.load()) break;
            auto dirty = engine.getDirtyBufferPages();
            if (!dirty.empty()) {
                try { storage.save(engine); }
                catch (...) {}
                for (const auto& pg : dirty)
                    engine.markBufferPageClean(pg);
            }
        }
    });

    while (true) {
        std::cout << "milansql> " << std::flush;
        if (!std::getline(std::cin, eingabe)) {
            std::cout << "\nAuf Wiedersehen!\n"; break;
        }
        if (eingabe.empty()) continue;

        // Phase 43: Multi-line CREATE TRIGGER — buffer lines until END
        {
            std::string upLine = eingabe;
            for (char& c : upLine)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            size_t s43 = 0;
            while (s43 < upLine.size() && (upLine[s43]==' ' || upLine[s43]=='\t')) ++s43;
            std::string trimmedUp = upLine.substr(s43);
            if (trimmedUp.size() >= 14 && trimmedUp.substr(0, 14) == "CREATE TRIGGER") {
                bool hasBegin = (upLine.find("BEGIN") != std::string::npos);
                bool hasEnd   = false;
                if (hasBegin) {
                    size_t beginPos = upLine.find("BEGIN");
                    std::string afterBegin = upLine.substr(beginPos + 5);
                    size_t endPos = afterBegin.find("END");
                    while (endPos != std::string::npos) {
                        size_t nxtChar = endPos + 3;
                        while (nxtChar < afterBegin.size() && afterBegin[nxtChar] == ' ') ++nxtChar;
                        if (nxtChar >= afterBegin.size() ||
                            (afterBegin.substr(nxtChar, 2) != "IF" &&
                             afterBegin.substr(nxtChar, 2) != "if")) {
                            hasEnd = true; break;
                        }
                        endPos = afterBegin.find("END", endPos + 1);
                    }
                }
                if (hasBegin && !hasEnd) {
                    while (true) {
                        std::string nextLine;
                        if (!std::getline(std::cin, nextLine)) break;
                        eingabe += " " + nextLine;
                        std::string upNext = nextLine;
                        for (char& c : upNext)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        size_t ep = upNext.find("END");
                        bool foundEnd = false;
                        while (ep != std::string::npos) {
                            size_t nc = ep + 3;
                            while (nc < upNext.size() && upNext[nc] == ' ') ++nc;
                            if (nc >= upNext.size() ||
                                (upNext.substr(nc, 2) != "IF" &&
                                 upNext.substr(nc, 2) != "if")) {
                                foundEnd = true; break;
                            }
                            ep = upNext.find("END", ep + 1);
                        }
                        if (foundEnd) break;
                    }
                } else if (!hasBegin) {
                    while (true) {
                        std::string nextLine;
                        if (!std::getline(std::cin, nextLine)) break;
                        eingabe += " " + nextLine;
                        std::string upNext = nextLine;
                        for (char& c : upNext)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        if (upNext.find("BEGIN") != std::string::npos) break;
                    }
                    while (true) {
                        std::string nextLine;
                        if (!std::getline(std::cin, nextLine)) break;
                        eingabe += " " + nextLine;
                        std::string upNext = nextLine;
                        for (char& c : upNext)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        size_t ep = upNext.find("END");
                        bool foundEnd = false;
                        while (ep != std::string::npos) {
                            size_t nc = ep + 3;
                            while (nc < upNext.size() && upNext[nc] == ' ') ++nc;
                            if (nc >= upNext.size() ||
                                (upNext.substr(nc, 2) != "IF" &&
                                 upNext.substr(nc, 2) != "if")) {
                                foundEnd = true; break;
                            }
                            ep = upNext.find("END", ep + 1);
                        }
                        if (foundEnd) break;
                    }
                }
            }
        }

        // Phase 44: Multi-line CREATE PROCEDURE — buffer lines until END
        {
            std::string upLine = eingabe;
            for (char& c : upLine)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            size_t s44 = 0;
            while (s44 < upLine.size() && (upLine[s44]==' '||upLine[s44]=='\t')) ++s44;
            std::string trimmedUp44 = upLine.substr(s44);
            if (trimmedUp44.size() >= 16 &&
                trimmedUp44.substr(0, 16) == "CREATE PROCEDURE") {
                bool hasBegin = (upLine.find("BEGIN") != std::string::npos);
                bool hasEnd   = false;
                if (hasBegin) {
                    size_t beginPos = upLine.find("BEGIN");
                    std::string afterBegin = upLine.substr(beginPos + 5);
                    size_t endPos = afterBegin.find("END");
                    while (endPos != std::string::npos) {
                        size_t nxtChar = endPos + 3;
                        while (nxtChar < afterBegin.size() && afterBegin[nxtChar] == ' ') ++nxtChar;
                        if (nxtChar >= afterBegin.size()) { hasEnd = true; break; }
                        hasEnd = true; break;
                    }
                }
                if (hasBegin && !hasEnd) {
                    while (true) {
                        std::string nextLine;
                        if (!std::getline(std::cin, nextLine)) break;
                        eingabe += " " + nextLine;
                        std::string upNext = nextLine;
                        for (char& c : upNext)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        size_t nt = 0;
                        while (nt < upNext.size() && (upNext[nt]==' '||upNext[nt]=='\t')) ++nt;
                        std::string trimNext = upNext.substr(nt);
                        if (trimNext == "END" || trimNext.substr(0,4) == "END ") break;
                        if (upNext.find("END") != std::string::npos) {
                            size_t ep = upNext.rfind("END");
                            size_t nc = ep + 3;
                            while (nc < upNext.size() && upNext[nc] == ' ') ++nc;
                            if (nc >= upNext.size()) break;
                        }
                    }
                } else if (!hasBegin) {
                    while (true) {
                        std::string nextLine;
                        if (!std::getline(std::cin, nextLine)) break;
                        eingabe += " " + nextLine;
                        std::string upNext = nextLine;
                        for (char& c : upNext)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        if (upNext.find("BEGIN") != std::string::npos) break;
                    }
                    while (true) {
                        std::string nextLine;
                        if (!std::getline(std::cin, nextLine)) break;
                        eingabe += " " + nextLine;
                        std::string upNext = nextLine;
                        for (char& c : upNext)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        size_t nt = 0;
                        while (nt < upNext.size() && (upNext[nt]==' '||upNext[nt]=='\t')) ++nt;
                        std::string trimNext = upNext.substr(nt);
                        if (trimNext == "END" || trimNext.substr(0,4) == "END ") break;
                        if (upNext.find("END") != std::string::npos) {
                            size_t ep = upNext.rfind("END");
                            size_t nc = ep + 3;
                            while (nc < upNext.size() && upNext[nc] == ' ') ++nc;
                            if (nc >= upNext.size()) break;
                        }
                    }
                }
            }
        }

        // ── Phase 67: Multi-Statement Support ────────────────────
        {
            auto stmts67 = milansql::splitStatements(eingabe);
            if (stmts67.size() > 1) {
                // Multiple statements — execute each in order, no interactive prompts
                bool doExit67 = false;
                for (const auto& s67 : stmts67) {
                    if (s67.empty()) continue;
                    try {
                        milansql::ParsedCommand sc67 = parser.parse(s67);
                        for (const auto& sq : sc67.subqueries) {
                            if (sq.condIdx < sc67.whereConds.size())
                                sc67.whereConds[sq.condIdx].inList =
                                    engine.subqueryValues(sq.subTable, sq.subCol,
                                                          sq.subWhere, sq.subWhereLogic);
                        }
                        if (milansql::dispatchCommand(sc67, engine, parser, s67,
                                persist, saveProcedures, saveTriggers))
                            doExit67 = true;
                    } catch (const std::exception& ex) {
                        std::cout << "  FEHLER: " << ex.what() << "\n\n";
                    }
                }
                if (doExit67) return 0;
                continue;
            }
        }

        milansql::ParsedCommand cmd = parser.parse(eingabe);

        // Subqueries auflösen: inList für IN/NOT IN-Bedingungen befüllen
        for (const auto& sq : cmd.subqueries) {
            if (sq.condIdx < cmd.whereConds.size()) {
                cmd.whereConds[sq.condIdx].inList =
                    engine.subqueryValues(
                        sq.subTable, sq.subCol,
                        sq.subWhere, sq.subWhereLogic);
            }
        }

        // Interactive confirmation for dangerous UPDATE without WHERE (REPL only)
        if (cmd.type == milansql::CommandType::UPDATE &&
            !cmd.tableName.empty() && !cmd.updateCols.empty() &&
            cmd.whereColumn.empty()) {
            std::size_t total = engine.countRows(cmd.tableName);
            std::cout << "  WARNUNG: Kein WHERE \u2014 alle " << total
                      << " Zeile(n) werden geaendert.\n"
                      << "  Fortfahren? (j/n): " << std::flush;
            std::string antwort;
            std::getline(std::cin, antwort);
            if (antwort != "j" && antwort != "J") {
                std::cout << "  Abgebrochen.\n\n"; continue;
            }
        }

        // Interactive confirmation for dangerous DELETE without WHERE (REPL only)
        if (cmd.type == milansql::CommandType::DELETE &&
            !cmd.tableName.empty() && cmd.whereColumn.empty()) {
            std::size_t total = engine.countRows(cmd.tableName);
            std::cout << "  WARNUNG: Kein WHERE \u2014 alle " << total
                      << " Zeile(n) werden geloescht.\n"
                      << "  Fortfahren? (j/n): " << std::flush;
            std::string antwort;
            std::getline(std::cin, antwort);
            if (antwort != "j" && antwort != "J") {
                std::cout << "  Abgebrochen.\n\n"; continue;
            }
        }

        try {
            bool doExit = milansql::dispatchCommand(
                cmd, engine, parser, eingabe,
                persist, saveProcedures, saveTriggers);
            if (doExit) {
                bufferPoolStop.store(true);
                bufferPoolThread.detach();
                return 0;
            }
        } catch (const std::exception& ex) {
            std::cout << "  FEHLER: " << ex.what() << "\n\n";
        }
    }

    bufferPoolStop.store(true);
    bufferPoolThread.detach();
    return 0;
}
