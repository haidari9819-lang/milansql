#pragma once
// ============================================================
// nl_query.hpp -- Natural Language to SQL translation
// Block 7: LLM-powered NL query endpoint
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace milansql {
namespace nl {

// ── NL Configuration (per-server, thread-safe) ────────────────

struct NLConfig {
    std::string apiKey;       // LLM API key
    std::string model;        // e.g. "llama-3.3-70b-versatile" or "gpt-4o-mini"
    std::string provider;     // "groq" or "openai"
    std::mutex  mtx;

    NLConfig() : model("llama-3.3-70b-versatile"), provider("groq") {
        // Try environment variable
        const char* envKey = std::getenv("MILANSQL_NL_API_KEY");
        if (envKey && envKey[0]) apiKey = envKey;
        const char* envModel = std::getenv("MILANSQL_NL_MODEL");
        if (envModel && envModel[0]) model = envModel;
        const char* envProvider = std::getenv("MILANSQL_NL_PROVIDER");
        if (envProvider && envProvider[0]) provider = envProvider;
    }

    void setApiKey(const std::string& k) { std::lock_guard<std::mutex> lk(mtx); apiKey = k; }
    void setModel(const std::string& m) { std::lock_guard<std::mutex> lk(mtx); model = m; }
    void setProvider(const std::string& p) {
        std::lock_guard<std::mutex> lk(mtx);
        std::string lp = p;
        for (auto& c : lp) c = (char)std::tolower((unsigned char)c);
        if (lp == "groq" || lp == "openai") provider = lp;
    }
    std::string getApiKey()   { std::lock_guard<std::mutex> lk(mtx); return apiKey; }
    std::string getModel()    { std::lock_guard<std::mutex> lk(mtx); return model; }
    std::string getProvider() { std::lock_guard<std::mutex> lk(mtx); return provider; }
    bool hasKey()             { std::lock_guard<std::mutex> lk(mtx); return !apiKey.empty(); }
};

inline NLConfig& g_nlConfig() {
    static NLConfig cfg;
    return cfg;
}

// ── Schema Context Builder ────────────────────────────────────

struct ColumnInfo {
    std::string name;
    std::string type;
};

struct TableSchema {
    std::string name;
    std::vector<ColumnInfo> columns;
    std::vector<std::vector<std::string>> sampleRows; // up to 3
};

inline std::string buildSchemaContext(const std::vector<TableSchema>& schemas) {
    std::string ctx;
    for (const auto& tbl : schemas) {
        ctx += "Table \"" + tbl.name + "\" (";
        for (size_t i = 0; i < tbl.columns.size(); ++i) {
            if (i > 0) ctx += ", ";
            ctx += tbl.columns[i].name + " " + tbl.columns[i].type;
        }
        ctx += ")";
        if (!tbl.sampleRows.empty()) {
            ctx += "\n  Sample data:";
            for (const auto& row : tbl.sampleRows) {
                ctx += "\n    (";
                for (size_t i = 0; i < row.size(); ++i) {
                    if (i > 0) ctx += ", ";
                    ctx += row[i];
                }
                ctx += ")";
            }
        }
        ctx += "\n";
    }
    return ctx;
}

// ── Build LLM Prompt ──────────────────────────────────────────

inline std::string buildPrompt(const std::string& question,
                                const std::string& schemaCtx) {
    return "You are a SQL assistant for MilanSQL database. "
           "Given the following database schema:\n\n"
           + schemaCtx +
           "\nConvert this natural language query to SQL:\n\""
           + question + "\"\n\n"
           "Rules:\n"
           "- Return ONLY the SQL query, nothing else\n"
           "- Do not include any explanation, markdown, or code fences\n"
           "- Use only tables and columns that exist in the schema above\n"
           "- Use standard SQL syntax\n"
           "- Return a single statement (no semicolons)\n"
           "- Only SELECT, INSERT, UPDATE, DELETE are allowed\n\n"
           "SQL:";
}

// ── Extract SQL from LLM response ─────────────────────────────

inline std::string extractSqlFromResponse(const std::string& response) {
    std::string sql = response;

    // Strip markdown code fences if present
    {
        auto pos = sql.find("```sql");
        if (pos != std::string::npos) {
            sql = sql.substr(pos + 6);
            auto end = sql.find("```");
            if (end != std::string::npos) sql = sql.substr(0, end);
        } else {
            auto pos2 = sql.find("```");
            if (pos2 != std::string::npos) {
                sql = sql.substr(pos2 + 3);
                auto end = sql.find("```");
                if (end != std::string::npos) sql = sql.substr(0, end);
            }
        }
    }

    // Strip leading "SQL:" if present
    {
        std::string trimmed = sql;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) trimmed = trimmed.substr(start);
        std::string upper;
        for (size_t i = 0; i < std::min(trimmed.size(), (size_t)4); ++i)
            upper += (char)std::toupper((unsigned char)trimmed[i]);
        if (upper == "SQL:") sql = trimmed.substr(4);
    }

    // Trim whitespace
    while (!sql.empty() && (sql.front() == ' ' || sql.front() == '\t' ||
           sql.front() == '\n' || sql.front() == '\r'))
        sql.erase(sql.begin());
    while (!sql.empty() && (sql.back() == ' ' || sql.back() == '\t' ||
           sql.back() == '\n' || sql.back() == '\r' || sql.back() == ';'))
        sql.pop_back();

    return sql;
}

// ── Safety Validator ──────────────────────────────────────────

struct SafetyResult {
    bool safe;
    std::string reason;
};

inline SafetyResult validateSafety(const std::string& sql) {
    SafetyResult r{true, ""};
    if (sql.empty()) { r.safe = false; r.reason = "Empty SQL"; return r; }

    // Uppercase for checking
    std::string upper;
    upper.reserve(sql.size());
    for (char c : sql) upper += (char)std::toupper((unsigned char)c);

    // Check for multiple statements (semicolons)
    // Allow semicolons inside string literals
    {
        bool inStr = false;
        for (size_t i = 0; i < sql.size(); ++i) {
            if (sql[i] == '\'') inStr = !inStr;
            if (!inStr && sql[i] == ';') {
                r.safe = false;
                r.reason = "Multiple statements not allowed (semicolons detected)";
                return r;
            }
        }
    }

    // Block dangerous operations first (more specific error messages)
    auto contains = [&](const std::string& pat) {
        return upper.find(pat) != std::string::npos;
    };

    if (contains("DROP DATABASE")) {
        r.safe = false; r.reason = "DROP DATABASE not allowed"; return r;
    }
    if (contains("DROP TABLE")) {
        r.safe = false; r.reason = "DROP TABLE not allowed in NL-generated SQL"; return r;
    }
    if (contains("TRUNCATE")) {
        r.safe = false; r.reason = "TRUNCATE not allowed"; return r;
    }

    // Only allow SELECT, INSERT, UPDATE, DELETE, WITH (CTE)
    std::string firstWord;
    for (char c : upper) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(') break;
        firstWord += c;
    }
    if (firstWord != "SELECT" && firstWord != "INSERT" &&
        firstWord != "UPDATE" && firstWord != "DELETE" &&
        firstWord != "WITH") {
        r.safe = false;
        r.reason = "Only SELECT, INSERT, UPDATE, DELETE statements allowed (got " + firstWord + ")";
        return r;
    }

    // Block system table access
    std::vector<std::string> systemTables = {
        "SYS_USERS", "SYS_SESSIONS", "SYS_CONFIG", "MILANSQL_INTERNAL",
        "PG_CATALOG", "INFORMATION_SCHEMA", "SQLITE_MASTER"
    };
    for (const auto& st : systemTables) {
        if (contains(st)) {
            r.safe = false;
            r.reason = "Access to system table " + st + " not allowed";
            return r;
        }
    }

    return r;
}

// ── Build LLM API request body (JSON) ─────────────────────────

inline std::string escJson(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c); r += buf; }
        else r += (char)c;
    }
    return r;
}

