#pragma once
// Phase 55: DATE/TIME Utility-Funktionen für MilanSQL
// Standalone header – keine Abhängigkeit zu engine.hpp / storage.hpp

#include <string>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace milansql {

// Thread-safe wrappers for localtime/gmtime (MEDIUM bug fix)
inline std::tm safe_localtime(const std::time_t* t) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, t);
#else
    localtime_r(t, &result);
#endif
    return result;
}

inline std::tm safe_gmtime(const std::time_t* t) {
    std::tm result{};
#ifdef _WIN32
    gmtime_s(&result, t);
#else
    gmtime_r(t, &result);
#endif
    return result;
}

namespace dateutils {

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────

// Entfernt umgebende einfache oder doppelte Anführungszeichen, falls vorhanden
inline std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '\'' && s.back() == '\'') ||
         (s.front() == '"'  && s.back() == '"')))
        return s.substr(1, s.size() - 2);
    return s;
}

// ── Aktuelle Zeit holen ──────────────────────────────────────────────────────

inline std::tm currentTm() {
    std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    return t;
}

inline std::string currentDateStr() {
    auto t = currentTm();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buf;
}

inline std::string currentTimeStr() {
    auto t = currentTm();
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

inline std::string currentDatetimeStr() {
    auto t = currentTm();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

// ── Datum/Zeit parsen ────────────────────────────────────────────────────────

// Parst "YYYY-MM-DD" → tm  (gibt false bei Fehler)
inline bool parseDateStr(const std::string& s, std::tm& out) {
    out = {};
    if (s.size() < 10) return false;
    try {
        int y = std::stoi(s.substr(0, 4));
        int m = std::stoi(s.substr(5, 2));
        int d = std::stoi(s.substr(8, 2));
        if (m < 1 || m > 12 || d < 1 || d > 31) return false;
        out.tm_year = y - 1900;
        out.tm_mon  = m - 1;
        out.tm_mday = d;
        return true;
    } catch (...) { return false; }
}

// Parst "HH:MM:SS" (oder "HH:MM") → tm
inline bool parseTimeStr(const std::string& s, std::tm& out) {
    out = {};
    if (s.size() < 5) return false;
    try {
        out.tm_hour = std::stoi(s.substr(0, 2));
        out.tm_min  = std::stoi(s.substr(3, 2));
        out.tm_sec  = s.size() >= 8 ? std::stoi(s.substr(6, 2)) : 0;
        return true;
    } catch (...) { return false; }
}

// Parst "YYYY-MM-DD HH:MM:SS" → tm
inline bool parseDatetimeStr(const std::string& s, std::tm& out) {
    out = {};
    if (s.size() < 19) return false;
    if (!parseDateStr(s.substr(0, 10), out)) return false;
    std::tm t2{};
    if (!parseTimeStr(s.substr(11, 8), t2)) return false;
    out.tm_hour = t2.tm_hour;
    out.tm_min  = t2.tm_min;
    out.tm_sec  = t2.tm_sec;
    return true;
}

// ── Bestandteile extrahieren ─────────────────────────────────────────────────

// Gibt eine Zahl als String zurück (YEAR, MONTH, DAY, HOUR, MINUTE, SECOND)
inline std::string extractPart(const std::string& valRaw, const std::string& part) {
    const std::string val = stripQuotes(valRaw);
    std::tm t{};
    bool ok = false;
    if (val.size() >= 19) ok = parseDatetimeStr(val, t);
    else if (val.size() >= 10 && val[4] == '-') ok = parseDateStr(val, t);
    else if (val.size() >= 5  && val[2] == ':') ok = parseTimeStr(val, t);
    if (!ok) return "NULL";

    // part in Großbuchstaben
    std::string p = part;
    for (char& c : p) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (p == "YEAR")   return std::to_string(t.tm_year + 1900);
    if (p == "MONTH")  return std::to_string(t.tm_mon + 1);
    if (p == "DAY")    return std::to_string(t.tm_mday);
    if (p == "HOUR")   return std::to_string(t.tm_hour);
    if (p == "MINUTE") return std::to_string(t.tm_min);
    if (p == "SECOND") return std::to_string(t.tm_sec);
    return "NULL";
}

// ── DATEDIFF ─────────────────────────────────────────────────────────────────

// Wandelt tm (nur Datum) in Tage seit Epoche (vereinfacht)
inline long dateToEpochDays(const std::tm& t) {
    // mktime benötigt komplettes tm; setze Zeit auf Mittag um DST-Sprünge zu vermeiden
    std::tm copy = t;
    copy.tm_hour = 12; copy.tm_min = 0; copy.tm_sec = 0;
    copy.tm_isdst = -1;
    std::time_t tt = std::mktime(&copy);
    if (tt == -1) return 0;
    return static_cast<long>(tt) / 86400;
}

// DATEDIFF(date1, date2) → date1 - date2 in Tagen
inline std::string dateDiff(const std::string& d1raw, const std::string& d2raw) {
    const std::string d1 = stripQuotes(d1raw);
    const std::string d2 = stripQuotes(d2raw);
    std::tm t1{}, t2{};
    bool ok1 = parseDateStr(d1, t1);
    bool ok2 = parseDateStr(d2, t2);
    // auch DateTime-Strings
    if (!ok1 && d1.size() >= 19) ok1 = parseDatetimeStr(d1, t1);
    if (!ok2 && d2.size() >= 19) ok2 = parseDatetimeStr(d2, t2);
    if (!ok1 || !ok2) return "NULL";
    long diff = dateToEpochDays(t1) - dateToEpochDays(t2);
    return std::to_string(diff);
}

// ── DATE_ADD ─────────────────────────────────────────────────────────────────

// DATE_ADD(date, INTERVAL n UNIT)  unit: DAY, MONTH, YEAR, HOUR, MINUTE, SECOND
// Gibt neues Datum als "YYYY-MM-DD" zurück (oder datetime wenn Zeit vorhanden)
inline std::string dateAdd(const std::string& dateValRaw,
                           long n,
                           const std::string& unitRaw) {
    const std::string dateVal = stripQuotes(dateValRaw);
    const std::string unit    = stripQuotes(unitRaw);
    std::tm t{};
    bool hasTime = false;
    bool ok = false;
    if (dateVal.size() >= 19) { ok = parseDatetimeStr(dateVal, t); hasTime = true; }
    if (!ok) ok = parseDateStr(dateVal, t);
    if (!ok) return "NULL";

    // unit normalise
    std::string u = unit;
    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (u == "DAY")    t.tm_mday += static_cast<int>(n);
    else if (u == "MONTH")  t.tm_mon  += static_cast<int>(n);
    else if (u == "YEAR")   t.tm_year += static_cast<int>(n);
    else if (u == "HOUR")   { t.tm_hour += static_cast<int>(n); hasTime = true; }
    else if (u == "MINUTE") { t.tm_min  += static_cast<int>(n); hasTime = true; }
    else if (u == "SECOND") { t.tm_sec  += static_cast<int>(n); hasTime = true; }
    else return "NULL";

    t.tm_isdst = -1;
    std::mktime(&t);  // normalisiert (overflow days/months)

    char buf[24];
    if (hasTime) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    }
    return buf;
}

// ── DATE_FORMAT ──────────────────────────────────────────────────────────────

// DATE_FORMAT(date, '%Y-%m-%d') → formatierter String (MySQL-kompatibel)
inline std::string dateFormat(const std::string& dateValRaw, std::string fmt) {
    const std::string dateVal = stripQuotes(dateValRaw);
    std::tm t{};
    bool ok = false;
    if (dateVal.size() >= 19) ok = parseDatetimeStr(dateVal, t);
    if (!ok) ok = parseDateStr(dateVal, t);
    if (!ok) ok = parseTimeStr(dateVal, t);
    if (!ok) return "NULL";

    std::string result;
    result.reserve(fmt.size() + 8);
    char buf[8];
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            ++i;
            switch (fmt[i]) {
                case 'Y': std::snprintf(buf, sizeof(buf), "%04d", t.tm_year + 1900); result += buf; break;
                case 'y': std::snprintf(buf, sizeof(buf), "%02d", (t.tm_year + 1900) % 100); result += buf; break;
                case 'm': std::snprintf(buf, sizeof(buf), "%02d", t.tm_mon + 1); result += buf; break;
                case 'c': std::snprintf(buf, sizeof(buf), "%d",   t.tm_mon + 1); result += buf; break;
                case 'd': std::snprintf(buf, sizeof(buf), "%02d", t.tm_mday); result += buf; break;
                case 'e': std::snprintf(buf, sizeof(buf), "%d",   t.tm_mday); result += buf; break;
                case 'H': std::snprintf(buf, sizeof(buf), "%02d", t.tm_hour); result += buf; break;
                case 'h': std::snprintf(buf, sizeof(buf), "%02d", t.tm_hour % 12 ? t.tm_hour % 12 : 12); result += buf; break;
                case 'i': std::snprintf(buf, sizeof(buf), "%02d", t.tm_min);  result += buf; break;
                case 's': std::snprintf(buf, sizeof(buf), "%02d", t.tm_sec);  result += buf; break;
                case 'j': std::snprintf(buf, sizeof(buf), "%d",   t.tm_yday + 1); result += buf; break;
                case 'W': {
                    static const char* days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
                    result += days[t.tm_wday]; break;
                }
                case 'a': {
                    static const char* sdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                    result += sdays[t.tm_wday]; break;
                }
                case 'M': {
                    static const char* months[] = {"January","February","March","April","May","June",
                                                    "July","August","September","October","November","December"};
                    if (t.tm_mon >= 0 && t.tm_mon < 12) { result += months[t.tm_mon]; } break;
                }
                case 'b': {
                    static const char* smonths[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                                     "Jul","Aug","Sep","Oct","Nov","Dec"};
                    if (t.tm_mon >= 0 && t.tm_mon < 12) { result += smonths[t.tm_mon]; } break;
                }
                case 'p': result += (t.tm_hour < 12 ? "AM" : "PM"); break;
                case '%': result += '%'; break;
                default:  result += '%'; result += fmt[i]; break;
            }
        } else {
            result += fmt[i];
        }
    }
    return result;
}

} // namespace dateutils
} // namespace milansql
