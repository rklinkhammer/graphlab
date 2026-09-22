#pragma once
#include <graphlab/runtime.hpp>
#include <lab_support/telemetry.hpp>
namespace graphlab::telemetry {
std::string boot();
std::uint64_t monotonic();
std::int64_t wall();
void initialize(sqlite3 *);
void append(sqlite3 *, const runtime::Json &);
runtime::Json query(sqlite3 *, const std::string &run, const runtime::Json &params);
runtime::Json validate_fault(const runtime::Json &);
} // namespace graphlab::telemetry
