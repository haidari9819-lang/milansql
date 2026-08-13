#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <ctime>
#include <random>
#include <iomanip>

// Requires cloud_instance.hpp for CloudPlan / getPlanLimits
#include "cloud_instance.hpp"

namespace milansql {

// ── Invoice / subscription record ────────────────────────────────────────────
struct Invoice {
    std::string invoiceId;
    std::string userId;
    std::string month;        // "YYYY-MM"
    std::string plan;
    double      amountEur;
    std::string status;       // "paid" | "pending" | "overdue"
    std::string issuedAt;
    std::string items;        // JSON array of line items (stored as string)
};

struct Subscription {
    std::string subscriptionId;
    std::string userId;
    std::string plan;
    std::string status;       // "active" | "cancelled" | "past_due"
    std::string createdAt;
    std::string currentPeriodEnd;
};

// ── ThrottleResult ────────────────────────────────────────────────────────────
struct ThrottleResult {
    bool        throttled;
    std::string reason;
    std::string action;       // "block" | "warn" | "allow"
};

// ── BillingManager ────────────────────────────────────────────────────────────
class BillingManager {
public:
    static BillingManager& instance() {
        static BillingManager m;
        return m;
    }
public:
    explicit BillingManager(const std::string& dataDir = "/opt/milansql/data")
        : dataDir_(dataDir) {
        subFilePath_     = dataDir_ + "/cloud_subscriptions.data";
        invoiceFilePath_ = dataDir_ + "/cloud_invoices.data";
        loadSubscriptions();
        loadInvoices();
    }

    // ── Plan information ──────────────────────────────────────────────────────

    // Returns plan limits as JSON
    std::string getPlanLimitsJson(CloudPlan plan) const {
        PlanLimits lim = getPlanLimits(plan);
        std::ostringstream j;
        j << "{"
          << "\"name\":\""          << lim.name            << "\","
          << "\"storageGB\":"        << (lim.storageBytes < 0 ? -1 : lim.storageBytes / (1024*1024*1024)) << ","
          << "\"storageBytes\":"     << lim.storageBytes   << ","
          << "\"queriesPerDay\":"    << lim.queriesPerDay  << ","
          << "\"priceMonthly\":"     << lim.priceMonthly   << ","
          << "\"prioritySupport\":"  << (lim.prioritySupport ? "true" : "false")
          << "}";
        return j.str();
    }

    // Returns all plans as JSON array
    std::string getAllPlansJson() const {
        std::ostringstream j;
        j << "["
          << getPlanLimitsJson(CloudPlan::FREE)       << ","
          << getPlanLimitsJson(CloudPlan::STARTER)    << ","
          << getPlanLimitsJson(CloudPlan::PRO)        << ","
          << getPlanLimitsJson(CloudPlan::ENTERPRISE)
          << "]";
        return j.str();
    }

    // ── Usage throttle check ──────────────────────────────────────────────────

    ThrottleResult checkUsage(const std::string& instanceId,
                              long long queryCount,
                              long long storageBytes,
                              CloudPlan plan) const {
        PlanLimits lim = getPlanLimits(plan);
        ThrottleResult result{false, "ok", "allow"};

        // Check daily query limit
        if (lim.queriesPerDay > 0 && queryCount >= lim.queriesPerDay) {
            result.throttled = true;
            result.reason    = "Daily query limit exceeded: " +
                               std::to_string(queryCount) + "/" +
                               std::to_string(lim.queriesPerDay);
            result.action    = "block";
            return result;
        }

        // Warn at 90% of query limit
        if (lim.queriesPerDay > 0 &&
            queryCount >= static_cast<long long>(lim.queriesPerDay * 0.9)) {
            result.reason = "Approaching daily query limit";
            result.action = "warn";
            return result;
        }

        // Check storage limit
        if (lim.storageBytes > 0 && storageBytes > lim.storageBytes) {
            result.throttled = true;
            result.reason    = "Storage limit exceeded";
            result.action    = "block";
            return result;
        }

        return result;
    }

    // ── Stripe-mock subscription ──────────────────────────────────────────────

    // Mock: creates a subscription record and returns a fake subscription ID
    std::string createSubscription(const std::string& userId, CloudPlan plan) {
        std::lock_guard<std::mutex> lk(mu_);

        Subscription sub;
        sub.subscriptionId  = "sub_" + hexRandom(12);
        sub.userId          = userId;
        sub.plan            = planToString(plan);
        sub.status          = "active";
        sub.createdAt       = currentIsoTimestamp();
        sub.currentPeriodEnd = nextMonthIso();

        subscriptions_[sub.subscriptionId] = sub;
        saveSubscriptions();
        return sub.subscriptionId;
    }

    // Cancel a subscription
    void cancelSubscription(const std::string& subscriptionId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = subscriptions_.find(subscriptionId);
        if (it == subscriptions_.end())
            throw std::runtime_error("Subscription not found: " + subscriptionId);
        it->second.status = "cancelled";
        saveSubscriptions();
    }

    std::string getSubscriptionJson(const std::string& subscriptionId) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = subscriptions_.find(subscriptionId);
        if (it == subscriptions_.end())
            throw std::runtime_error("Subscription not found: " + subscriptionId);
        return subscriptionToJson(it->second);
    }

    // ── Invoice ───────────────────────────────────────────────────────────────

    // Returns a mock invoice for userId + month (YYYY-MM)
    std::string getInvoiceJson(const std::string& userId,
                               const std::string& month) const {
        std::lock_guard<std::mutex> lk(mu_);

        // Look for existing invoice
        for (auto& [k, inv] : invoices_)
            if (inv.userId == userId && inv.month == month)
                return invoiceToJson(inv);

        // Generate mock invoice
        Invoice inv = buildMockInvoice(userId, month);
        return invoiceToJson(inv);
    }

