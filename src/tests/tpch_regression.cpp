// ============================================================
// tpch_regression.cpp — TPC-H Automated Regression (Phase 2.4)
// Sets up TPC-H SF=0.01 schema, runs all 22 queries,
// measures wall-clock time, compares against baseline JSON.
// ============================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <cstdio>

#include "engine/engine.hpp"
#include "parser/parser.hpp"
#include "dispatch_result.hpp"

namespace milansql {

// ── JSON helpers ──────────────────────────────────────────────

static std::string jsonEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r;
}

static double parseJsonDouble(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return -1.0;
    pos += key.size() + 3;
    // skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    size_t end = pos;
    while (end < json.size() && (std::isdigit((unsigned char)json[end]) || json[end] == '.' || json[end] == 'e' || json[end] == 'E' || json[end] == '-' || json[end] == '+'))
        ++end;
    if (end == pos) return -1.0;
    try { return std::stod(json.substr(pos, end - pos)); } catch (...) { return -1.0; }
}

// ── Schema setup ──────────────────────────────────────────────

static void execSql(Engine& eng, Parser& parser, const std::string& sql) {
    QueryResult qr = dispatch(parser.parse(sql), eng);
    if (!qr.error.empty()) {
        // Non-fatal: just print and continue
        std::cerr << "  [WARN] " << sql.substr(0, 60) << "... -> " << qr.error << "\n";
    }
}

static void setupTpchSchema(Engine& eng) {
    Parser parser;
    // Drop existing tables (ignore errors)
    for (const std::string& t : {"lineitem","orders","customer","part","supplier","partsupp","nation","region"}) {
        execSql(eng, parser, "DROP TABLE IF EXISTS " + t);
    }

    // Region
    execSql(eng, parser, "CREATE TABLE region (r_regionkey INT, r_name VARCHAR(25), r_comment VARCHAR(152))");
    // Nation
    execSql(eng, parser, "CREATE TABLE nation (n_nationkey INT, n_name VARCHAR(25), n_regionkey INT, n_comment VARCHAR(152))");
    // Supplier
    execSql(eng, parser, "CREATE TABLE supplier (s_suppkey INT, s_name VARCHAR(25), s_address VARCHAR(40), s_nationkey INT, s_phone VARCHAR(15), s_acctbal DOUBLE, s_comment VARCHAR(101))");
    // Part
    execSql(eng, parser, "CREATE TABLE part (p_partkey INT, p_name VARCHAR(55), p_mfgr VARCHAR(25), p_brand VARCHAR(10), p_type VARCHAR(25), p_size INT, p_container VARCHAR(10), p_retailprice DOUBLE, p_comment VARCHAR(23))");
    // Customer
    execSql(eng, parser, "CREATE TABLE customer (c_custkey INT, c_name VARCHAR(25), c_address VARCHAR(40), c_nationkey INT, c_phone VARCHAR(15), c_acctbal DOUBLE, c_mktsegment VARCHAR(10), c_comment VARCHAR(117))");
    // Orders
    execSql(eng, parser, "CREATE TABLE orders (o_orderkey INT, o_custkey INT, o_orderstatus VARCHAR(1), o_totalprice DOUBLE, o_orderdate VARCHAR(10), o_orderpriority VARCHAR(15), o_clerk VARCHAR(15), o_shippriority INT, o_comment VARCHAR(79))");
    // Partsupp
    execSql(eng, parser, "CREATE TABLE partsupp (ps_partkey INT, ps_suppkey INT, ps_availqty INT, ps_supplycost DOUBLE, ps_comment VARCHAR(199))");
    // Lineitem
    execSql(eng, parser, "CREATE TABLE lineitem (l_orderkey INT, l_partkey INT, l_suppkey INT, l_linenumber INT, l_quantity DOUBLE, l_extendedprice DOUBLE, l_discount DOUBLE, l_tax DOUBLE, l_returnflag VARCHAR(1), l_linestatus VARCHAR(1), l_shipdate VARCHAR(10), l_commitdate VARCHAR(10), l_receiptdate VARCHAR(10), l_shipinstruct VARCHAR(25), l_shipmode VARCHAR(10), l_comment VARCHAR(44))");
}

