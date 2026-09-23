#pragma once
#include <lab_support/contracts.hpp>
namespace lab_support::application_telemetry {
// Throws std::invalid_argument for unsupported, malformed or oversized reports.
Json validate(const Json &);
// Reports and envelopes use decimal strings for every cumulative uint64 value.
// The collector supplies identity/epoch; workload timestamps never become wall time.
Json derive(const Json &previous, const Json &report, const std::string &collector);
inline constexpr std::size_t max_report_bytes = 3072;
} // namespace lab_support::application_telemetry
