// ============================================================
// tpch_benchmark.cpp — MilanSQL TPC-H Benchmark (Phase 101)
// Scale Factor 0.01 — synthetic data for fast CI
// ============================================================

#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <stdexcept>
#include <iomanip>

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
        default:
            break;
    }
}

// ── Timer helper ─────────────────────────────────────────────
struct Timer {
    std::chrono::steady_clock::time_point t0;
    Timer() : t0(std::chrono::steady_clock::now()) {}
    double elapsedMs() const {
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
};

// ── Schema setup ─────────────────────────────────────────────
static void createSchema(Engine& engine, Parser& parser) {
    execSQL(engine, parser,
        "CREATE TABLE region (r_regionkey INT, r_name TEXT, r_comment TEXT)");
    execSQL(engine, parser,
        "CREATE TABLE nation (n_nationkey INT, n_name TEXT, n_regionkey INT, n_comment TEXT)");
    execSQL(engine, parser,
        "CREATE TABLE supplier (s_suppkey INT, s_name TEXT, s_nationkey INT, s_acctbal REAL)");
    execSQL(engine, parser,
        "CREATE TABLE customer (c_custkey INT, c_name TEXT, c_nationkey INT, c_acctbal REAL, c_mktsegment TEXT)");
    execSQL(engine, parser,
        "CREATE TABLE orders (o_orderkey INT, o_custkey INT, o_orderstatus TEXT, o_totalprice REAL, o_orderdate TEXT, o_orderpriority TEXT)");
    execSQL(engine, parser,
        "CREATE TABLE lineitem (l_orderkey INT, l_partkey INT, l_suppkey INT, l_linenumber INT, l_quantity REAL, l_extendedprice REAL, l_discount REAL, l_tax REAL, l_returnflag TEXT, l_linestatus TEXT, l_shipdate TEXT, l_receiptdate TEXT)");
    execSQL(engine, parser,
        "CREATE TABLE part (p_partkey INT, p_name TEXT, p_type TEXT, p_size INT, p_retailprice REAL)");
    execSQL(engine, parser,
        "CREATE TABLE partsupp (ps_partkey INT, ps_suppkey INT, ps_availqty INT, ps_supplycost REAL)");
}

// ── Data generation (SF 0.01) ─────────────────────────────────
static int generateData(Engine& engine, Parser& parser) {
    int totalRows = 0;

    // region: 5 rows
    std::vector<std::vector<std::string>> regionData = {
        {"0","AFRICA","lar deposits. blithely final packages cajole. regular waters are final requests. regular accounts"},
        {"1","AMERICA","hs use ironic, even requests. s"},
        {"2","ASIA","ges. thinly even pinto beans ca"},
        {"3","EUROPE","ly final courts cajole furiously final excuse"},
        {"4","MIDDLE EAST","uickly special accounts cajole carefully blithely close requests."}
    };
    for (auto& r : regionData) {
        execSQL(engine, parser,
            "INSERT INTO region VALUES (" + r[0] + ", '" + r[1] + "', '" + r[2] + "')");
        totalRows++;
    }

    // nation: 5 rows
    std::vector<std::vector<std::string>> nationData = {
        {"0","ALGERIA","0"},
        {"1","ARGENTINA","1"},
        {"2","BRAZIL","1"},
        {"3","CANADA","1"},
        {"4","EGYPT","4"}
    };
    for (auto& n : nationData) {
        execSQL(engine, parser,
            "INSERT INTO nation VALUES (" + n[0] + ", '" + n[1] + "', " + n[2] + ", 'comment')");
        totalRows++;
    }

    // supplier: 5 rows
    std::vector<std::vector<std::string>> supplierData = {
        {"1","Supplier#001","0","5755.94"},
        {"2","Supplier#002","1","4032.68"},
        {"3","Supplier#003","2","4192.40"},
        {"4","Supplier#004","3","7367.05"},
        {"5","Supplier#005","4","6820.35"}
    };
    for (auto& s : supplierData) {
        execSQL(engine, parser,
            "INSERT INTO supplier VALUES (" + s[0] + ", '" + s[1] + "', " + s[2] + ", " + s[3] + ")");
        totalRows++;
    }

    // customer: 15 rows
    std::vector<std::vector<std::string>> customerData = {
        {"1","Customer#001","0","711.56","BUILDING"},
        {"2","Customer#002","1","121.65","AUTOMOBILE"},
        {"3","Customer#003","2","7498.12","AUTOMOBILE"},
        {"4","Customer#004","3","2866.83","MACHINERY"},
        {"5","Customer#005","4","794.47","BUILDING"},
        {"6","Customer#006","0","7638.57","AUTOMOBILE"},
        {"7","Customer#007","1","9561.95","MACHINERY"},
        {"8","Customer#008","2","6819.74","BUILDING"},
        {"9","Customer#009","3","8324.07","FURNITURE"},
        {"10","Customer#010","4","2753.54","BUILDING"},
        {"11","Customer#011","0","5121.28","HOUSEHOLD"},
        {"12","Customer#012","1","3332.02","HOUSEHOLD"},
        {"13","Customer#013","2","8741.01","BUILDING"},
        {"14","Customer#014","3","1234.56","AUTOMOBILE"},
        {"15","Customer#015","4","9876.54","FURNITURE"}
    };
    for (auto& c : customerData) {
        execSQL(engine, parser,
            "INSERT INTO customer VALUES (" + c[0] + ", '" + c[1] + "', " +
            c[2] + ", " + c[3] + ", '" + c[4] + "')");
        totalRows++;
    }

    // orders: 15 rows
    std::vector<std::vector<std::string>> ordersData = {
        {"1","1","O","173665.47","1993-01-29","5-LOW"},
        {"2","2","P","46929.18","1996-12-01","1-URGENT"},
        {"3","3","F","193846.25","1993-10-14","5-LOW"},
        {"4","4","O","32151.78","1995-10-11","5-LOW"},
        {"5","5","F","144659.20","1994-07-30","5-LOW"},
        {"6","6","F","58749.59","1992-02-21","4-NOT SPECIFIED"},
        {"7","7","O","252004.18","1996-01-10","2-HIGH"},
        {"8","8","P","208660.75","1995-07-16","2-HIGH"},
        {"9","9","F","112186.56","1991-10-21","3-MEDIUM"},
        {"10","10","F","65522.46","1995-07-21","1-URGENT"},
        {"11","11","O","19455.06","1993-09-14","5-LOW"},
        {"12","12","F","31368.02","1992-06-24","1-URGENT"},
        {"13","13","P","40789.33","1993-12-29","3-MEDIUM"},
        {"14","14","F","91490.97","1994-02-02","5-LOW"},
        {"15","15","O","75694.16","1996-08-07","2-HIGH"}
    };
    for (auto& o : ordersData) {
        execSQL(engine, parser,
            "INSERT INTO orders VALUES (" + o[0] + ", " + o[1] + ", '" +
            o[2] + "', " + o[3] + ", '" + o[4] + "', '" + o[5] + "')");
        totalRows++;
    }

    // lineitem: 60 rows (4 per order)
    // l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_receiptdate
    struct LI { int okey, pkey, skey, lnum; double qty, ep, disc, tax; const char *rf, *ls, *ship, *recv; };
    std::vector<LI> liData = {
        {1,1,1,1, 17,24386.67,0.04,0.02,"N","O","1996-03-13","1996-04-12"},
        {1,2,2,2, 36,45983.16,0.09,0.06,"N","O","1996-04-12","1996-04-20"},
        {1,3,3,3, 8, 13309.60,0.10,0.02,"N","O","1996-01-29","1996-03-05"},
        {1,4,4,4, 28,28955.64,0.09,0.06,"N","O","1996-04-21","1996-05-16"},
        {2,5,5,1, 38,44694.46,0.00,0.05,"N","O","1997-01-28","1997-01-31"},
        {2,1,1,2, 45,54182.40,0.06,0.00,"R","F","1992-02-21","1994-01-28"},
        {2,2,2,3, 25,7532.75, 0.10,0.04,"A","F","1994-08-08","1994-08-20"},
        {2,3,3,4, 4, 7662.12, 0.10,0.00,"A","F","1994-07-16","1994-07-20"},
        {3,4,4,1, 45,54182.40,0.06,0.00,"R","F","1993-11-22","1994-01-22"},
        {3,5,5,2, 25,7532.75, 0.10,0.04,"A","F","1993-12-29","1994-01-10"},
        {3,1,1,3, 4, 7662.12, 0.10,0.00,"A","F","1994-01-10","1994-01-15"},
        {3,2,2,4, 12,13582.56,0.08,0.02,"N","O","1996-02-12","1996-03-10"},
        {4,3,3,1, 32,45983.16,0.09,0.06,"N","O","1995-10-23","1995-11-15"},
        {4,4,4,2, 18,24386.67,0.04,0.02,"N","O","1995-11-01","1995-11-30"},
        {4,5,5,3, 22,28955.64,0.09,0.06,"N","O","1995-12-01","1996-01-10"},
        {4,1,1,4, 40,54182.40,0.06,0.00,"N","O","1996-01-15","1996-02-25"},
        {5,2,2,1, 15,7532.75, 0.10,0.04,"A","F","1994-08-08","1994-08-20"},
        {5,3,3,2, 26,7662.12, 0.10,0.00,"A","F","1994-07-16","1994-07-20"},
        {5,4,4,3, 50,54182.40,0.06,0.00,"R","F","1994-09-10","1994-10-10"},
        {5,5,5,4, 30,24386.67,0.04,0.02,"N","F","1994-10-15","1994-11-01"},
        {6,1,1,1, 21,45983.16,0.09,0.06,"N","O","1992-04-27","1992-05-12"},
        {6,2,2,2, 8, 28955.64,0.09,0.06,"N","O","1992-05-01","1992-05-30"},
        {6,3,3,3, 43,13582.56,0.08,0.02,"N","O","1992-06-15","1992-07-10"},
        {6,4,4,4, 35,7662.12, 0.10,0.00,"N","O","1992-07-25","1992-08-20"},
        {7,5,5,1, 28,24386.67,0.04,0.02,"N","O","1996-02-12","1996-03-10"},
        {7,1,1,2, 14,13309.60,0.10,0.02,"N","O","1996-03-01","1996-03-20"},
        {7,2,2,3, 45,54182.40,0.06,0.00,"N","O","1996-03-15","1996-04-05"},
        {7,3,3,4, 36,45983.16,0.09,0.06,"N","O","1996-04-01","1996-04-25"},
        {8,4,4,1, 22,28955.64,0.09,0.06,"N","O","1995-07-20","1995-08-15"},
        {8,5,5,2, 17,7532.75, 0.10,0.04,"N","O","1995-08-01","1995-08-20"},
        {8,1,1,3, 12,7662.12, 0.10,0.00,"N","O","1995-08-15","1995-09-01"},
        {8,2,2,4, 31,54182.40,0.06,0.00,"N","O","1995-09-01","1995-09-20"},
        {9,3,3,1, 42,45983.16,0.09,0.06,"A","F","1991-11-10","1991-12-10"},
        {9,4,4,2, 8, 13309.60,0.10,0.02,"A","F","1991-12-01","1991-12-20"},
        {9,5,5,3, 15,24386.67,0.04,0.02,"R","F","1991-12-15","1992-01-05"},
        {9,1,1,4, 29,13582.56,0.08,0.02,"R","F","1992-01-10","1992-01-25"},
        {10,2,2,1, 38,28955.64,0.09,0.06,"A","F","1995-07-25","1995-08-10"},
        {10,3,3,2, 44,54182.40,0.06,0.00,"R","F","1995-08-05","1995-08-25"},
        {10,4,4,3, 19,7532.75, 0.10,0.04,"A","F","1995-08-20","1995-09-10"},
        {10,5,5,4, 7, 7662.12, 0.10,0.00,"R","F","1995-09-05","1995-09-20"},
        {11,1,1,1, 33,45983.16,0.09,0.06,"N","O","1993-09-20","1993-10-10"},
        {11,2,2,2, 10,28955.64,0.09,0.06,"N","O","1993-10-05","1993-10-25"},
        {11,3,3,3, 24,54182.40,0.06,0.00,"N","O","1993-10-20","1993-11-10"},
        {11,4,4,4, 16,13309.60,0.10,0.02,"N","O","1993-11-01","1993-11-20"},
        {12,5,5,1, 27,24386.67,0.04,0.02,"A","F","1992-07-01","1992-07-20"},
        {12,1,1,2, 11,13582.56,0.08,0.02,"A","F","1992-07-15","1992-08-05"},
        {12,2,2,3, 47,54182.40,0.06,0.00,"R","F","1992-08-01","1992-08-20"},
        {12,3,3,4, 9, 7662.12, 0.10,0.00,"R","F","1992-08-15","1992-09-05"},
        {13,4,4,1, 20,45983.16,0.09,0.06,"N","O","1993-12-30","1994-01-20"},
        {13,5,5,2, 34,28955.64,0.09,0.06,"N","O","1994-01-10","1994-01-30"},
        {13,1,1,3, 48,13309.60,0.10,0.02,"N","O","1994-01-20","1994-02-10"},
        {13,2,2,4, 6, 7532.75, 0.10,0.04,"N","O","1994-02-01","1994-02-20"},
        {14,3,3,1, 39,54182.40,0.06,0.00,"A","F","1994-02-10","1994-03-01"},
        {14,4,4,2, 25,45983.16,0.09,0.06,"R","F","1994-02-25","1994-03-15"},
        {14,5,5,3, 13,24386.67,0.04,0.02,"A","F","1994-03-10","1994-03-30"},
        {14,1,1,4, 41,13582.56,0.08,0.02,"R","F","1994-03-25","1994-04-15"},
        {15,2,2,1, 23,28955.64,0.09,0.06,"N","O","1996-08-15","1996-09-05"},
        {15,3,3,2, 37,45983.16,0.09,0.06,"N","O","1996-09-01","1996-09-25"},
        {15,4,4,3, 5, 13309.60,0.10,0.02,"N","O","1996-09-15","1996-10-05"},
        {15,5,5,4, 49,54182.40,0.06,0.00,"N","O","1996-10-01","1996-10-20"}
    };
    for (auto& li : liData) {
        execSQL(engine, parser,
            "INSERT INTO lineitem VALUES (" +
            std::to_string(li.okey) + ", " +
            std::to_string(li.pkey) + ", " +
            std::to_string(li.skey) + ", " +
            std::to_string(li.lnum) + ", " +
            std::to_string(li.qty) + ", " +
            std::to_string(li.ep) + ", " +
            std::to_string(li.disc) + ", " +
            std::to_string(li.tax) + ", '" +
            li.rf + "', '" + li.ls + "', '" +
            li.ship + "', '" + li.recv + "')");
        totalRows++;
    }

    // part: 10 rows
    std::vector<std::vector<std::string>> partData = {
        {"1","goldenrod lace spring peru powder","PROMO BURNISHED COPPER","7","901.00"},
        {"2","blush thistle blue yellow saddle","LARGE BRUSHED BRASS","1","902.00"},
        {"3","dark green antique puff wheat","STANDARD POLISHED BRASS","21","903.00"},
        {"4","floral forest bisque hot chocolate","SMALL PLATED BRASS","14","904.00"},
        {"5","forest blush chiffon thistle chocolate","STANDARD POLISHED TIN","15","905.00"},
        {"6","bisque spring wheat lace yellow","LARGE BURNISHED COPPER","4","906.00"},
        {"7","antique salmon wheat white blush","SMALL PLATED COPPER","45","907.00"},
        {"8","moccasin goldenrod chartreuse rose smoke","LARGE PLATED COPPER","41","908.00"},
        {"9","cornflower turquoise maroon sandy red","MEDIUM PLATED COPPER","17","909.00"},
        {"10","khaki frosted aquamarine rosy blanched","LARGE PLATED STEEL","28","910.00"}
    };
    for (auto& p : partData) {
        execSQL(engine, parser,
            "INSERT INTO part VALUES (" + p[0] + ", '" + p[1] + "', '" +
            p[2] + "', " + p[3] + ", " + p[4] + ")");
        totalRows++;
    }

    // partsupp: 10 rows
    std::vector<std::vector<std::string>> partsuppData = {
        {"1","2","3325","771.64"},
        {"1","5","8076","993.49"},
        {"2","3","3956","337.09"},
        {"2","4","4069","357.84"},
        {"3","1","8895","378.49"},
        {"3","2","4798","131.60"},
        {"4","3","6882","645.40"},
        {"4","4","5765","615.37"},
        {"5","1","5765","336.21"},
        {"5","2","8399","831.39"}
    };
    for (auto& ps : partsuppData) {
        execSQL(engine, parser,
            "INSERT INTO partsupp VALUES (" + ps[0] + ", " + ps[1] + ", " +
            ps[2] + ", " + ps[3] + ")");
        totalRows++;
    }

    return totalRows;
}

// ── Query helpers ─────────────────────────────────────────────

static SelectItem aggItem(const std::string& func, const std::string& col,
                          const std::string& alias = "") {
    SelectItem si;
    si.isAgg = true;
    si.aggFunc = func;
    si.aggCol = col;
    si.alias = alias;
    return si;
}

static SelectItem colItem(const std::string& col, const std::string& alias = "") {
    SelectItem si;
    si.colName = col;
    si.alias = alias;
    return si;
}

// ── Q1: Pricing Summary Report (simplified) ───────────────────
// GROUP BY l_returnflag, l_linestatus — SUM qty, SUM ep, AVG disc, COUNT
static size_t runQ1(Engine& engine) {
    std::vector<SelectItem> items = {
        colItem("l_returnflag"),
        colItem("l_linestatus"),
        aggItem("SUM", "l_quantity",      "sum_qty"),
        aggItem("SUM", "l_extendedprice", "sum_base_price"),
        aggItem("AVG", "l_discount",      "avg_discount"),
        aggItem("COUNT", "*",             "count_order")
    };

    WhereCondition wc("l_shipdate", "<=", "1995-09-01");
    auto result = engine.groupBy("lineitem",
        {wc}, "AND",
        {"l_returnflag", "l_linestatus"},
        items,
        {}, "AND");
    return result.rowCount();
}

// ── Q3: Shipping Priority (simplified) ───────────────────────
// customer JOIN orders ON c_custkey=o_custkey
//          JOIN lineitem ON o_orderkey=l_orderkey
// WHERE c_mktsegment='BUILDING' AND o_orderdate<'1995-03-15'
// Group by l_orderkey, o_orderdate
// Strategy: pre-filter individual tables, then join (avoids schema-qualified column issues)
static size_t runQ3(Engine& engine) {
    // Pre-filter customer WHERE c_mktsegment='BUILDING'
    WhereCondition wc1("c_mktsegment", "=", "BUILDING");
    auto custFiltered = engine.selectWhere("customer", {wc1}, "AND").table;

    // Pre-filter orders WHERE o_orderdate < '1995-03-15'
    WhereCondition wo1("o_orderdate", "<", "1995-03-15");
    auto ordFiltered = engine.selectWhere("orders", {wo1}, "AND").table;

    // Count matching rows across the join manually by iterating
    // customer -> orders (on c_custkey=o_custkey) -> lineitem (on o_orderkey=l_orderkey)
    // Find column indices
    int custKeyIdx = -1, ordCustIdx = -1, ordKeyIdx = -1, liOrdIdx = -1;
    for (int i = 0; i < static_cast<int>(custFiltered.columns().size()); ++i)
        if (custFiltered.columns()[static_cast<size_t>(i)].name == "c_custkey") { custKeyIdx = i; break; }
    for (int i = 0; i < static_cast<int>(ordFiltered.columns().size()); ++i) {
        if (ordFiltered.columns()[static_cast<size_t>(i)].name == "o_custkey") ordCustIdx = i;
        if (ordFiltered.columns()[static_cast<size_t>(i)].name == "o_orderkey") ordKeyIdx = i;
    }
    const auto& lineitem = engine.selectAll("lineitem");
    for (int i = 0; i < static_cast<int>(lineitem.columns().size()); ++i)
        if (lineitem.columns()[static_cast<size_t>(i)].name == "l_orderkey") { liOrdIdx = i; break; }

    if (custKeyIdx < 0 || ordCustIdx < 0 || ordKeyIdx < 0 || liOrdIdx < 0) return 0;

    size_t count = 0;
    for (const auto& crow : custFiltered.rows()) {
        const std::string& ck = crow.values[static_cast<size_t>(custKeyIdx)];
        for (const auto& orow : ordFiltered.rows()) {
            if (orow.values[static_cast<size_t>(ordCustIdx)] != ck) continue;
            const std::string& ok = orow.values[static_cast<size_t>(ordKeyIdx)];
            for (const auto& lrow : lineitem.rows()) {
                if (lrow.xmax != 0) continue;
                if (lrow.values[static_cast<size_t>(liOrdIdx)] == ok) count++;
            }
        }
    }
    return count;
}

// ── Q5: Local Supplier Volume (simplified) ────────────────────
// customer JOIN orders JOIN lineitem JOIN supplier JOIN nation
// WHERE o_orderdate in [1994-01-01, 1995-01-01)
// Returns joined row count
static size_t runQ5(Engine& engine) {
    // Pre-filter orders by date range
    WhereCondition wo1("o_orderdate", ">=", "1994-01-01");
    WhereCondition wo2("o_orderdate", "<",  "1995-01-01");
    auto ordFiltered = engine.selectWhere("orders", {wo1, wo2}, "AND").table;

    const auto& customer  = engine.selectAll("customer");
    const auto& lineitem  = engine.selectAll("lineitem");
    const auto& supplier  = engine.selectAll("supplier");
    const auto& nation    = engine.selectAll("nation");

    // Find column indices
    auto findIdx = [](const Table& t, const std::string& col) -> int {
        for (int i = 0; i < static_cast<int>(t.columns().size()); ++i)
            if (t.columns()[static_cast<size_t>(i)].name == col) return i;
        return -1;
    };

    int cCustKey = findIdx(customer,    "c_custkey");
    int oCustKey = findIdx(ordFiltered, "o_custkey");
    int oOrdKey  = findIdx(ordFiltered, "o_orderkey");
    int lOrdKey  = findIdx(lineitem,    "l_orderkey");
    int lSuppKey = findIdx(lineitem,    "l_suppkey");
    int sSuppKey = findIdx(supplier,    "s_suppkey");
    int sNatKey  = findIdx(supplier,    "s_nationkey");
    int nNatKey  = findIdx(nation,      "n_nationkey");

    if (cCustKey<0||oCustKey<0||oOrdKey<0||lOrdKey<0||
        lSuppKey<0||sSuppKey<0||sNatKey<0||nNatKey<0) return 0;

    size_t count = 0;
    for (const auto& crow : customer.rows()) {
        if (crow.xmax != 0) continue;
        const std::string& ck = crow.values[static_cast<size_t>(cCustKey)];
        for (const auto& orow : ordFiltered.rows()) {
            if (orow.values[static_cast<size_t>(oCustKey)] != ck) continue;
            const std::string& ok = orow.values[static_cast<size_t>(oOrdKey)];
            for (const auto& lrow : lineitem.rows()) {
                if (lrow.xmax != 0) continue;
                if (lrow.values[static_cast<size_t>(lOrdKey)] != ok) continue;
                const std::string& sk = lrow.values[static_cast<size_t>(lSuppKey)];
                for (const auto& srow : supplier.rows()) {
                    if (srow.xmax != 0) continue;
                    if (srow.values[static_cast<size_t>(sSuppKey)] != sk) continue;
                    const std::string& nk = srow.values[static_cast<size_t>(sNatKey)];
                    for (const auto& nrow : nation.rows()) {
                        if (nrow.xmax != 0) continue;
                        if (nrow.values[static_cast<size_t>(nNatKey)] == nk) count++;
                    }
                }
            }
        }
    }
    return count;
}

// ── Q6: Forecasting Revenue Change ────────────────────────────
// Simple filter + SUM (no JOIN)
// SUM(l_extendedprice * l_discount) WHERE ...
static size_t runQ6(Engine& engine) {
    WhereCondition w1("l_shipdate", ">=", "1994-01-01");
    WhereCondition w2("l_shipdate", "<",  "1995-01-01");
    WhereCondition w3("l_discount", ">=", "0.05");
    WhereCondition w4("l_discount", "<=", "0.07");
    WhereCondition w5("l_quantity", "<",  "24");

    auto result = engine.selectWhere("lineitem",
        {w1, w2, w3, w4, w5}, "AND");
    return result.table.rowCount();
}

// ── main ──────────────────────────────────────────────────────
int main() {
    std::cout << "=== TPC-H Benchmark (Scale Factor 0.01) ===\n\n";

    Engine engine;
    Parser parser;

    // Setup schema
    std::cout << "Creating schema...\n";
    try {
        createSchema(engine, parser);
    } catch (const std::exception& ex) {
        std::cerr << "Schema creation failed: " << ex.what() << "\n";
        return 1;
    }

    // Generate data
    std::cout << "Generating data...\n";
    int totalRows = 0;
    {
        Timer t;
        try {
            totalRows = generateData(engine, parser);
        } catch (const std::exception& ex) {
            std::cerr << "Data generation failed: " << ex.what() << "\n";
            return 1;
        }
        double ms = t.elapsedMs();
        std::cout << "  Loaded " << totalRows << " rows in "
                  << std::fixed << std::setprecision(1) << ms << " ms\n\n";
    }

    // Print table row counts
    std::cout << "Table sizes:\n";
    for (const char* const& tbl : {"region","nation","supplier","customer",
                                    "orders","lineitem","part","partsupp"}) {
        try {
            const auto& t = engine.selectAll(tbl);
            std::cout << "  " << std::left << std::setw(12) << tbl
                      << " " << t.rowCount() << " rows\n";
        } catch (...) {}
    }
    std::cout << "\n";

    double totalMs = 0.0;
    struct QueryResult { std::string name; double ms; size_t rows; bool ok; };
    std::vector<QueryResult> qresults;

    // ── Q1 ──────────────────────────────────────────────────────
    {
        std::cout << "Q1  Pricing Summary Report (GROUP BY l_returnflag, l_linestatus)...\n";
        Timer t;
        size_t rows = 0;
        bool ok = true;
        try {
            rows = runQ1(engine);
        } catch (const std::exception& ex) {
            std::cerr << "  ERROR: " << ex.what() << "\n";
            ok = false;
        }
        double ms = t.elapsedMs();
        totalMs += ms;
        if (ok)
            std::cout << "  Result: " << rows << " rows | " << ms << " ms\n\n";
        qresults.push_back({"Q1", ms, rows, ok});
    }

    // ── Q3 ──────────────────────────────────────────────────────
    {
        std::cout << "Q3  Shipping Priority (3-table JOIN customer+orders+lineitem)...\n";
        Timer t;
        size_t rows = 0;
        bool ok = true;
        try {
            rows = runQ3(engine);
        } catch (const std::exception& ex) {
            std::cerr << "  ERROR: " << ex.what() << "\n";
            ok = false;
        }
        double ms = t.elapsedMs();
        totalMs += ms;
        if (ok)
            std::cout << "  Result: " << rows << " rows | " << ms << " ms\n\n";
        qresults.push_back({"Q3", ms, rows, ok});
    }

    // ── Q5 ──────────────────────────────────────────────────────
    {
        std::cout << "Q5  Local Supplier Volume (5-table JOIN)...\n";
        Timer t;
        size_t rows = 0;
        bool ok = true;
        try {
            rows = runQ5(engine);
        } catch (const std::exception& ex) {
            std::cerr << "  ERROR: " << ex.what() << "\n";
            ok = false;
        }
        double ms = t.elapsedMs();
        totalMs += ms;
        if (ok)
            std::cout << "  Result: " << rows << " rows | " << ms << " ms\n\n";
        qresults.push_back({"Q5", ms, rows, ok});
    }

    // ── Q6 ──────────────────────────────────────────────────────
    {
        std::cout << "Q6  Forecasting Revenue Change (filter + SUM)...\n";
        Timer t;
        size_t rows = 0;
        bool ok = true;
        try {
            rows = runQ6(engine);
        } catch (const std::exception& ex) {
            std::cerr << "  ERROR: " << ex.what() << "\n";
            ok = false;
        }
        double ms = t.elapsedMs();
        totalMs += ms;
        if (ok)
            std::cout << "  Result: " << rows << " rows | " << ms << " ms\n\n";
        qresults.push_back({"Q6", ms, rows, ok});
    }

    // ── Summary ─────────────────────────────────────────────────
    std::cout << "=== TPC-H Summary ===\n";
    for (auto& qr : qresults) {
        std::cout << "  " << std::left << std::setw(4) << qr.name
                  << "  " << (qr.ok ? "PASS" : "FAIL")
                  << "  rows=" << std::setw(6) << qr.rows
                  << "  time=" << std::fixed << std::setprecision(2)
                  << qr.ms << " ms\n";
    }
    if (totalMs > 0.0) {
        double score = 4000.0 / totalMs;
        std::cout << "\nTotal time:  " << std::fixed << std::setprecision(2)
                  << totalMs << " ms\n";
        std::cout << "Composite score (4000/total_ms): "
                  << std::fixed << std::setprecision(2) << score << "\n";
    }

    return 0;
}