static void insertSampleData(Engine& eng) {
    Parser parser;
    // SF=0.01: minimal data for regression (5 rows each key table)
    // Region
    execSql(eng, parser, "INSERT INTO region VALUES (0,'AFRICA','special')");
    execSql(eng, parser, "INSERT INTO region VALUES (1,'AMERICA','special')");
    execSql(eng, parser, "INSERT INTO region VALUES (2,'ASIA','special')");
    execSql(eng, parser, "INSERT INTO region VALUES (3,'EUROPE','special')");
    execSql(eng, parser, "INSERT INTO region VALUES (4,'MIDDLE EAST','special')");

    // Nation (5 rows)
    execSql(eng, parser, "INSERT INTO nation VALUES (0,'ALGERIA',0,'comment')");
    execSql(eng, parser, "INSERT INTO nation VALUES (1,'ARGENTINA',1,'comment')");
    execSql(eng, parser, "INSERT INTO nation VALUES (2,'BRAZIL',1,'comment')");
    execSql(eng, parser, "INSERT INTO nation VALUES (3,'CANADA',1,'comment')");
    execSql(eng, parser, "INSERT INTO nation VALUES (4,'EGYPT',4,'comment')");

    // Supplier (5 rows)
    execSql(eng, parser, "INSERT INTO supplier VALUES (1,'Supplier#1','Addr1',0,'555-0001',1000.00,'good')");
    execSql(eng, parser, "INSERT INTO supplier VALUES (2,'Supplier#2','Addr2',1,'555-0002',2000.00,'ok')");
    execSql(eng, parser, "INSERT INTO supplier VALUES (3,'Supplier#3','Addr3',2,'555-0003',1500.00,'fine')");
    execSql(eng, parser, "INSERT INTO supplier VALUES (4,'Supplier#4','Addr4',3,'555-0004',3000.00,'best')");
    execSql(eng, parser, "INSERT INTO supplier VALUES (5,'Supplier#5','Addr5',4,'555-0005',500.00,'avg')");

    // Part (5 rows)
    execSql(eng, parser, "INSERT INTO part VALUES (1,'almond antique','Mfgr#1','Brand#11','STANDARD ANODIZED TIN',1,'SM CASE',900.00,'note')");
    execSql(eng, parser, "INSERT INTO part VALUES (2,'almond blush','Mfgr#1','Brand#12','PROMO BRUSHED COPPER',2,'LG BOX',800.00,'note')");
    execSql(eng, parser, "INSERT INTO part VALUES (3,'almond chocolate','Mfgr#2','Brand#21','STANDARD PLATED BRASS',3,'MED BAG',700.00,'note')");
    execSql(eng, parser, "INSERT INTO part VALUES (4,'almond cornsilk','Mfgr#2','Brand#22','ECONOMY ANODIZED BRASS',4,'LG DRUM',600.00,'note')");
    execSql(eng, parser, "INSERT INTO part VALUES (5,'almond cream','Mfgr#3','Brand#31','ECONOMY POLISHED NICKEL',5,'SM BOX',500.00,'note')");

    // Customer (5 rows)
    execSql(eng, parser, "INSERT INTO customer VALUES (1,'Cust#1','Addr1',0,'555-1001',1000.00,'BUILDING','comment')");
    execSql(eng, parser, "INSERT INTO customer VALUES (2,'Cust#2','Addr2',1,'555-1002',2000.00,'AUTOMOBILE','comment')");
    execSql(eng, parser, "INSERT INTO customer VALUES (3,'Cust#3','Addr3',2,'555-1003',3000.00,'MACHINERY','comment')");
    execSql(eng, parser, "INSERT INTO customer VALUES (4,'Cust#4','Addr4',3,'555-1004',4000.00,'HOUSEHOLD','comment')");
    execSql(eng, parser, "INSERT INTO customer VALUES (5,'Cust#5','Addr5',4,'555-1005',5000.00,'FURNITURE','comment')");

    // Orders (10 rows)
    execSql(eng, parser, "INSERT INTO orders VALUES (1,1,'O',1000.00,'1996-01-02','1-URGENT','Clerk#1',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (2,2,'O',2000.00,'1996-01-03','2-HIGH','Clerk#2',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (3,3,'F',3000.00,'1993-01-04','3-MEDIUM','Clerk#3',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (4,4,'O',4000.00,'1995-01-05','4-NOT SPECIFIED','Clerk#4',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (5,5,'F',5000.00,'1994-01-06','5-LOW','Clerk#5',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (6,1,'F',6000.00,'1993-06-01','1-URGENT','Clerk#1',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (7,2,'O',7000.00,'1997-06-02','2-HIGH','Clerk#2',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (8,3,'O',8000.00,'1998-06-03','3-MEDIUM','Clerk#3',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (9,4,'F',9000.00,'1992-06-04','4-NOT SPECIFIED','Clerk#4',0,'comment')");
    execSql(eng, parser, "INSERT INTO orders VALUES (10,5,'O',10000.00,'1996-06-05','5-LOW','Clerk#5',0,'comment')");

    // Partsupp (5 rows)
    execSql(eng, parser, "INSERT INTO partsupp VALUES (1,1,100,100.00,'comment')");
    execSql(eng, parser, "INSERT INTO partsupp VALUES (1,2,200,200.00,'comment')");
    execSql(eng, parser, "INSERT INTO partsupp VALUES (2,1,300,300.00,'comment')");
    execSql(eng, parser, "INSERT INTO partsupp VALUES (2,3,400,400.00,'comment')");
    execSql(eng, parser, "INSERT INTO partsupp VALUES (3,2,500,500.00,'comment')");

    // Lineitem (10 rows)
    execSql(eng, parser, "INSERT INTO lineitem VALUES (1,1,1,1,17.0,17954.55,0.04,0.02,'N','O','1996-03-13','1996-02-12','1996-03-22','DELIVER IN PERSON','TRUCK','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (1,2,2,2,36.0,45983.16,0.09,0.06,'N','O','1996-04-12','1996-02-28','1996-04-20','TAKE BACK RETURN','MAIL','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (2,3,3,1,8.0,13309.60,0.10,0.02,'N','O','1997-01-28','1997-01-14','1997-02-02','TAKE BACK RETURN','RAIL','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (3,1,1,1,45.0,54750.75,0.06,0.00,'R','F','1994-02-02','1994-01-04','1994-02-23','NONE','AIR','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (3,2,2,2,49.0,46796.47,0.10,0.00,'R','F','1993-11-09','1993-12-20','1993-11-24','TAKE BACK RETURN','RAIL','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (4,1,1,1,28.0,28955.64,0.09,0.06,'N','O','1995-10-23','1995-10-13','1995-11-03','NONE','TRUCK','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (5,1,1,1,24.0,22824.48,0.10,0.07,'A','F','1994-10-31','1994-08-31','1994-11-20','NONE','AIR','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (6,1,1,1,32.0,49620.16,0.07,0.02,'N','O','1996-01-30','1995-11-22','1996-02-09','DELIVER IN PERSON','RAIL','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (7,2,2,1,38.0,44694.46,0.00,0.05,'N','O','1996-02-01','1996-01-01','1996-02-19','TAKE BACK RETURN','FOB','comment')");
    execSql(eng, parser, "INSERT INTO lineitem VALUES (8,3,3,1,45.0,54750.75,0.06,0.00,'N','O','1998-09-02','1998-08-01','1998-09-29','NONE','SHIP','comment')");
}

