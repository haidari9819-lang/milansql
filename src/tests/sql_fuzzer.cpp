// ============================================================
// sql_fuzzer.cpp — MilanSQL SQL Fuzzer (Phase 101)
// ============================================================

#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <random>
#include <stdexcept>
#include <algorithm>

#include "engine/engine.hpp"
#include "parser/parser.hpp"

using namespace milansql;

// ── Helper: execute SQL via engine ────────────────────────────
static void execSQL(Engine& engine, Parser& parser, const std::string& sql) {
    ParsedCommand cmd = parser.parse(sql);
    switch (cmd.type) {
        case CommandType::CREATE_TABLE:
            engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
            break;
        case CommandType::INSERT: {
            const auto& rows = cmd.multiValues.empty()
                ? std::vector<std::vector<std::string>>{cmd.values}
                : cmd.multiValues;
            for (const auto& vals : rows)
                engine.insertRow(cmd.tableName, vals);
            break;
        }
        case CommandType::DELETE:
            if (!cmd.whereColumn.empty())
                engine.deleteWhere(cmd.tableName, cmd.whereColumn, cmd.whereValue);
            else
                engine.deleteAll(cmd.tableName);
            break;
        case CommandType::UPDATE:
            if (!cmd.whereColumn.empty())
                engine.updateWhere(cmd.tableName, cmd.updateCols, cmd.updateVals,
                                   cmd.whereColumn, cmd.whereValue);
            else
                engine.updateAll(cmd.tableName, cmd.updateCols, cmd.updateVals);
            break;
        case CommandType::BEGIN:
            engine.beginTransaction("/tmp/fuzz_milansql.wal");
            break;
        case CommandType::COMMIT:
            engine.applyAndCommit();
            break;
        case CommandType::ROLLBACK:
            engine.rollbackTransaction();
            break;
        case CommandType::CREATE_INDEX:
            engine.createIndex(cmd.tableName, cmd.indexColumns, cmd.indexName);
            break;
        case CommandType::DROP_TABLE:
            engine.dropTable(cmd.tableName);
            break;
        default:
            break;
    }
}

// ── FuzzResult ────────────────────────────────────────────────
struct FuzzResult {
    int passed = 0;
    int failed = 0;
    int crashes = 0;
    int slowQueries = 0;
    std::vector<std::string> failedQueries;
};

// ── FuzzEngine ────────────────────────────────────────────────
class FuzzEngine {
    Engine engine_;
    Parser parser_;
    std::mt19937 rng_;

    static const std::vector<std::string>& names() {
        static const std::vector<std::string> v{
            "Alice", "Bob", "Carol", "Dave", "Eve", "NULL", ""
        };
        return v;
    }

    static const std::vector<std::string>& vals() {
        static const std::vector<std::string> v{
            "0", "1", "-1", "42", "99999", "3.14", "-0.5"
        };
        return v;
    }

    static const std::vector<std::string>& ops() {
        static const std::vector<std::string> v{
            "=", "<", ">", "!=", ">=", "<="
        };
        return v;
    }

    template<typename V>
    const typename V::value_type& pick(const V& vec) {
        std::uniform_int_distribution<size_t> d(0, vec.size() - 1);
        return vec[d(rng_)];
    }

    std::string randomName() { return pick(names()); }
    std::string randomVal()  { return pick(vals()); }
    std::string randomOp()   { return pick(ops()); }

