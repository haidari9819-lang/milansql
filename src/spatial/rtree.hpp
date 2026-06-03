#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

// ============================================================
// rtree.hpp — Phase 132: R-Tree Spatial Index V2
// ============================================================

namespace milansql {

struct MBR {
    double minLat, maxLat, minLng, maxLng;

    bool contains(double lat, double lng) const {
        return lat >= minLat && lat <= maxLat && lng >= minLng && lng <= maxLng;
    }

    bool intersects(const MBR& o) const {
        return !(o.minLat > maxLat || o.maxLat < minLat ||
                 o.minLng > maxLng || o.maxLng < minLng);
    }

    void expand(double lat, double lng) {
        minLat = std::min(minLat, lat); maxLat = std::max(maxLat, lat);
        minLng = std::min(minLng, lng); maxLng = std::max(maxLng, lng);
    }

    static MBR fromPoint(double lat, double lng, double marginDeg = 0.0) {
        return {lat - marginDeg, lat + marginDeg, lng - marginDeg, lng + marginDeg};
    }
};

struct RTreeEntry { uint64_t id; double lat, lng; };

struct RTreeNode {
    MBR mbr = {1e9, -1e9, 1e9, -1e9};
    std::vector<RTreeEntry> entries; // leaf entries
    bool isLeaf = true;
    static constexpr int MAX_ENTRIES = 8;
};

class RTree {
public:
    void insert(uint64_t id, double lat, double lng) {
        entries_.push_back({id, lat, lng});
        // Expand root MBR
        root_.mbr.minLat = std::min(root_.mbr.minLat, lat);
        root_.mbr.maxLat = std::max(root_.mbr.maxLat, lat);
        root_.mbr.minLng = std::min(root_.mbr.minLng, lng);
        root_.mbr.maxLng = std::max(root_.mbr.maxLng, lng);
        root_.entries.push_back({id, lat, lng});
    }

    std::vector<uint64_t> search(const MBR& box) const {
        std::vector<uint64_t> result;
        for (auto& e : entries_) {
            if (box.contains(e.lat, e.lng)) result.push_back(e.id);
        }
        return result;
    }

    size_t size() const { return entries_.size(); }

private:
    RTreeNode root_;
    std::vector<RTreeEntry> entries_; // flat for simplicity
};

} // namespace milansql