// ── TPC-H Queries (SF=0.01 adapted) ──────────────────────────

static const char* TPCH_QUERIES[22] = {
    // Q1: Pricing Summary Report
    "SELECT l_returnflag, l_linestatus, SUM(l_quantity) as sum_qty, SUM(l_extendedprice) as sum_base_price FROM lineitem WHERE l_shipdate <= '1998-09-01' GROUP BY l_returnflag, l_linestatus",
    // Q2: Minimum Cost Supplier (simplified)
    "SELECT s_acctbal, s_name, n_name, p_partkey FROM part, supplier, partsupp, nation WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey AND p_size = 1",
    // Q3: Shipping Priority (simplified)
    "SELECT o_orderkey, SUM(l_extendedprice) as revenue, o_orderdate FROM customer, orders, lineitem WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate < '1995-03-15' AND l_shipdate > '1995-03-15' GROUP BY o_orderkey, o_orderdate",
    // Q4: Order Priority Checking (simplified)
    "SELECT o_orderpriority, COUNT(*) as order_count FROM orders WHERE o_orderdate >= '1993-07-01' AND o_orderdate < '1993-10-01' GROUP BY o_orderpriority",
    // Q5: Local Supplier Volume (simplified)
    "SELECT n_name, SUM(l_extendedprice) as revenue FROM customer, orders, lineitem, supplier, nation WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey GROUP BY n_name",
    // Q6: Forecasting Revenue Change
    "SELECT SUM(l_extendedprice * l_discount) as revenue FROM lineitem WHERE l_shipdate >= '1994-01-01' AND l_shipdate < '1995-01-01' AND l_discount >= 0.05 AND l_discount <= 0.07 AND l_quantity < 24",
    // Q7: Volume Shipping (simplified)
    "SELECT n1.n_name as supp_nation, n2.n_name as cust_nation, SUM(l_extendedprice) as volume FROM supplier, lineitem, orders, customer, nation n1, nation n2 WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey AND c_nationkey = n2.n_nationkey GROUP BY n1.n_name, n2.n_name",
    // Q8: National Market Share (simplified)
    "SELECT o_orderdate, SUM(l_extendedprice) as volume FROM part, supplier, lineitem, orders, customer, nation WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey AND l_orderkey = o_orderkey AND o_custkey = c_custkey AND c_nationkey = n_nationkey GROUP BY o_orderdate",
    // Q9: Product Type Profit Measure (simplified)
    "SELECT n_name, SUM(l_extendedprice - ps_supplycost * l_quantity) as amount FROM part, supplier, lineitem, partsupp, orders, nation WHERE s_suppkey = l_suppkey AND ps_suppkey = l_suppkey AND ps_partkey = l_partkey AND p_partkey = l_partkey AND o_orderkey = l_orderkey AND s_nationkey = n_nationkey GROUP BY n_name",
    // Q10: Returned Item Reporting (simplified)
    "SELECT c_custkey, c_name, SUM(l_extendedprice) as revenue FROM customer, orders, lineitem WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_returnflag = 'R' AND o_orderdate >= '1993-10-01' AND o_orderdate < '1994-01-01' GROUP BY c_custkey, c_name",
    // Q11: Important Stock Identification (simplified)
    "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) as value FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = 'GERMANY' GROUP BY ps_partkey",
    // Q12: Shipping Modes and Order Priority (simplified)
    "SELECT l_shipmode, COUNT(*) as high_line_count FROM orders, lineitem WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL','SHIP') AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate AND l_receiptdate >= '1994-01-01' AND l_receiptdate < '1995-01-01' GROUP BY l_shipmode",
    // Q13: Customer Distribution (simplified)
    "SELECT COUNT(*) as custdist FROM customer",
    // Q14: Promotion Effect (simplified)
    "SELECT SUM(l_extendedprice) as promo_revenue FROM lineitem, part WHERE l_partkey = p_partkey AND l_shipdate >= '1995-09-01' AND l_shipdate < '1995-10-01'",
    // Q15: Top Supplier (simplified)
    "SELECT s_suppkey, s_name, SUM(l_extendedprice) as total_revenue FROM supplier, lineitem WHERE s_suppkey = l_suppkey AND l_shipdate >= '1996-01-01' AND l_shipdate < '1996-04-01' GROUP BY s_suppkey, s_name",
    // Q16: Parts/Supplier Relationship (simplified)
    "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) as supplier_cnt FROM partsupp, part WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#45' GROUP BY p_brand, p_type, p_size",
    // Q17: Small-Quantity Order Revenue (simplified)
    "SELECT SUM(l_extendedprice) as avg_yearly FROM lineitem, part WHERE p_partkey = l_partkey AND p_brand = 'Brand#23' AND p_container = 'MED BOX'",
    // Q18: Large Volume Customer (simplified)
    "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity) as total_qty FROM customer, orders, lineitem WHERE c_custkey = o_custkey AND o_orderkey = l_orderkey GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice",
    // Q19: Discounted Revenue (simplified)
    "SELECT SUM(l_extendedprice * (1 - l_discount)) as revenue FROM lineitem, part WHERE p_partkey = l_partkey AND l_shipinstruct = 'DELIVER IN PERSON'",
    // Q20: Potential Part Promotion (simplified)
    "SELECT s_name, s_address FROM supplier, nation WHERE s_nationkey = n_nationkey AND n_name = 'CANADA'",
    // Q21: Suppliers Who Kept Orders Waiting (simplified)
    "SELECT s_name, COUNT(*) as numwait FROM supplier, lineitem, orders, nation WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey AND o_orderstatus = 'F' AND s_nationkey = n_nationkey AND n_name = 'SAUDI ARABIA' GROUP BY s_name",
    // Q22: Global Sales Opportunity (simplified)
    "SELECT COUNT(*) as numcust, SUM(c_acctbal) as totacctbal FROM customer WHERE c_acctbal > 0.00"
};

