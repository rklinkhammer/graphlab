#include <fcntl.h>
#include <fstream>
#include <graphlab/capture.hpp>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
namespace graphlab::capture {
namespace {
using Bytes = std::vector<unsigned char>;
void u16(Bytes &b, std::uint16_t v) {
  b.push_back(v);
  b.push_back(v >> 8);
}
void u32(Bytes &b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b.push_back(v >> (8 * i));
}
void u64(Bytes &b, std::uint64_t v) {
  u32(b, v);
  u32(b, v >> 32);
}
void pad(Bytes &b) {
  while (b.size() % 4)
    b.push_back(0);
}
void option(Bytes &b, std::uint16_t key, const std::string &value) {
  if (value.size() > 65535)
    throw runtime::Failure("capture_metadata_too_long");
  u16(b, key);
  u16(b, value.size());
  b.insert(b.end(), value.begin(), value.end());
  pad(b);
}
void write_all(int fd, std::span<const unsigned char> bytes) {
  while (!bytes.empty()) {
    auto n = ::write(fd, bytes.data(), bytes.size());
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw runtime::Failure("capture_write_failed", 503);
    bytes = bytes.subspan(n);
  }
}
void sync_directory(const std::filesystem::path &p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    throw runtime::Failure("capture_directory_unavailable");
  auto result = fsync(fd);
  ::close(fd);
  if (result)
    throw runtime::Failure("capture_directory_sync");
}
} // namespace
void atomic_json(const std::filesystem::path &p, const Json &j) {
  auto temporary = p.string() + ".tmp";
  int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    throw runtime::Failure("manifest_open");
  try {
    auto s = j.dump(2) + "\n";
    write_all(fd, {reinterpret_cast<const unsigned char *>(s.data()), s.size()});
    if (fsync(fd))
      throw runtime::Failure("manifest_sync");
    ::close(fd);
    fd = -1;
    if (rename(temporary.c_str(), p.c_str()))
      throw runtime::Failure("manifest_rename");
    sync_directory(p.parent_path());
  } catch (...) {
    if (fd >= 0)
      ::close(fd);
    throw;
  }
}
std::string file_hash(const std::filesystem::path &p) {
  std::ifstream input(p, std::ios::binary);
  if (!input)
    throw runtime::Failure("artifact_unavailable");
  auto ctx = EVP_MD_CTX_new();
  if (!ctx)
    throw runtime::Failure("hash_allocation");
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  char buffer[65536];
  while (input.read(buffer, sizeof(buffer)) || input.gcount())
    EVP_DigestUpdate(ctx, buffer, input.gcount());
  if (!input.eof()) {
    EVP_MD_CTX_free(ctx);
    throw runtime::Failure("artifact_read");
  }
  unsigned char digest[32];
  unsigned length;
  EVP_DigestFinal_ex(ctx, digest, &length);
  EVP_MD_CTX_free(ctx);
  std::string result = "sha256:";
  constexpr char hex[] = "0123456789abcdef";
  for (auto c : digest) {
    result += hex[c >> 4];
    result += hex[c & 15];
  }
  return result;
}
Writer::Writer(std::filesystem::path directory, Json config)
    : directory_(std::move(directory)), config_(std::move(config)) {}
Json inspect_partial(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw runtime::Failure("partial_unavailable");
  std::uint64_t valid = 0;
  bool section = false, interface = false;
  auto read32 = [](const unsigned char *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
  };
  for (;;) {
    unsigned char header[8];
    input.read(reinterpret_cast<char *>(header), 8);
    if (input.gcount() != 8)
      break;
    auto type = read32(header), length = read32(header + 4);
    if (length < 12 || length % 4 || length > 1024 * 1024)
      break;
    std::vector<unsigned char> body(length - 8);
    input.read(reinterpret_cast<char *>(body.data()), body.size());
    if (input.gcount() != static_cast<std::streamsize>(body.size()) ||
        read32(body.data() + body.size() - 4) != length)
      break;
    if (!section) {
      if (type != 0x0a0d0d0a || length < 28 || read32(body.data()) != 0x1a2b3c4d)
        break;
      section = true;
    } else if (type == 1) {
      if (length < 20)
        break;
      interface = true;
    } else if (type == 6) {
      if (!interface || length < 32 || read32(body.data()) != 0 ||
          read32(body.data() + 12) > read32(body.data() + 16) ||
          read32(body.data() + 12) > length - 32)
        break;
    }
    valid += length;
  }
  return {
      {"state", "partial"},
      {"size", std::to_string(std::filesystem::file_size(path))},
      {"validPrefixBytes", std::to_string(valid)},
      {"formatScope", "Graphlab little-endian PCAPNG structural prefix; original bytes preserved"}};
}
Writer::~Writer() {
  if (fd_ >= 0)
    ::close(fd_);
}
void Writer::block(std::uint32_t type, std::span<const unsigned char> body) {
  Bytes b;
  u32(b, type);
  u32(b, body.size() + 12);
  b.insert(b.end(), body.begin(), body.end());
  u32(b, body.size() + 12);
  if (total_ + bytes_ + b.size() > config_.at("byteBudget").get<std::uint64_t>())
    throw runtime::Failure("capture_quota_exhausted");
  write_all(fd_, b);
  bytes_ += b.size();
}
void Writer::open() {
  if (fd_ >= 0 || sequence_ >= 1024)
    throw runtime::Failure("capture_segment_limit");
  partial_ = directory_ / (std::to_string(sequence_) + ".pcapng.partial");
  fd_ = ::open(partial_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd_ < 0)
    throw runtime::Failure("capture_open");
  bytes_ = 0;
  packets_ = 0;
  Bytes b;
  u32(b, 0x1a2b3c4d);
  u16(b, 1);
  u16(b, 0);
  u64(b, UINT64_MAX);
  block(0x0a0d0d0a, b);
  b.clear();
  u16(b, 1);
  u16(b, 0);
  u32(b, config_.at("snaplen"));
  option(b, 2, config_.at("interface"));
  option(b, 3,
         config_.at("edge").get<std::string>() + "; direction flags unavailable; offloads unknown");
  u16(b, 9);
  u16(b, 1);
  b.push_back(6);
  pad(b);
  u32(b, 0);
  block(1, b);
  sync();
}
void Writer::packet(std::uint64_t micros, std::span<const unsigned char> bytes,
                    std::uint32_t original) {
  if (bytes.size() > config_.at("snaplen").get<std::uint32_t>() || bytes.size() > original)
    throw runtime::Failure("invalid_packet_length");
  Bytes b;
  u32(b, 0);
  u32(b, micros >> 32);
  u32(b, micros);
  u32(b, bytes.size());
  u32(b, original);
  b.insert(b.end(), bytes.begin(), bytes.end());
  pad(b);
  block(6, b);
  ++packets_;
}
void Writer::sync() {
  if (fd_ < 0 || fsync(fd_))
    throw runtime::Failure("capture_sync");
}
void Writer::close(Json stats) {
  if (fd_ < 0)
    return;
  Bytes b;
  u32(b, 0);
  auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
  u32(b, std::uint64_t(now) >> 32);
  u32(b, now);
  // Values are cumulative libpcap counters, not inferred zero when unavailable.
  // Linux libpcap socket statistics: filter-accepted and OS-buffer drops.
  // Physical-interface receive/drop counters are not qualified here.
  for (auto [key, code] : {std::pair{"received", 6}, std::pair{"dropped", 7}})
    if (stats.contains(key) && !stats[key].is_null()) {
      u16(b, code);
      u16(b, 8);
      u64(b, stats[key].get<std::uint64_t>());
    }
  u32(b, 0);
  block(5, b);
  sync();
  ::close(fd_);
  fd_ = -1;
  auto closed = directory_ / (std::to_string(sequence_) + ".pcapng");
  // Atomically publish without replacement, even after a crashed worker or
  // accidental reuse of an output directory. Never overwrite an old segment.
  if (link(partial_.c_str(), closed.c_str()) || unlink(partial_.c_str()))
    throw runtime::Failure("capture_finalize");
  sync_directory(directory_);
  segments_.push_back({{"sequence", std::to_string(sequence_++)},
                       {"file", closed.filename().string()},
                       {"size", std::to_string(bytes_)},
                       {"packets", std::to_string(packets_)},
                       {"sha256", file_hash(closed)},
                       {"state", "closed"},
                       {"statistics", stats},
                       {"closedAt", console::timestamp()}});
  total_ += bytes_;
  bytes_ = 0;
}
} // namespace graphlab::capture
