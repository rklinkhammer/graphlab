#include <fcntl.h>
#include <graphlab/capture.hpp>
#include <graphlab/message_history.hpp>
#include <graphlab/packet_history.hpp>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
namespace graphlab::messages {
using runtime::Failure;
Json correlate(const Json &run, const Json &event, const Json &p) {
  if (event.at("runId") != run.at("id"))
    throw Failure("message_run_mismatch", 404);
  std::string edge = p.value("edge", "");
  if (edge.size() > 128)
    throw Failure("invalid_message_edge");
  bool known = edge.empty();
  for (const auto &e : run["topology"]["edges"])
    if (e["id"] == edge)
      known = true;
  if (!known)
    throw Failure("message_edge_not_found", 404);
  auto captures = run.value("captureHistory", Json::array());
  for (const auto &c : run.value("captures", Json::array()))
    captures.push_back(c);
  Json matches = Json::array(), errors = Json::array();
  bool complete = true;
  std::size_t budget = 16 * 1024 * 1024, segments = 0;
  std::uint64_t examined = 0;
  auto observation = event.at("observation");
  for (const auto &c : captures) {
    if (!edge.empty() && c["edge"] != edge)
      continue;
    try {
      auto manifest = console::load(std::filesystem::path(c.at("directory").get<std::string>()) /
                                    "manifest.json");
      if (c.at("runId") != run["id"])
        throw Failure("message_capture_run_mismatch");
      for (auto key : {"id", "runId", "edge", "epoch", "mapping", "bootId"})
        if (manifest.at(key) != c.at(key))
          throw Failure("message_capture_identity_changed");
      if (manifest.value("state", "") != "closed")
        complete = false;
      for (const auto &s : manifest.at("segments")) {
        if (segments++ >= 16) {
          complete = false;
          break;
        }
        auto sequence = s.at("sequence").get<std::string>();
        if (sequence.empty() || sequence.size() > 8 ||
            sequence.find_first_not_of("0123456789") != std::string::npos)
          throw Failure("message_capture_segment_invalid");
        auto path =
            std::filesystem::path(c.at("directory").get<std::string>()) / (sequence + ".pcapng");
        int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
          throw Failure("message_capture_unavailable");
        struct Guard {
          int fd;
          ~Guard() { close(fd); }
        } guard{fd};
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<std::uint64_t>(st.st_size) > budget) {
          complete = false;
          errors.push_back("capture_scan_byte_limit_or_invalid_file");
          continue;
        }
        std::vector<unsigned char> bytes(st.st_size);
        budget -= bytes.size();
        std::size_t offset = 0;
        while (offset < bytes.size()) {
          auto n = pread(fd, bytes.data() + offset, bytes.size() - offset, offset);
          if (n <= 0)
            throw Failure("message_capture_read");
          offset += n;
        }
        unsigned char hash[32];
        unsigned length = 0;
        if (!EVP_Digest(bytes.data(), bytes.size(), hash, &length, EVP_sha256(), nullptr))
          throw Failure("message_capture_hash");
        std::string digest = "sha256:";
        for (auto b : hash) {
          digest += "0123456789abcdef"[b >> 4];
          digest += "0123456789abcdef"[b & 15];
        }
        if (digest != s.at("sha256").get<std::string>() ||
            std::to_string(bytes.size()) != s.at("size").get<std::string>())
          throw Failure("message_capture_checksum_mismatch");
        auto id = c.at("id").get<std::string>() + "-" + sequence;
        auto decoded = packets::decode(bytes, {{"decodeMessageIdentifiers", true}}, 2000);
        if (decoded.value("omittedRecords", std::string("0")) != "0")
          complete = false;
        for (const auto &packet : decoded["items"]) {
          ++examined;
          if (packet.value("truncated", false)) {
            complete = false;
            continue;
          }
          auto headers = packet["headers"];
          if (!headers.contains("fixtureIdentifier"))
            continue;
          if (headers["fixtureIdentifier"]["messageId"] != observation["messageId"] ||
              headers["fixtureIdentifier"]["stream"] != observation["stream"] ||
              headers["fixtureIdentifier"]["phase"] != observation["phase"])
            continue;
          if (matches.size() >= 64) {
            complete = false;
            break;
          }
          matches.push_back(
              {{"artifactId", id},
               {"edge", c["edge"]},
               {"captureEpoch", c["epoch"]},
               {"packetIndex", packet["packetIndex"]},
               {"blockOffset", packet["blockOffset"]},
               {"sha256", digest},
               {"timestampUnixMicros", packet["timestampUnixMicros"]},
               {"matchBasis",
                "GLM1/v1 exact 48-hex identifier and stream in verified UDP payload"}});
        }
      }
    } catch (const std::exception &e) {
      complete = false;
      if (errors.size() < 16)
        errors.push_back(std::string(e.what()).substr(0, 128));
    }
  }
  // Multiple capture points, echo packets, retries and reused identifiers are never collapsed.
  bool gaps = run.value("captureCoverage", "").find("incomplete") != std::string::npos ||
              run.value("captureCoverage", "").find("interrupted") != std::string::npos;
  std::string status = matches.empty()                            ? "unavailable"
                       : matches.size() == 1 && complete && !gaps ? "exact"
                                                                  : "ambiguous";
  return {
      {"apiVersion", "graphlab.message-correlation/v1"},
      {"eventId", event["id"]},
      {"status", status},
      {"matches", matches},
      {"searchComplete", complete},
      {"packetsExamined", std::to_string(examined)},
      {"errors", errors},
      {"captureCoverage", run.value("captureCoverage", "unavailable")},
      {"scope", edge.empty() ? "retained run captures (union; occurrences remain distinct)" : edge},
      {"reason", matches.empty() ? "No verified match in the scanned retained scope; "
                                   "unsupported/encrypted/omitted traffic cannot be correlated"
                 : status == "exact"
                     ? "One exact wire-identifier occurrence in the scanned retained scope; not "
                       "delivery accounting"
                     : "Multiple occurrences or incomplete coverage; request/echo, duplicate "
                       "capture, retry and identifier reuse cannot be distinguished"},
      {"deliveryAccounting", "unavailable"}};
}
} // namespace graphlab::messages