    int randomInt(int lo, int hi) {
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng_);
    }

    std::string generateRandomSelect() {
        // Randomly choose a WHERE combination
        int variant = randomInt(0, 5);
        switch (variant) {
            case 0:
                return "SELECT * FROM fuzz_t WHERE id " + randomOp() +
                       " " + std::to_string(randomInt(0, 25));
            case 1:
                return "SELECT * FROM fuzz_t WHERE name = '" + randomName() + "'";
            case 2:
                return "SELECT * FROM fuzz_t WHERE val " + randomOp() +
                       " " + randomVal();
            case 3:
                return "SELECT * FROM fuzz_t WHERE id " + randomOp() +
                       " " + std::to_string(randomInt(0, 25)) +
                       " AND name = '" + randomName() + "'";
            case 4:
                return "SELECT * FROM fuzz_t WHERE id " + randomOp() +
                       " " + std::to_string(randomInt(0, 25)) +
                       " OR val " + randomOp() + " " + randomVal();
            case 5:
            default:
                return "SELECT * FROM fuzz_t";
        }
    }

    std::string generateRandomInsert() {
        std::string name = randomName();
        std::string val  = randomVal();
        // Escape single quotes in name
        std::string escapedName;
        for (char c : name) {
            if (c == '\'') escapedName += "''";
            else           escapedName += c;
        }
        return "INSERT INTO fuzz_t (name, val, ts) VALUES ('" +
               escapedName + "', " + val + ", '2024-01-01')";
    }

    std::string generateRandomUpdate() {
        int id = randomInt(1, 25);
        std::string val = randomVal();
        return "UPDATE fuzz_t SET val = " + val +
               " WHERE id = " + std::to_string(id);
    }

    std::string generateRandomDelete() {
        int id = randomInt(18, 25);   // Only delete rows beyond initial 20
        return "DELETE FROM fuzz_t WHERE id = " + std::to_string(id);
    }

    std::string generateRandomTransaction() {
        // A small transaction with an insert + commit
        int val = randomInt(0, 1000);
        return "BEGIN; INSERT INTO fuzz_t (name, val, ts) VALUES ('TxRow', " +
               std::to_string(val) + ", '2024-06-01'); COMMIT";
    }

    // Run a single SQL string and update result
    bool runQuery(const std::string& sql, FuzzResult& result) {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        try {
            execSQL(engine_, parser_, sql);
            ok = true;
        } catch (const std::exception&) {
            // Parse/runtime errors are "failed" but not crashes
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (ms > 2000.0) {
            result.slowQueries++;
            std::cout << "[SLOW] " << sql << "\n";
        }

        if (ok) {
            result.passed++;
            // Uncomment for verbose output:
            // std::cout << "[PASS] " << sql << "\n";
        } else {
            result.failed++;
            result.failedQueries.push_back(sql);
            std::cout << "[FAIL] " << sql << "\n";
        }
        return ok;
    }

public:
    explicit FuzzEngine(unsigned seed) : rng_(seed) { setupSchema(); }

    void setupSchema() {
        execSQL(engine_, parser_,
            "CREATE TABLE fuzz_t ("
            "id INT PRIMARY KEY AUTO_INCREMENT, "
            "name TEXT, "
            "val REAL, "
            "ts TEXT)");

        // Pre-insert 20 rows
        for (int i = 1; i <= 20; i++) {
            std::string sql =
                "INSERT INTO fuzz_t (name, val, ts) VALUES ('User" +
                std::to_string(i) + "', " + std::to_string(i * 1.5) +
                ", '2024-01-" + (i < 10 ? "0" : "") + std::to_string(i) + "')";
            execSQL(engine_, parser_, sql);
        }
    }

    FuzzResult runFuzz(int iterations) {
        FuzzResult result;

        // ── a) Type fuzzing ─────────────────────────────────────
        std::cout << "\n[FUZZ] Category A: Type fuzzing\n";
        {
            // NULL-like values
            std::vector<std::string> typeCases = {
                "INSERT INTO fuzz_t (name, val, ts) VALUES ('', 0, '')",
                "INSERT INTO fuzz_t (name, val, ts) VALUES ('NULL', -1, '0')",
                "INSERT INTO fuzz_t (name, val, ts) VALUES ('Special!@#', 999999999, '2024-12-31')",
                "INSERT INTO fuzz_t (name, val, ts) VALUES ('O''Brien', 3.14, '2024-06-15')",
                "SELECT * FROM fuzz_t WHERE name = ''",
                "SELECT * FROM fuzz_t WHERE val = 0",
                "SELECT * FROM fuzz_t WHERE val = -1",
                "SELECT * FROM fuzz_t WHERE id = 0",
                "SELECT * FROM fuzz_t WHERE id = 999999999",
            };
            for (auto& q : typeCases)
                runQuery(q, result);
        }

        // ── b) Query fuzzing ────────────────────────────────────
        std::cout << "\n[FUZZ] Category B: Query fuzzing\n";
        {
            int qIter = iterations / 4;
            for (int i = 0; i < qIter; i++) {
                int variant = randomInt(0, 3);
                std::string sql;
                switch (variant) {
                    case 0: sql = generateRandomSelect(); break;
                    case 1: sql = generateRandomInsert(); break;
                    case 2: sql = generateRandomUpdate(); break;
                    case 3: sql = generateRandomDelete(); break;
                    default: sql = generateRandomSelect(); break;
                }
                runQuery(sql, result);
            }
        }

        // ── c) Size fuzzing ─────────────────────────────────────
        std::cout << "\n[FUZZ] Category C: Size fuzzing (10000 rows)\n";
        {
            // Create a large table
            try {
                execSQL(engine_, parser_,
                    "CREATE TABLE fuzz_big (id INT, val INT, name TEXT)");
                for (int i = 0; i < 10000; i++) {
                    execSQL(engine_, parser_,
                        "INSERT INTO fuzz_big VALUES (" +
                        std::to_string(i) + ", " +
                        std::to_string(i % 100) + ", 'row" +
                        std::to_string(i) + "')");
                }

                // Run queries on the large table
                std::vector<std::string> bigQueries = {
                    "SELECT * FROM fuzz_big WHERE id = 5000",
                    "SELECT * FROM fuzz_big WHERE val = 42",
                    "SELECT * FROM fuzz_big WHERE val > 90",
                    "SELECT * FROM fuzz_big WHERE id < 10",
                    "SELECT * FROM fuzz_big WHERE id >= 9990",
                };
                for (auto& q : bigQueries)
                    runQuery(q, result);

            } catch (const std::exception& e) {
                std::cout << "[FAIL] Size fuzzing setup: " << e.what() << "\n";
                result.failed++;
            }
        }

        // ── d) Injection fuzzing ────────────────────────────────
        std::cout << "\n[FUZZ] Category D: Injection fuzzing\n";
        {
            std::vector<std::string> injCases = {
                "SELECT * FROM fuzz_t WHERE id = 1 OR 1=1",
                "SELECT * FROM fuzz_t WHERE name = 'x' OR 'x'='x'",
                "SELECT * FROM fuzz_t; DROP TABLE fuzz_t",
                "SELECT * FROM fuzz_t WHERE id = 1; --",
                "INSERT INTO fuzz_t VALUES (1, 'a', 0, 'b')",
                "SELECT * FROM nonexistent_table",
                "SELECT * FROM fuzz_t WHERE nonexistent_col = 1",
                "UPDATE fuzz_t SET nonexistent = 1",
                "DELETE FROM fuzz_t WHERE 1=1",
                "SELECT * FROM fuzz_t WHERE id = 'not_a_number'",
                "INSERT INTO fuzz_t (id, name, val, ts) VALUES (1, 'dup', 0, 'ts')",
                "SELECT * FROM fuzz_t WHERE id = NULL",
                "SELECT * FROM fuzz_t WHERE name LIKE '%'; DROP TABLE fuzz_t --",
                "SELECT * FROM fuzz_t WHERE id = 1 UNION SELECT * FROM fuzz_t",
            };
            for (auto& q : injCases)
                runQuery(q, result);
        }

        // ── e) Remaining random iterations ─────────────────────
        std::cout << "\n[FUZZ] Category E: Random mix\n";
        {
            int remaining = iterations - (iterations / 4) - 9 - 5 - 14;
            if (remaining < 0) remaining = 0;
            for (int i = 0; i < remaining; i++) {
                int variant = randomInt(0, 4);
                std::string sql;
                switch (variant) {
                    case 0: sql = generateRandomSelect(); break;
                    case 1: sql = generateRandomInsert(); break;
                    case 2: sql = generateRandomUpdate(); break;
                    case 3: sql = generateRandomDelete(); break;
                    case 4: sql = generateRandomTransaction(); break;
                    default: sql = generateRandomSelect(); break;
                }
                runQuery(sql, result);
            }
        }

        return result;
    }
};

