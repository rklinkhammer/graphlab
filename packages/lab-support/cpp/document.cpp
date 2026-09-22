#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <lab_support/contracts.hpp>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/yaml.h>

namespace lab_support {
namespace {
constexpr std::size_t max_bytes = 1024 * 1024;
struct Frame {
  Json value;
  std::string key;
  bool needs_key = true;
};
class Decoder final : public YAML::EventHandler {
  std::vector<Frame> stack;
  std::size_t count = 0;
  void check(const std::string &tag, YAML::anchor_t anchor) {
    if (++count > 100000)
      throw std::runtime_error("document exceeds 100000 values");
    if (anchor || (tag != "?" && tag != "!" && !tag.empty()))
      throw std::runtime_error("anchors and explicit tags are not supported");
  }
  void put(Json value) {
    if (stack.empty()) {
      root = std::move(value);
      return;
    }
    auto &frame = stack.back();
    if (frame.value.is_array())
      frame.value.push_back(std::move(value));
    else if (frame.needs_key) {
      if (!value.is_string())
        throw std::runtime_error("mapping keys must be strings");
      frame.key = value.get<std::string>();
      if (frame.value.contains(frame.key))
        throw std::runtime_error("duplicate key: " + frame.key);
      if (frame.key == "<<")
        throw std::runtime_error("YAML merge keys are not supported");
      frame.needs_key = false;
    } else {
      frame.value[frame.key] = std::move(value);
      frame.needs_key = true;
    }
  }
  void begin(Json value, const std::string &tag, YAML::anchor_t anchor) {
    check(tag, anchor);
    if (stack.size() >= 64)
      throw std::runtime_error("document exceeds depth 64");
    if (!stack.empty() && stack.back().value.is_object() && stack.back().needs_key)
      throw std::runtime_error("complex mapping keys are not supported");
    stack.push_back({std::move(value), {}, true});
  }
  void end() {
    auto v = std::move(stack.back().value);
    stack.pop_back();
    put(std::move(v));
  }

public:
  Json root;
  int documents = 0;
  void OnDocumentStart(const YAML::Mark &) override {
    if (++documents != 1)
      throw std::runtime_error("exactly one document is required");
  }
  void OnDocumentEnd() override {}
  void OnNull(const YAML::Mark &, YAML::anchor_t a) override {
    check({}, a);
    put(nullptr);
  }
  void OnAlias(const YAML::Mark &, YAML::anchor_t) override {
    throw std::runtime_error("aliases are not supported");
  }
  void OnAnchor(const YAML::Mark &, const std::string &) override {
    throw std::runtime_error("anchors are not supported");
  }
  void OnScalar(const YAML::Mark &, const std::string &tag, YAML::anchor_t a,
                const std::string &v) override {
    check(tag, a);
    if ((!stack.empty() && stack.back().value.is_object() && stack.back().needs_key) ||
        tag == "!") {
      put(v);
      return;
    }
    if (v == "true" || v == "false") {
      put(v == "true");
      return;
    }
    std::int64_t n = 0;
    auto [ptr, error] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (!v.empty() && error == std::errc{} && ptr == v.data() + v.size()) {
      put(n);
      return;
    }
    put(v);
  }
  void OnSequenceStart(const YAML::Mark &, const std::string &t, YAML::anchor_t a,
                       YAML::EmitterStyle::value) override {
    begin(Json::array(), t, a);
  }
  void OnSequenceEnd() override { end(); }
  void OnMapStart(const YAML::Mark &, const std::string &t, YAML::anchor_t a,
                  YAML::EmitterStyle::value) override {
    begin(Json::object(), t, a);
  }
  void OnMapEnd() override { end(); }
};
} // namespace
Result<Json> parse_document(std::string_view text) {
  if (text.size() > max_bytes)
    return std::unexpected(Error{"document_limit", "$", "document exceeds 1 MiB"});
  try {
    std::istringstream in{std::string(text)};
    YAML::Parser parser(in);
    Decoder decoder;
    while (parser.HandleNextDocument(decoder)) {
    }
    if (decoder.documents != 1 || !decoder.root.is_object())
      throw std::runtime_error("root must be a mapping");
    return decoder.root;
  } catch (const std::exception &e) {
    return std::unexpected(Error{"parse_error", "$", e.what()});
  }
}
Result<std::string> read_document(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::unexpected(Error{"io_error", path, "cannot open input"});
  std::string text(max_bytes + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(input.gcount()));
  if (input.bad())
    return std::unexpected(Error{"io_error", path, "input read failed"});
  if (text.size() > max_bytes)
    return std::unexpected(Error{"document_limit", path, "document exceeds 1 MiB"});
  return text;
}
std::string digest(const Json &value) {
  const auto text = value.dump(); // std::map object keys, no insignificant whitespace.
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned int length = 0;
  if (EVP_Digest(text.data(), text.size(), bytes.data(), &length, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 unavailable");
  std::ostringstream out;
  out << "sha256:" << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < length; ++i)
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  return out.str();
}
std::string crypto_version() { return OpenSSL_version(OPENSSL_VERSION); }
} // namespace lab_support