    // Returns billing usage summary for userId + month
    std::string getBillingUsageJson(const std::string& userId,
                                    const std::string& month) const {
        std::lock_guard<std::mutex> lk(mu_);
        // In production this would query the UsageMeter; here we return mock data
        std::ostringstream j;
        j << "{"
          << "\"userId\":\""      << userId << "\","
          << "\"month\":\""       << month  << "\","
          << "\"totalQueries\":"  << 4200   << ","
          << "\"storageGB\":"     << 1.2    << ","
          << "\"bandwidthGB\":"   << 0.8    << ","
          << "\"estimatedCost\":" << 9.0
          << "}";
        return j.str();
    }

    // List all subscriptions for a user
    std::string listSubscriptionsJson(const std::string& userId) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& [k, v] : subscriptions_) {
            if (v.userId != userId) continue;
            if (!first) j << ",";
            first = false;
            j << subscriptionToJson(v);
        }
        j << "]";
        return j.str();
    }

private:
    std::string dataDir_;
    std::string subFilePath_;
    std::string invoiceFilePath_;
    mutable std::mutex mu_;

    std::unordered_map<std::string, Subscription> subscriptions_;
    std::unordered_map<std::string, Invoice>      invoices_;

    // ── Serialisation helpers ─────────────────────────────────────────────────

    static std::string subscriptionToJson(const Subscription& s) {
        std::ostringstream j;
        j << "{"
          << "\"subscriptionId\":\""   << s.subscriptionId  << "\","
          << "\"userId\":\""           << s.userId          << "\","
          << "\"plan\":\""             << s.plan            << "\","
          << "\"status\":\""           << s.status          << "\","
          << "\"createdAt\":\""        << s.createdAt       << "\","
          << "\"currentPeriodEnd\":\"" << s.currentPeriodEnd << "\""
          << "}";
        return j.str();
    }

    static std::string invoiceToJson(const Invoice& inv) {
        std::ostringstream j;
        j << "{"
          << "\"invoiceId\":\""  << inv.invoiceId  << "\","
          << "\"userId\":\""     << inv.userId     << "\","
          << "\"month\":\""      << inv.month      << "\","
          << "\"plan\":\""       << inv.plan       << "\","
          << "\"amountEur\":"    << inv.amountEur  << ","
          << "\"status\":\""     << inv.status     << "\","
          << "\"issuedAt\":\""   << inv.issuedAt   << "\","
          << "\"items\":"        << inv.items
          << "}";
        return j.str();
    }

    static Invoice buildMockInvoice(const std::string& userId,
                                    const std::string& month) {
        Invoice inv;
        inv.invoiceId = "inv_" + month + "_" + userId.substr(0, 8);
        inv.userId    = userId;
        inv.month     = month;
        inv.plan      = "starter";
        inv.amountEur = 9.0;
        inv.status    = "paid";
        inv.issuedAt  = month + "-01T00:00:00Z";
        inv.items     = "[{"
                         "\"description\":\"Starter Plan\","
                         "\"quantity\":1,"
                         "\"unitPrice\":9.0,"
                         "\"total\":9.0"
                         "}]";
        return inv;
    }

    // ── Tiny utilities ────────────────────────────────────────────────────────

    static std::string hexRandom(size_t bytes) {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint32_t> dist(0, 255);
        std::ostringstream oss;
        oss << std::hex;
        for (size_t i = 0; i < bytes; ++i) {
            unsigned v = dist(gen);
            if (v < 16) oss << "0";
            oss << v;
        }
        return oss.str();
    }

    static std::string currentIsoTimestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        struct tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
    }

    static std::string nextMonthIso() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        tm_buf.tm_mon++;
        if (tm_buf.tm_mon > 11) { tm_buf.tm_mon = 0; tm_buf.tm_year++; }
        std::mktime(&tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
    }

    // ── Persistence ───────────────────────────────────────────────────────────

    static std::string subscriptionToLine(const Subscription& s) {
        return s.subscriptionId + "|" + s.userId + "|" + s.plan + "|" +
               s.status + "|" + s.createdAt + "|" + s.currentPeriodEnd;
    }

    static Subscription lineToSubscription(const std::string& line) {
        Subscription s;
        std::vector<std::string> parts;
        std::istringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, '|')) parts.push_back(tok);
        if (parts.size() < 6) return s;
        s.subscriptionId  = parts[0];
        s.userId          = parts[1];
        s.plan            = parts[2];
        s.status          = parts[3];
        s.createdAt       = parts[4];
        s.currentPeriodEnd = parts[5];
        return s;
    }

    void loadSubscriptions() {
        std::ifstream f(subFilePath_);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            Subscription s = lineToSubscription(line);
            if (!s.subscriptionId.empty())
                subscriptions_[s.subscriptionId] = s;
        }
    }

    void saveSubscriptions() {
        std::ofstream f(subFilePath_, std::ios::trunc);
        if (!f.is_open()) return;
        f << "# MilanSQL Cloud Subscriptions\n";
        for (auto& [k, v] : subscriptions_)
            f << subscriptionToLine(v) << "\n";
    }

    void loadInvoices() {
        // Invoices are currently generated on-the-fly; persistence stub for future use
        std::ifstream f(invoiceFilePath_);
        if (!f.is_open()) return;
        // (Reserved for future invoice persistence)
    }
};

} // namespace milansql