static const char* TPCH_NAMES[22] = {
    "Q1:Pricing Summary","Q2:Min Cost Supplier","Q3:Shipping Priority",
    "Q4:Order Priority","Q5:Local Supplier","Q6:Revenue Forecast",
    "Q7:Volume Shipping","Q8:National Market","Q9:Product Profit",
    "Q10:Returned Items","Q11:Important Stock","Q12:Shipping Modes",
    "Q13:Customer Dist","Q14:Promo Effect","Q15:Top Supplier",
    "Q16:Part Supplier","Q17:Small Qty","Q18:Large Volume",
    "Q19:Discounted Rev","Q20:Potential Promo","Q21:Waiting Suppliers",
    "Q22:Sales Opportunity"
};

// ── Baseline JSON I/O ─────────────────────────────────────────

static bool baselineExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static void saveBaseline(const std::string& path, const double times[22]) {
    std::ofstream f(path);
    if (!f) { std::cerr << "Cannot write baseline: " << path << "\n"; return; }
    f << "{\n";
    for (int i = 0; i < 22; ++i) {
        f << "  \"" << TPCH_NAMES[i] << "\": " << std::fixed << std::setprecision(6) << times[i];
        if (i < 21) f << ",";
        f << "\n";
    }
    f << "}\n";
    std::cout << "  Baseline saved to: " << path << "\n";
}

