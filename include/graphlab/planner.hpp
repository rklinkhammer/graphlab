#pragma once
#include <lab_support/contracts.hpp>
namespace graphlab {
// Produces data only. No process, network, filesystem-write or Docker adapter is linked.
lab_support::Result<lab_support::Json> plan(const lab_support::Json &topology,
                                            const lab_support::Json &artifacts);
} // namespace graphlab
