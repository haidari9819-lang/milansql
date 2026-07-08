#pragma once
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <algorithm>

// ============================================================
// timeseries_manager.hpp — MilanSQL Phase 97: Time-Series
// ============================================================

namespace milansql {

enum class TsPartitionBy { HOUR, DAY, WEEK, MONTH };

struct TimeSeriesDef {
    std::string    tableName;
    std::string    timeColumn;
    TsPartitionBy  partitionBy  = TsPartitionBy::DAY;
    int            retentionDays = -1;  // -1 = no retention
    bool           active        = true;
};

class TimeSeriesManager {
public:
    void define(TimeSeriesDef def) {
        defs_[def.tableName] = std::move(def);
    }

    bool isTimeSeries(const std::string& tableName) const {
        return defs_.count(tableName) > 0;
    }

    const TimeSeriesDef* getDef(const std::string& tableName) const {
        auto it = defs_.find(tableName);
        return it != defs_.end() ? &it->second : nullptr;
    }

    const std::map<std::string, TimeSeriesDef>& allDefs() const {
        return defs_;
    }

    // Show status for one table, given its rows and the time column index
    std::string showStatus(const std::string& tableName,
                           const std::vector<std::vector<std::string>>& rows,
                           int timeColIdx) const {
        const TimeSeriesDef* def = getDef(tableName);
        if (!def) return "  Not a time-series table: " + tableName + "\n";

        std::ostringstream oss;
        oss << "Time-Series Table: " << tableName << "\n";
        oss << "  Time Column:   " << def->timeColumn << "\n";

        std::string pbStr;
        switch (def->partitionBy) {
            case TsPartitionBy::HOUR:  pbStr = "HOUR";  break;
            case TsPartitionBy::WEEK:  pbStr = "WEEK";  break;
            case TsPartitionBy::MONTH: pbStr = "MONTH"; break;
            default:                   pbStr = "DAY";   break;
        }
        oss << "  Partition By:  " << pbStr << "\n";

        if (def->retentionDays >= 0)
            oss << "  Retention:     " << def->retentionDays << " days\n";
        else
            oss << "  Retention:     none\n";

        oss << "  Total Rows:    " << rows.size() << "\n";
        oss << "Partitions:\n";

        // Build partition label → row count
        std::map<std::string, int> partCounts;
        for (const auto& row : rows) {
            std::string tsVal;
            if (timeColIdx >= 0 && static_cast<size_t>(timeColIdx) < row.size())
                tsVal = row[static_cast<size_t>(timeColIdx)];
            std::string label = getPartitionLabel(def->partitionBy, tsVal);
            partCounts[label]++;
        }

        if (partCounts.empty()) {
            oss << "  (no data)\n";
        } else {
            for (const auto& kv : partCounts)
                oss << "  " << kv.first << ": " << kv.second << " rows\n";
        }
        return oss.str();
    }

    // Show status for all registered time-series tables (no row data)
    std::string showAllStatus() const {
        if (defs_.empty())
            return "  No time-series tables registered.\n";
        std::ostringstream oss;
        for (const auto& kv : defs_) {
            const auto& def = kv.second;
            std::string pbStr;
            switch (def.partitionBy) {
                case TsPartitionBy::HOUR:  pbStr = "HOUR";  break;
                case TsPartitionBy::WEEK:  pbStr = "WEEK";  break;
                case TsPartitionBy::MONTH: pbStr = "MONTH"; break;
                default:                   pbStr = "DAY";   break;
            }
            oss << "Table: " << def.tableName
                << "  timeCol=" << def.timeColumn
                << "  partitionBy=" << pbStr;
            if (def.retentionDays >= 0)
                oss << "  retention=" << def.retentionDays << "d";
            oss << "\n";
        }
        return oss.str();
    }

    // time_bucket: truncate timestamp to bucket interval
    // interval like "1 DAY", "1 HOUR", "1 WEEK", "1 MONTH"
    // tsStr like "2026-01-01 08:00:00" or "2026-01-01"
    static std::string timeBucket(const std::string& interval, const std::string& tsStr) {
        std::string unit;
        int amount = 1;
        std::istringstream iss(interval);
        iss >> amount >> unit;
        for (auto& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Extract date parts
        std::string datepart = tsStr.size() >= 10 ? tsStr.substr(0, 10) : tsStr;
        std::string timepart = tsStr.size() > 10 ? tsStr.substr(11, 8) : "00:00:00";

        int year = 0, month = 0, day = 0, hour = 0, minute = 0, sec = 0;
        { char sep; std::istringstream ds(datepart); ds >> year >> sep >> month >> sep >> day; }
        { char sep; std::istringstream ts(timepart); ts >> hour >> sep >> minute >> sep >> sec; }
        (void)minute; (void)sec;

        if (unit == "hour" || unit == "hours") {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:00:00",
                          year, month, day, (hour / amount) * amount);
            return std::string(buf);
        } else if (unit == "day" || unit == "days") {
            return datepart;
        } else if (unit == "week" || unit == "weeks") {
            // Simplified: truncate to day
            return datepart;
        } else if (unit == "month" || unit == "months") {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04d-%02d", year, month);
            return std::string(buf);
        }
        return datepart;
    }

    // Get partition label for a timestamp
    static std::string getPartitionLabel(TsPartitionBy pb, const std::string& tsStr) {
        std::string interval;
        switch (pb) {
            case TsPartitionBy::HOUR:  interval = "1 HOUR";  break;
            case TsPartitionBy::WEEK:  interval = "1 WEEK";  break;
            case TsPartitionBy::MONTH: interval = "1 MONTH"; break;
            default:                   interval = "1 DAY";   break;
        }
        return timeBucket(interval, tsStr);
    }

private:
    std::map<std::string, TimeSeriesDef> defs_;
};

} // namespace milansql