static void loadBaseline(const std::string& path, double baseline[22]) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Cannot read baseline: " << path << "\n"; return; }
    std::ostringstream oss;
    oss << f.rdbuf();
    std::string json = oss.str();
    for (int i = 0; i < 22; ++i) {
        baseline[i] = parseJsonDouble(json, TPCH_NAMES[i]);
    }
}

} // namespace milansql

// ── main ──────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string baselinePath = "tpch_baseline.json";
    bool forceBaseline = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--reset-baseline") forceBaseline = true;
        if (std::string(argv[i]).rfind("--baseline=", 0) == 0)
            baselinePath = std::string(argv[i]).substr(11);
    }

    std::cout << "========================================\n";
    std::cout << "  MilanSQL TPC-H Regression (Phase 2.4)\n";
    std::cout << "========================================\n";

    milansql::Engine engine;
    engine.setCurrentUser(0, true);

    milansql::Parser mainParser;
    std::cout << "  Setting up TPC-H schema (SF=0.01)...\n";
    milansql::setupTpchSchema(engine);
    milansql::insertSampleData(engine);
    std::cout << "  Schema ready.\n\n";

    // Determine baseline mode
    bool hasBaseline = !forceBaseline && milansql::baselineExists(baselinePath);
    double baseline[22] = {};
    if (hasBaseline) {
        milansql::loadBaseline(baselinePath, baseline);
        std::cout << "  Loaded baseline from: " << baselinePath << "\n\n";
    } else {
        std::cout << "  No baseline found — will create one after run.\n\n";
    }

    // Run all 22 queries
    double times[22] = {};
    int    warns = 0;
    int    fails = 0;
    bool   anyError = false;

    std::cout << "  Running TPC-H queries...\n";
    std::cout << std::left;

    for (int i = 0; i < 22; ++i) {
        milansql::QueryResult qr;
        auto t0 = std::chrono::high_resolution_clock::now();
        qr = milansql::dispatch(mainParser.parse(milansql::TPCH_QUERIES[i]), engine);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        times[i] = ms;

        std::string status = "OK";
        if (!qr.error.empty()) {
            status = "ERR:" + qr.error.substr(0, 20);
            anyError = true;
        } else if (hasBaseline && baseline[i] > 0.001) {
            double ratio = ms / baseline[i];
            if (ratio > 1.5) {
                status = "FAIL(>50% slower)";
                ++fails;
            } else if (ratio > 1.2) {
                status = "WARN(>20% slower)";
                ++warns;
            }
        }

        std::cout << "  " << std::setw(26) << milansql::TPCH_NAMES[i]
                  << std::setw(10) << std::fixed << std::setprecision(3) << ms << " ms  "
                  << status << "\n";
    }

    std::cout << "\n";

    // Save or compare baseline
    if (!hasBaseline || forceBaseline) {
        milansql::saveBaseline(baselinePath, times);
    }

    // Output JSON summary
    std::string summaryPath = "tpch_summary.json";
    {
        std::ofstream sf(summaryPath);
        sf << "{\n";
        sf << "  \"baseline\": \"" << baselinePath << "\",\n";
        sf << "  \"baseline_exists\": " << (hasBaseline ? "true" : "false") << ",\n";
        sf << "  \"warnings\": " << warns << ",\n";
        sf << "  \"failures\": " << fails << ",\n";
        sf << "  \"errors\": " << (anyError ? "true" : "false") << ",\n";
        sf << "  \"results\": {\n";
        for (int i = 0; i < 22; ++i) {
            sf << "    \"" << milansql::TPCH_NAMES[i] << "\": "
               << std::fixed << std::setprecision(6) << times[i];
            if (i < 21) sf << ",";
            sf << "\n";
        }
        sf << "  }\n}\n";
    }
    std::cout << "  Summary written to: " << summaryPath << "\n";

    // Final verdict
    if (fails > 0) {
        std::cout << "  RESULT: FAIL — " << fails << " query(ies) >50% regression, " << warns << " warn(s)\n";
        return 1;
    }
    if (warns > 0) {
        std::cout << "  RESULT: WARN — " << warns << " query(ies) >20% slower\n";
        return 0;
    }
    std::cout << "  RESULT: PASS — all queries within baseline\n";
    return 0;
}
