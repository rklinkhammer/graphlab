#include <fcntl.h>
#include <graphlab/terminal.hpp>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
namespace graphlab::terminal {
namespace {
void put(std::string &b, std::uint64_t n, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    b.push_back(char(n >> (8 * i)));
}
std::uint64_t get(const char *p, unsigned n) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i)
    v |= std::uint64_t(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}
void all(int fd, std::string_view b) {
  while (!b.empty()) {
    auto n = write(fd, b.data(), b.size());
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw runtime::Failure("recording_write_failed");
    b.remove_prefix(n);
  }
}
} // namespace
Json Lease::acquire(const std::string &owner, bool takeover, Clock::time_point now) {
  if (owner.empty() || owner.size() > 128)
    throw runtime::Failure("invalid_terminal_owner");
  if (now < until_ && !takeover)
    throw runtime::Failure("writer_busy", 409);
  owner_ = owner;
  token_ = console::random_hex(24);
  until_ = now + std::chrono::seconds(15);
  return {{"token", token_}, {"expiresInSeconds", 15}};
}
void Lease::require(const std::string &owner, const std::string &token,
                    Clock::time_point now) const {
  if (now >= until_ || owner != owner_ || token != token_)
    throw runtime::Failure("writer_lease_expired", 409);
}
void Lease::renew(const std::string &o, const std::string &t, Clock::time_point now) {
  require(o, t, now);
  until_ = now + std::chrono::seconds(15);
}
void Lease::revoke() {
  until_ = {};
  owner_.clear();
  token_.clear();
}
Recording::Recording(const std::filesystem::path &p, std::uint64_t budget)
    : path_(p), budget_(budget) {
  fd_ = open(p.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0)
    throw runtime::Failure("recording_exists_or_unavailable");
  try {
    all(fd_, std::string_view("GLTERM1\0", 8));
    size_ = 8;
    sync();
  } catch (...) {
    ::close(fd_);
    fd_ = -1;
    throw;
  }
}
Recording::~Recording() {
  if (fd_ >= 0)
    ::close(fd_);
}
void Recording::append(std::uint32_t type, std::string_view bytes) {
  if (type < 1 || type > 5 || bytes.size() > 65536 || fd_ < 0)
    throw runtime::Failure("invalid_record");
  if (size_ + 32 + bytes.size() > budget_)
    throw runtime::Failure("recording_quota_exhausted");
  std::string b;
  put(b, type, 4);
  put(b, bytes.size(), 4);
  put(b, sequence_, 8);
  put(b, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - anchor_).count(), 8);
  put(b, output_, 8);
  b += bytes;
  all(fd_, b);
  size_ += b.size();
  ++sequence_;
  if (type == 1)
    output_ += bytes.size();
}
void Recording::sync() {
  if (fd_ < 0 || fsync(fd_))
    throw runtime::Failure("recording_sync_failed");
}
void Recording::close() {
  if (fd_ < 0)
    return;
  sync();
  ::close(fd_);
  fd_ = -1;
  auto closed = path_;
  closed.replace_extension("glterm");
  if (link(path_.c_str(), closed.c_str()) || unlink(path_.c_str()))
    throw runtime::Failure("recording_publish_failed");
  int d = open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (d < 0)
    throw runtime::Failure("recording_directory");
  auto result = fsync(d);
  ::close(d);
  if (result)
    throw runtime::Failure("recording_directory_sync");
}
std::string encode(std::string_view b) {
  std::string s(4 * ((b.size() + 2) / 3) + 1, '\0');
  s.resize(EVP_EncodeBlock(reinterpret_cast<unsigned char *>(s.data()),
                           reinterpret_cast<const unsigned char *>(b.data()), b.size()));
  return s;
}
std::string decode(const std::string &s) {
  if (s.size() > 87384 || s.size() % 4 ||
      s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
          std::string::npos)
    throw runtime::Failure("invalid_terminal_bytes");
  std::string b(s.size() / 4 * 3, '\0');
  auto n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(b.data()),
                           reinterpret_cast<const unsigned char *>(s.data()), s.size());
  if (n < 0)
    throw runtime::Failure("invalid_terminal_bytes");
  if (!s.empty() && s.back() == '=')
    --n;
  if (s.size() > 1 && s[s.size() - 2] == '=')
    --n;
  if (n < 0)
    throw runtime::Failure("invalid_terminal_bytes");
  b.resize(n);
  return b;
}
Json replay(const std::filesystem::path &p, std::uint64_t start) {
  int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    throw runtime::Failure("invalid_recording");
  struct Guard {
    int fd;
    ~Guard() { ::close(fd); }
  } guard{fd};
  struct stat st{};
  if (fstat(fd, &st) || !S_ISREG(st.st_mode))
    throw runtime::Failure("invalid_recording");
  auto read_bytes = [&](char *b, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
      auto n = ::read(fd, b + total, size - total);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        throw runtime::Failure("recording_read_failed");
      if (!n)
        break;
      total += n;
    }
    return total;
  };
  char magic[8];
  auto count = read_bytes(magic, 8);
  if (count != 8 || std::string_view(magic, 8) != std::string_view("GLTERM1\0", 8))
    throw runtime::Failure("invalid_recording");
  Json records = Json::array();
  std::uint64_t expected = 0, next = start;
  std::size_t bytes = 0;
  bool partial = false;
  for (;;) {
    char h[32];
    count = read_bytes(h, 32);
    if (count == 0)
      break;
    if (count != 32) {
      partial = true;
      break;
    }
    auto type = get(h, 4), n = get(h + 4, 4), seq = get(h + 8, 8);
    if (n > 65536 || seq != expected++ || type < 1 || type > 5)
      throw runtime::Failure("invalid_recording");
    std::string b(n, '\0');
    count = read_bytes(b.data(), n);
    if (count != n) {
      partial = true;
      break;
    }
    if (seq < start)
      continue;
    if (bytes + n > 65536 && !records.empty())
      break;
    records.push_back({{"type", type},
                       {"sequence", std::to_string(seq)},
                       {"monotonicNs", std::to_string(get(h + 16, 8))},
                       {"offset", std::to_string(get(h + 24, 8))},
                       {"base64", encode(b)}});
    next = seq + 1;
    bytes += n;
    if (records.size() >= 128)
      break;
  }
  return {{"records", records}, {"next", std::to_string(next)}, {"partialTail", partial}};
}
} // namespace graphlab::terminal
