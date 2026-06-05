#pragma once
#include <string>
#include <map>
#include <vector>

namespace milansql {

struct RoutineParam {
    std::string name;
    std::string type;
};

struct Routine {
    std::string name;
    std::string kind; // "FUNCTION" or "PROCEDURE"
    std::vector<RoutineParam> params;
    std::string returnType; // empty for procedures
    std::string body;
    std::string language = "sql";
    std::string createdAt;
};

class RoutineManager {
    std::map<std::string, Routine> routines_;

public:
    void createRoutine(const Routine& r) {
        routines_[r.name] = r;
    }

    bool hasRoutine(const std::string& name) const {
        return routines_.count(name) > 0;
    }

    const Routine* getRoutine(const std::string& name) const {
        auto it = routines_.find(name);
        return it != routines_.end() ? &it->second : nullptr;
    }

    bool dropRoutine(const std::string& name) {
        return routines_.erase(name) > 0;
    }

    std::vector<Routine> getByKind(const std::string& kind) const {
        std::vector<Routine> result;
        for (auto& [k, v] : routines_)
            if (v.kind == kind) result.push_back(v);
        return result;
    }

    const std::map<std::string, Routine>& getAll() const { return routines_; }
};

} // namespace milansql
