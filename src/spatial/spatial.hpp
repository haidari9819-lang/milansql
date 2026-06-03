#pragma once

#include <string>
#include <cmath>
#include <stdexcept>
#include <sstream>

// ============================================================
// spatial.hpp — Phase 70: Spatial Index + POINT type
// ============================================================

namespace milansql {

struct SpatialUtils {
    // ── Point parsing ──────────────────────────────────────

    // Parse "POINT(lat lng)" or "POINT(lat, lng)" → {lat, lng}
    static std::pair<double,double> parsePoint(const std::string& s) {
        // Find first '('
        auto p = s.find('(');
        if (p == std::string::npos) return {0.0, 0.0};
        auto q = s.rfind(')');
        if (q == std::string::npos) return {0.0, 0.0};
        std::string inner = s.substr(p + 1, q - p - 1);
        // Replace comma with space
        for (char& c : inner) if (c == ',') c = ' ';
        double lat = 0.0, lng = 0.0;
        std::istringstream iss(inner);
        iss >> lat >> lng;
        return {lat, lng};
    }

    // Normalize a POINT literal to canonical "POINT(lat lng)" form
    static std::string normalizePoint(const std::string& s) {
        auto [lat, lng] = parsePoint(s);
        return pointToString(lat, lng);
    }

    // "POINT(lat lng)" canonical string
    static std::string pointToString(double lat, double lng) {
        std::ostringstream oss;
        oss << "POINT(" << lat << " " << lng << ")";
        return oss.str();
    }

    // ── Haversine distance ──────────────────────────────────

    static constexpr double PI  = 3.14159265358979323846;
    static constexpr double R_KM = 6371.0;  // Earth radius in km

    static double toRad(double deg) { return deg * PI / 180.0; }

    // Returns distance in km between two lat/lng points
    static double haversine(double lat1, double lng1, double lat2, double lng2) {
        double dlat = toRad(lat2 - lat1);
        double dlng = toRad(lng2 - lng1);
        double a = std::sin(dlat / 2) * std::sin(dlat / 2)
                 + std::cos(toRad(lat1)) * std::cos(toRad(lat2))
                 * std::sin(dlng / 2) * std::sin(dlng / 2);
        double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        return R_KM * c;
    }

    // ── String helpers ──────────────────────────────────────

    // ST_DISTANCE(p1_str, p2_str) → km as string
    static std::string stDistance(const std::string& p1, const std::string& p2) {
        auto [lat1, lng1] = parsePoint(p1);
        auto [lat2, lng2] = parsePoint(p2);
        double km = haversine(lat1, lng1, lat2, lng2);
        std::ostringstream oss;
        oss << km;
        return oss.str();
    }

    // ST_X(point_str) → latitude as string
    static std::string stX(const std::string& s) {
        auto [lat, lng] = parsePoint(s);
        (void)lng;
        std::ostringstream oss; oss << lat; return oss.str();
    }

    // ST_Y(point_str) → longitude as string
    static std::string stY(const std::string& s) {
        auto [lat, lng] = parsePoint(s);
        (void)lat;
        std::ostringstream oss; oss << lng; return oss.str();
    }

    // ST_ASTEXT(point_str) → "POINT(lat lng)"
    static std::string stAsText(const std::string& s) {
        return normalizePoint(s);
    }

    // ST_WITHIN(point, center, radius_km) → "1" or "0"
    static std::string stWithin(const std::string& p, const std::string& center, double radiusKm) {
        auto [lat1, lng1] = parsePoint(p);
        auto [lat2, lng2] = parsePoint(center);
        double km = haversine(lat1, lng1, lat2, lng2);
        return km <= radiusKm ? "1" : "0";
    }

    // Check if a string looks like a POINT literal (starts with POINT()
    static bool isPointLiteral(const std::string& s) {
        if (s.size() < 7) return false;
        std::string u;
        for (size_t i = 0; i < 5 && i < s.size(); ++i)
            u += static_cast<char>(std::toupper((unsigned char)s[i]));
        return u == "POINT";
    }

    // ── Phase 132: V2 Spatial functions ──────────────────────────

    // ST_WITHIN(lat, lng, minLat, maxLat, minLng, maxLng) — check if point inside bounding box
    static bool stWithin(double lat, double lng, double minLat, double maxLat, double minLng, double maxLng) {
        return lat >= minLat && lat <= maxLat && lng >= minLng && lng <= maxLng;
    }

    // ST_BBOX: create bounding box from center + radius in km
    // Returns as string "BBOX(minLat,maxLat,minLng,maxLng)"
    static std::string stBbox(double centerLat, double centerLng, double radiusKm) {
        double latDeg = radiusKm / 111.0;
        double lngDeg = radiusKm / (111.0 * std::cos(centerLat * PI / 180.0));
        return "BBOX(" + std::to_string(centerLat - latDeg) + "," +
                         std::to_string(centerLat + latDeg) + "," +
                         std::to_string(centerLng - lngDeg) + "," +
                         std::to_string(centerLng + lngDeg) + ")";
    }

    // ST_GEOHASH: encode lat/lng as geohash string
    static std::string stGeohash(double lat, double lng, int precision = 6) {
        static const char* BASE32 = "0123456789bcdefghjkmnpqrstuvwxyz";
        std::string hash;
        double minLat2 = -90, maxLat2 = 90, minLng2 = -180, maxLng2 = 180;
        int bits = 0, bit = 0;
        bool isLng = true;
        while ((int)hash.size() < precision) {
            if (isLng) {
                double mid = (minLng2 + maxLng2) / 2.0;
                if (lng >= mid) { bit = (bit << 1) | 1; minLng2 = mid; }
                else { bit = bit << 1; maxLng2 = mid; }
            } else {
                double mid = (minLat2 + maxLat2) / 2.0;
                if (lat >= mid) { bit = (bit << 1) | 1; minLat2 = mid; }
                else { bit = bit << 1; maxLat2 = mid; }
            }
            isLng = !isLng;
            if (++bits == 5) { hash += BASE32[bit]; bits = 0; bit = 0; }
        }
        return hash;
    }

    // ST_BEARING: compass bearing from point A to point B (degrees)
    static double stBearing(double lat1, double lng1, double lat2, double lng2) {
        double dLng = (lng2 - lng1) * PI / 180.0;
        double rlat1 = lat1 * PI / 180.0;
        double rlat2 = lat2 * PI / 180.0;
        double y = std::sin(dLng) * std::cos(rlat2);
        double x = std::cos(rlat1) * std::sin(rlat2) -
                    std::sin(rlat1) * std::cos(rlat2) * std::cos(dLng);
        double bearing = std::atan2(y, x) * 180.0 / PI;
        return std::fmod(bearing + 360.0, 360.0);
    }

    // ST_DESTINATION: destination point given start, distance (km), bearing (degrees)
    // Returns "POINT(lat,lng)"
    static std::string stDestination(double lat, double lng, double distKm, double bearingDeg) {
        double d = distKm / R_KM;
        double bearing = bearingDeg * PI / 180.0;
        double rlat = lat * PI / 180.0;
        double rlng = lng * PI / 180.0;
        double lat2 = std::asin(std::sin(rlat)*std::cos(d) +
                                 std::cos(rlat)*std::sin(d)*std::cos(bearing));
        double lng2 = rlng + std::atan2(std::sin(bearing)*std::sin(d)*std::cos(rlat),
                                         std::cos(d) - std::sin(rlat)*std::sin(lat2));
        return "POINT(" + std::to_string(lat2 * 180.0 / PI) + "," +
                          std::to_string(lng2 * 180.0 / PI) + ")";
    }
};

} // namespace milansql