// ── main ──────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    int iterations = 500;
    unsigned seed = 42;

    if (argc > 1) {
        try { iterations = std::stoi(argv[1]); }
        catch (...) { /* ignore */ }
    }
    if (argc > 2) {
        try { seed = static_cast<unsigned>(std::stoul(argv[2])); }
        catch (...) { /* ignore */ }
    }

    std::cout << "[FUZZ] Running " << iterations << " iterations (seed=" << seed << ")...\n";

    FuzzEngine fe(seed);
    FuzzResult result;

    // Wrap the entire fuzz run to catch unexpected crashes
    try {
        result = fe.runFuzz(iterations);
    } catch (const std::exception& ex) {
        std::cerr << "[CRASH] Unhandled exception: " << ex.what() << "\n";
        result.crashes++;
    } catch (...) {
        std::cerr << "[CRASH] Unknown exception escaped fuzz loop\n";
        result.crashes++;
    }

    // Cleanup
    std::remove("/tmp/fuzz_milansql.wal");

    std::cout << "\n=== Fuzz Results ===\n";
    std::cout << "Passed:       " << result.passed << "\n";
    std::cout << "Failed:       " << result.failed << "\n";
    std::cout << "Crashes:      " << result.crashes << "\n";
    std::cout << "Slow (>2s):   " << result.slowQueries << "\n";

    if (!result.failedQueries.empty()) {
        int show = static_cast<int>(result.failedQueries.size());
        if (show > 20) show = 20;   // Cap display
        std::cout << "\nFailed queries (first " << show << "):\n";
        for (int i = 0; i < show; i++)
            std::cout << "  " << result.failedQueries[static_cast<size_t>(i)] << "\n";
    }

    // Exit 1 only if there were actual crashes (exceptions that bubbled up)
    return result.crashes > 0 ? 1 : 0;
}
