#pragma once
// compliance_report.hpp — Phase 5.4: Compliance Reports (DSGVO/GoBD/SOC2)
#include <string>
#include <vector>

namespace milansql {

class ComplianceReporter {
public:
    struct ReportContext {
        std::string generatedAt;
        std::string serverVersion;
        int         tableCount    = 0;
        int         userCount     = 0;
        int         auditEntries  = 0;
        bool        encryptionOn  = false;
        bool        mtlsOn        = false;
        bool        auditOn       = false;
        bool        auditChainOk  = false;
        std::vector<std::string> tablesWithRls;
        std::vector<std::string> recentAdminActions;
    };

    static std::string generateDSGVO(const ReportContext& ctx) {
        std::string j = "{\"report_type\":\"DSGVO_GDPR\"";
        j += ",\"generated_at\":\"" + ctx.generatedAt + "\"";
        j += ",\"version\":\"" + ctx.serverVersion + "\"";
        j += ",\"data_inventory\":{\"total_tables\":" + std::to_string(ctx.tableCount);
        j += ",\"tables_with_rls\":" + std::to_string(ctx.tablesWithRls.size()) + "}";
        j += ",\"access_controls\":{";
        j += "\"row_level_security\":" + std::string(!ctx.tablesWithRls.empty() ? "true" : "false");
        j += ",\"encryption_at_rest\":" + std::string(ctx.encryptionOn ? "true" : "false");
        j += ",\"mtls_enabled\":" + std::string(ctx.mtlsOn ? "true" : "false") + "}";
        j += ",\"audit_trail\":{\"enabled\":" + std::string(ctx.auditOn ? "true" : "false");
        j += ",\"entries\":" + std::to_string(ctx.auditEntries);
        j += ",\"integrity_verified\":" + std::string(ctx.auditChainOk ? "true" : "false") + "}";
        j += ",\"recommendations\":[";
        bool first = true;
        if (!ctx.encryptionOn) { j += "\"Enable encryption at rest\""; first=false; }
        if (!ctx.auditOn)      { if(!first)j+=","; j+="\"Enable audit logging\""; first=false; }
        if (!ctx.mtlsOn)       { if(!first)j+=","; j+="\"Enable mTLS transport security\""; first=false; }
        if (ctx.tablesWithRls.empty()) { if(!first)j+=","; j+="\"Apply Row-Level Security policies\""; first=false; }
        j += "],\"compliance_score\":" + std::to_string(computeScore(ctx)) + "}";
        return j;
    }

    static std::string generateGoBD(const ReportContext& ctx) {
        std::string j = "{\"report_type\":\"GoBD\"";
        j += ",\"generated_at\":\"" + ctx.generatedAt + "\"";
        j += ",\"version\":\"" + ctx.serverVersion + "\"";
        j += ",\"immutability\":{\"audit_hash_chain\":" + std::string(ctx.auditChainOk ? "true" : "false");
        j += ",\"tamper_detection\":\"FNV-hash-chain\",\"audit_entries\":" + std::to_string(ctx.auditEntries) + "}";
        j += ",\"access_log\":{\"recent_admin_actions\":[";
        for (size_t i = 0; i < ctx.recentAdminActions.size() && i < 20; ++i) {
            if (i) j += ",";
            j += "\"" + ctx.recentAdminActions[i] + "\"";
        }
        j += "]}";
        j += ",\"data_exportability\":\"available via /backup and SELECT\"";
        j += ",\"compliance_score\":" + std::to_string(computeScore(ctx)) + "}";
        return j;
    }

    static std::string generateSOC2(const ReportContext& ctx) {
        std::string j = "{\"report_type\":\"SOC2\"";
        j += ",\"generated_at\":\"" + ctx.generatedAt + "\"";
        j += ",\"version\":\"" + ctx.serverVersion + "\"";
        j += ",\"trust_service_criteria\":{";
        j += "\"CC6_security\":{\"logical_access_controls\":" + std::string(ctx.userCount>0?"true":"false");
        j += ",\"encryption_at_rest\":" + std::string(ctx.encryptionOn?"true":"false");
        j += ",\"mtls_transport\":" + std::string(ctx.mtlsOn?"true":"false");
        j += ",\"ip_allowlisting\":true";
        j += ",\"row_level_security\":" + std::string(!ctx.tablesWithRls.empty()?"true":"false") + "}";
        j += ",\"A1_availability\":{\"backup_capability\":true,\"pitr_capability\":true,\"replication_supported\":true}";
        j += ",\"C1_confidentiality\":{\"audit_logging\":" + std::string(ctx.auditOn?"true":"false") + "}}";
        j += ",\"compliance_score\":" + std::to_string(computeScore(ctx)) + "}";
        return j;
    }

private:
    static int computeScore(const ReportContext& ctx) {
        int score = 0;
        if (ctx.encryptionOn) score += 25;
        if (ctx.auditOn)      score += 25;
        if (ctx.auditChainOk) score += 15;
        if (ctx.mtlsOn)       score += 15;
        if (!ctx.tablesWithRls.empty()) score += 20;
        return score;
    }
};

} // namespace milansql
