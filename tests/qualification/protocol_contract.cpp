#include <iostream>
#include <lab_support/contracts.hpp>
using namespace lab_support;
int main() {
  Json legacy = {{"apiVersion", "graphlab.gate/v1"}};
  auto a = gate_protocol_minor(legacy);
  if (!a || *a != 0)
    return 1;
  Json current = legacy;
  current["protocolMinor"] = 1;
  current["futureOptional"] = true;
  auto b = gate_protocol_minor(current);
  if (!b || *b != 1)
    return 1;
  current["requiredFeatures"] = Json::array({"traffic-lease", "quiesce"});
  if (!gate_protocol_minor(current))
    return 1;
  for (auto bad : {Json(-1), Json(2), Json("1"), Json(nullptr), Json(1.5)}) {
    auto value = legacy;
    value["protocolMinor"] = bad;
    if (gate_protocol_minor(value))
      return 1;
  }
  for (auto bad : {Json("future"), Json::array({"unknown"}), Json::array({1})}) {
    auto value = current;
    value["requiredFeatures"] = bad;
    if (gate_protocol_minor(value))
      return 1;
  }
  for (auto bad : {Json(nullptr), Json::object(), Json{{"apiVersion", "graphlab.gate/v2"}}})
    if (gate_protocol_minor(bad))
      return 1;
  std::cout
      << "PASS legacy/current minors, additive fields, required features and incompatible majors\n";
}
