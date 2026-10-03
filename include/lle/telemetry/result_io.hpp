#pragma once
// Writing benchmark results to disk (always off the hot path).
//
// Every run produces two files next to each other:
//   <prefix>.hist.csv   non-empty histogram buckets (low,high,count), in TSC ticks
//   <prefix>.meta.json  how the binary was built, the TSC calibration, and run parameters
// scripts/analyse.py turns ticks into ns with the calibration from the meta file, and
// scripts/run_bench.py adds the machine-level environment JSON (env_capture.sh).

#include <string>

#include "lle/core/tsc.hpp"
#include "lle/telemetry/histogram.hpp"

namespace lle {

// JSON object with build info (git SHA, compiler, flags, sanitizer) for this binary.
[[nodiscard]] std::string build_info_json();

// Writes <prefix>.hist.csv and <prefix>.meta.json. `params_json` must be a JSON object
// describing the run (variant, cpu, sample count, ...). Throws std::runtime_error on I/O failure.
void write_run(const std::string& prefix, const Histogram& hist, const TscCalibration& cal,
               const std::string& params_json);

// Escapes a string for embedding in JSON.
[[nodiscard]] std::string json_escape(const std::string& s);

}  // namespace lle
