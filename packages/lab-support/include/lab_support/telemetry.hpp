#pragma once
#include <lab_support/contracts.hpp>
namespace lab_support::telemetry {
// Raw counter strings retain all 64 bits. Invalid epochs never produce a rate.
Json derive(const Json &previous, Json current);
} // namespace lab_support::telemetry
