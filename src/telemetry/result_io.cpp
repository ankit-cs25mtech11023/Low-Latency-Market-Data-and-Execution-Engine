#include "lle/telemetry/result_io.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include "lle/build_info.hpp"

namespace lle {
namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

File open_for_write(const std::string& path) {
    if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);
    File f(std::fopen(path.c_str(), "w"));
    if (!f) throw std::runtime_error("cannot open " + path + " for writing");
    return f;
}

}  // namespace

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string build_info_json() {
    return std::string(R"({"git_sha":")") + json_escape(build_info::git_sha) + R"(","git_dirty":)" +
           (build_info::git_dirty ? "true" : "false") + R"(,"build_type":")" +
           json_escape(build_info::build_type) + R"(","compiler":")" + json_escape(build_info::compiler) +
           R"(","cxx_flags":")" + json_escape(build_info::cxx_flags) + R"(","sanitizer":")" +
           json_escape(build_info::sanitizer) + R"("})";
}

void write_run(const std::string& prefix, const Histogram& hist, const TscCalibration& cal,
               const std::string& params_json) {
    {
        File f = open_for_write(prefix + ".hist.csv");
        hist.write_csv(f.get());
    }
    File f = open_for_write(prefix + ".meta.json");
    std::fprintf(f.get(),
                 "{\n  \"build\": %s,\n  \"tsc_features\": %s,\n  \"tsc_calibration\": %s,\n"
                 "  \"summary_ticks\": {\"count\": %llu, \"min\": %llu, \"max\": %llu, \"mean\": %.3f},\n"
                 "  \"params\": %s\n}\n",
                 build_info_json().c_str(), detect_tsc_features().to_json().c_str(), cal.to_json().c_str(),
                 static_cast<unsigned long long>(hist.count()), static_cast<unsigned long long>(hist.min()),
                 static_cast<unsigned long long>(hist.max()), hist.mean(), params_json.c_str());
}

}  // namespace lle
