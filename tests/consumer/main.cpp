#include <lab_support/contracts.hpp>
#include <lab_support/telemetry.hpp>
int main() {
  const auto sample = lab_support::telemetry::derive(nullptr, {{"valid", true}});
  if (!sample["forwardBitsPerSecond"].is_null() || sample["gapReason"] != "first_sample")
    return 3;
  const auto value = lab_support::parse_document("node: independent\n");
  if (!value || (*value)["node"] != "independent")
    return 1;
  return lab_support::digest(lab_support::Json::object()) ==
                 "sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"
             ? 0
             : 2;
}
