#include <lab_support/contracts.hpp>
namespace lab_support {
Result<unsigned> gate_protocol_minor(const Json &response) {
  if (!response.is_object() || !response.contains("apiVersion") ||
      response["apiVersion"] != "graphlab.gate/v1")
    return std::unexpected(Error{"unsupported_gate_major", "$.apiVersion", "gate v1 required"});
  auto minor = response.value("protocolMinor", Json(0));
  if ((!minor.is_number_unsigned() && !minor.is_number_integer()) || minor < 0 || minor > 1)
    return std::unexpected(
        Error{"unsupported_gate_minor", "$.protocolMinor", "gate minor 0 or 1 required"});
  if (response.contains("requiredFeatures")) {
    const auto &features = response["requiredFeatures"];
    if (!features.is_array())
      return std::unexpected(
          Error{"invalid_gate_features", "$.requiredFeatures", "array required"});
    for (const auto &feature : features)
      if (!feature.is_string() || (feature != "quiesce" && feature != "traffic-lease"))
        return std::unexpected(Error{"unsupported_gate_feature", "$.requiredFeatures",
                                     "required feature unavailable"});
  }
  return minor.get<unsigned>();
}
} // namespace lab_support
