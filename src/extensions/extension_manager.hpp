#pragma once
// ============================================================
// extension_manager.hpp — Extension System for MilanSQL
// Phase 90: Manages built-in extensions:
//   milansql_math, milansql_crypto, milansql_uuid, milansql_text
// ============================================================

#include <string>
#include <set>
#include <map>
#include <functional>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

#include "builtin/math_ext.hpp"
#include "builtin/crypto_ext.hpp"
#include "builtin/uuid_ext.hpp"
#include "builtin/text_ext.hpp"

namespace milansql {

class ExtensionManager {
public:
    // Try to evaluate a function.
    // Returns {true, result} if handled, {false, ""} if not handled.
    std::pair<bool, std::string> tryEvaluate(const std::string& funcName,
                                              const std::vector<std::string>& args) const {
        std::string fn = toUpper(funcName);
        auto it = funcs_.find(fn);
        if (it == funcs_.end()) return {false, ""};
        return {true, it->second(args)};
    }

    bool isExtensionFunc(const std::string& funcName) const {
        return funcs_.count(toUpper(funcName)) > 0;
    }

    bool createExtension(const std::string& name) {
        std::string lower = toLower(name);
        if (lower == "milansql_math") {
            if (!loaded_.count(lower)) { loaded_.insert(lower); registerMath(); }
            return true;
        }
        if (lower == "milansql_crypto") {
            if (!loaded_.count(lower)) { loaded_.insert(lower); registerCrypto(); }
            return true;
        }
        if (lower == "milansql_uuid") {
            if (!loaded_.count(lower)) { loaded_.insert(lower); registerUuid(); }
            return true;
        }
        if (lower == "milansql_text") {
            if (!loaded_.count(lower)) { loaded_.insert(lower); registerText(); }
            return true;
        }
        return false;  // unknown extension
    }

    void dropExtension(const std::string& name) {
        std::string lower = toLower(name);
        if (!loaded_.count(lower)) return;
        loaded_.erase(lower);
        if (lower == "milansql_math")   unregisterMath();
        if (lower == "milansql_crypto") unregisterCrypto();
        if (lower == "milansql_uuid")   unregisterUuid();
        if (lower == "milansql_text")   unregisterText();
    }

    bool isLoaded(const std::string& name) const {
        return loaded_.count(toLower(name)) > 0;
    }

    std::string showExtensions() const {
        if (loaded_.empty()) return "(no extensions loaded)";
        std::ostringstream oss;
        for (const auto& ext : loaded_) oss << ext << "\n";
        std::string s = oss.str();
        if (!s.empty() && s.back() == '\n') s.pop_back();
        return s;
    }

    std::vector<std::string> registeredFunctions() const {
        std::vector<std::string> names;
        names.reserve(funcs_.size());
        for (const auto& kv : funcs_) names.push_back(kv.first);
        return names;
    }

private:
    std::set<std::string> loaded_;
    std::map<std::string, std::function<std::string(const std::vector<std::string>&)>> funcs_;

    static std::string toUpper(std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string toLower(std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    void registerMath() {
        funcs_["PI"]        = [](const std::vector<std::string>& a){ return math_ext::evalMath("PI",        a); };
        funcs_["EXP"]       = [](const std::vector<std::string>& a){ return math_ext::evalMath("EXP",       a); };
        funcs_["LOG"]       = [](const std::vector<std::string>& a){ return math_ext::evalMath("LOG",       a); };
        funcs_["LOG10"]     = [](const std::vector<std::string>& a){ return math_ext::evalMath("LOG10",     a); };
        funcs_["SIN"]       = [](const std::vector<std::string>& a){ return math_ext::evalMath("SIN",       a); };
        funcs_["COS"]       = [](const std::vector<std::string>& a){ return math_ext::evalMath("COS",       a); };
        funcs_["TAN"]       = [](const std::vector<std::string>& a){ return math_ext::evalMath("TAN",       a); };
        funcs_["RADIANS"]   = [](const std::vector<std::string>& a){ return math_ext::evalMath("RADIANS",   a); };
        funcs_["DEGREES"]   = [](const std::vector<std::string>& a){ return math_ext::evalMath("DEGREES",   a); };
        funcs_["FACTORIAL"] = [](const std::vector<std::string>& a){ return math_ext::evalMath("FACTORIAL", a); };
    }

    void unregisterMath() {
        for (const auto& n : {"PI","EXP","LOG","LOG10","SIN","COS","TAN",
                               "RADIANS","DEGREES","FACTORIAL"})
            funcs_.erase(n);
    }

    void registerCrypto() {
        funcs_["MD5"]    = [](const std::vector<std::string>& a){ return crypto_ext::evalCrypto("MD5",    a); };
        funcs_["SHA1"]   = [](const std::vector<std::string>& a){ return crypto_ext::evalCrypto("SHA1",   a); };
        funcs_["SHA256"] = [](const std::vector<std::string>& a){ return crypto_ext::evalCrypto("SHA256", a); };
        funcs_["ENCODE"] = [](const std::vector<std::string>& a){ return crypto_ext::evalCrypto("ENCODE", a); };
        funcs_["DECODE"] = [](const std::vector<std::string>& a){ return crypto_ext::evalCrypto("DECODE", a); };
    }

    void unregisterCrypto() {
        for (const auto& n : {"MD5","SHA1","SHA256","ENCODE","DECODE"})
            funcs_.erase(n);
    }

    void registerUuid() {
        funcs_["GEN_RANDOM_UUID"]   = [](const std::vector<std::string>& a){ return uuid_ext::evalUuid("GEN_RANDOM_UUID",   a); };
        funcs_["UUID_GENERATE_V1"]  = [](const std::vector<std::string>& a){ return uuid_ext::evalUuid("UUID_GENERATE_V1",  a); };
        funcs_["IS_VALID_UUID"]     = [](const std::vector<std::string>& a){ return uuid_ext::evalUuid("IS_VALID_UUID",     a); };
    }

    void unregisterUuid() {
        for (const auto& n : {"GEN_RANDOM_UUID","UUID_GENERATE_V1","IS_VALID_UUID"})
            funcs_.erase(n);
    }

    void registerText() {
        funcs_["SOUNDEX"]      = [](const std::vector<std::string>& a){ return text_ext::evalText("SOUNDEX",      a); };
        funcs_["LEVENSHTEIN"]  = [](const std::vector<std::string>& a){ return text_ext::evalText("LEVENSHTEIN",  a); };
        funcs_["SIMILARITY"]   = [](const std::vector<std::string>& a){ return text_ext::evalText("SIMILARITY",   a); };
        funcs_["INITCAP"]      = [](const std::vector<std::string>& a){ return text_ext::evalText("INITCAP",      a); };
        funcs_["REPEAT"]       = [](const std::vector<std::string>& a){ return text_ext::evalText("REPEAT",       a); };
        funcs_["LPAD"]         = [](const std::vector<std::string>& a){ return text_ext::evalText("LPAD",         a); };
        funcs_["RPAD"]         = [](const std::vector<std::string>& a){ return text_ext::evalText("RPAD",         a); };
    }

    void unregisterText() {
        for (const auto& n : {"SOUNDEX","LEVENSHTEIN","SIMILARITY","INITCAP","REPEAT","LPAD","RPAD"})
            funcs_.erase(n);
    }
};

} // namespace milansql