inline std::string buildApiRequestBody(const std::string& prompt, const std::string& model) {
    return "{\"model\":\"" + escJson(model) + "\","
           "\"messages\":[{\"role\":\"user\",\"content\":\"" + escJson(prompt) + "\"}],"
           "\"temperature\":0.1,\"max_tokens\":500}";
}

inline std::string getApiUrl(const std::string& provider) {
    if (provider == "openai") return "https://api.openai.com/v1/chat/completions";
    return "https://api.groq.com/openai/v1/chat/completions"; // groq default
}

// ── Extract content from LLM API JSON response ───────────────

inline std::string extractContentFromApiResponse(const std::string& json) {
    // Find "content" field in the response JSON
    // Typical: {"choices":[{"message":{"content":"SELECT ..."}}]}
    auto pos = json.find("\"content\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + 9);
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return "";

    if (json[pos] == '"') {
        // Parse JSON string
        std::string result;
        size_t i = pos + 1;
        while (i < json.size()) {
            char c = json[i];
            if (c == '"') break;
            if (c == '\\' && i + 1 < json.size()) {
                ++i;
                char esc = json[i];
                if (esc == 'n') result += '\n';
                else if (esc == 'r') result += '\r';
                else if (esc == 't') result += '\t';
                else if (esc == '"') result += '"';
                else if (esc == '\\') result += '\\';
                else { result += '\\'; result += esc; }
            } else {
                result += c;
            }
            ++i;
        }
        return result;
    }
    return "";
}

// ── LLM HTTP Client (libcurl or fallback) ─────────────────────
// This calls the LLM API over HTTPS. On Linux with libcurl installed,
// we shell out to `curl` as a simple reliable approach.

inline std::string callLlmApi(const std::string& prompt,
                               std::string& errorOut) {
    auto& cfg = g_nlConfig();
    std::string apiKey   = cfg.getApiKey();
    std::string model    = cfg.getModel();
    std::string provider = cfg.getProvider();

    if (apiKey.empty()) {
        errorOut = "NL API key not configured. Use SET NL_API_KEY = 'your-key' or set MILANSQL_NL_API_KEY env var";
        return "";
    }

    std::string url  = getApiUrl(provider);
    std::string body = buildApiRequestBody(prompt, model);

    // Shell out to curl (available on Linux servers)
    // Write body to temp file to avoid shell escaping issues
    std::string tmpFile = "/tmp/milansql_nl_" + std::to_string(std::hash<std::string>{}(prompt) & 0xFFFFFF) + ".json";
    {
        std::ofstream f(tmpFile);
        if (!f) { errorOut = "Cannot create temp file"; return ""; }
        f << body;
    }

    std::string cmd = "curl -s -m 30 -X POST \"" + url + "\" "
                      "-H \"Content-Type: application/json\" "
                      "-H \"Authorization: Bearer " + apiKey + "\" "
                      "-d @" + tmpFile + " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::remove(tmpFile.c_str());
        errorOut = "Failed to execute curl";
        return "";
    }

    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    int status = pclose(pipe);
    std::remove(tmpFile.c_str());

    if (status != 0 || result.empty()) {
        errorOut = "LLM API call failed (curl exit " + std::to_string(status) + ")";
        return "";
    }

    // Check for API error
    if (result.find("\"error\"") != std::string::npos &&
        result.find("\"choices\"") == std::string::npos) {
        // Extract error message
        auto epos = result.find("\"message\"");
        if (epos != std::string::npos) {
            auto vpos = result.find('"', epos + 10);
            if (vpos != std::string::npos) {
                auto vend = result.find('"', vpos + 1);
                if (vend != std::string::npos) {
                    errorOut = "LLM API error: " + result.substr(vpos + 1, vend - vpos - 1);
                    return "";
                }
            }
        }
        errorOut = "LLM API returned error";
        return "";
    }

    std::string content = extractContentFromApiResponse(result);
    if (content.empty()) {
        errorOut = "Could not parse LLM response";
        return "";
    }

    return content;
}

// ── SHOW NL STATUS ────────────────────────────────────────────

inline std::string nlStatusJson() {
    auto& cfg = g_nlConfig();
    std::string json = "{\"provider\":\"" + escJson(cfg.getProvider()) + "\","
                       "\"model\":\"" + escJson(cfg.getModel()) + "\","
                       "\"api_key_set\":" + (cfg.hasKey() ? "true" : "false") + "}";
    return json;
}

} // namespace nl
} // namespace milansql
