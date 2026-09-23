#pragma once
#include <graphlab/runtime.hpp>
namespace graphlab::application_telemetry {
void initialize(sqlite3 *);
void ingest(sqlite3 *, const std::string &run, const std::string &node, const runtime::Json &report,
            const std::string &collector);
runtime::Json query(sqlite3 *, const std::string &run, const runtime::Json &params,
                    const std::string &collector);
} // namespace graphlab::application_telemetry
