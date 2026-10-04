#pragma once
// Minimal "--key value" / "--flag" argument parser for experiment drivers.
// Startup-only code: allocation and exceptions are fine here.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lle {

class Cli {
public:
    Cli(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a.size() < 3 || !a.starts_with("--"))
                throw std::invalid_argument("unexpected argument: " + std::string(a));
            std::string key(a.substr(2));
            if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--"))
                kv_[key] = argv[++i];
            else
                kv_[key] = "1";
        }
    }

    [[nodiscard]] bool has(const std::string& k) const { return kv_.count(k) != 0; }

    [[nodiscard]] std::string str(const std::string& k, const std::string& def) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? def : it->second;
    }
    [[nodiscard]] std::string str(const std::string& k) const {
        auto it = kv_.find(k);
        if (it == kv_.end()) throw std::invalid_argument("missing required --" + k);
        return it->second;
    }
    [[nodiscard]] std::int64_t i64(const std::string& k, std::int64_t def) const {
        return has(k) ? std::stoll(str(k)) : def;
    }
    [[nodiscard]] double f64(const std::string& k, double def) const { return has(k) ? std::stod(str(k)) : def; }

    // Comma-separated list, e.g. "--cpus 0,2,4".
    [[nodiscard]] std::vector<std::string> list(const std::string& k) const {
        std::vector<std::string> out;
        std::string cur;
        for (char c : str(k)) {
            if (c == ',') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

private:
    std::map<std::string, std::string> kv_;
};

}  // namespace lle
