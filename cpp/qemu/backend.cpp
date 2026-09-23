#include <fstream>
#include <graphlab/qemu.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#endif
namespace graphlab::qemu {
namespace {
std::string cmd(std::vector<std::string> a) {
  auto r = runtime::process(a, 30);
  if (r.code)
    throw runtime::Failure("qemu_backend_command_failed");
  while (!r.output.empty() && r.output.back() == '\n')
    r.output.pop_back();
  return r.output;
}
std::filesystem::path artifact(const std::filesystem::path &root, const std::string &hash) {
  if (hash.size() != 71 || !hash.starts_with("sha256:") ||
      hash.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos)
    throw runtime::Failure("invalid_vm_digest");
  auto p = root / "artifacts" / hash.substr(7);
  struct stat s{};
  if (lstat(p.c_str(), &s) || !S_ISREG(s.st_mode) || (s.st_mode & 0022) || s.st_uid != 0 ||
      capture::file_hash(p) != hash)
    throw runtime::Failure("vm_artifact_integrity");
  return p;
}
Json config(const Json &r, const Json &n, const std::filesystem::path &root) {
  auto p = root / "vms" / runtime::resource_name(r, n["key"]) / "config.json";
  auto c = console::load(p);
  if (c["runId"] != r["id"] || c["node"] != n["logical"])
    throw runtime::Failure("vm_ownership_conflict");
  return c;
}
} // namespace
Json qmp(const std::filesystem::path &path, const std::string &op) {
  int fd = terminal::connect_unix(path);
  struct Guard {
    int fd;
    ~Guard() { close(fd); }
  } guard{fd};
  auto read = [&]() {
    std::string s;
    char c;
    while (s.size() < 65536) {
      auto n = recv(fd, &c, 1, 0);
      if (n != 1)
        throw runtime::Failure("qmp_read_failed");
      s += c;
      if (c == '\n')
        return Json::parse(s);
    }
    throw runtime::Failure("qmp_frame_limit");
  };
  if (!read().contains("QMP"))
    throw runtime::Failure("qmp_greeting");
  auto call = [&](const std::string &command, int id) {
    auto b = Json{{"execute", command}, {"id", id}}.dump() + "\r\n";
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    if (send(fd, b.data(), b.size(), flags) != static_cast<ssize_t>(b.size()))
      throw runtime::Failure("qmp_send_failed");
    for (int i = 0; i < 64; ++i) {
      auto response = read();
      if (response.contains("event"))
        continue;
      if (response.value("id", -1) != id)
        throw runtime::Failure("qmp_response_identity");
      if (response.contains("error"))
        throw runtime::Failure("qmp_command_failed");
      return response.at("return");
    }
    throw runtime::Failure("qmp_event_limit");
  };
  call("qmp_capabilities", 0);
  return call(op, 1);
}
std::string tap_name(const Json &r, const std::string &node, const std::string &port) {
  return runtime::resource_name(r, "tap/" + node + "/" + port);
}
std::vector<std::string> arguments(const Json &c) {
  auto dir = c["directory"].get<std::string>(), arch = c["platform"].get<std::string>();
  auto binary = arch == "linux/ppc64le" ? "/usr/bin/qemu-system-ppc64"
                : arch == "linux/arm64" ? "/usr/bin/qemu-system-aarch64"
                                        : "/usr/bin/qemu-system-x86_64";
  std::vector<std::string> a = {
      binary,        "-S",
      "-nodefaults", "-no-reboot",
      "-display",    "none",
      "-monitor",    "none",
      "-machine",    c["machine"],
      "-accel",      c["accelerator"],
      "-m",          std::to_string(c["guestMemoryMiB"].get<int>()),
      "-smp",        std::to_string(c["cpus"].get<int>()),
      "-qmp",        "unix:" + dir + "/qmp.sock,server=on,wait=off",
      "-chardev",    "socket,id=serial,path=" + dir + "/serial.sock,server=on,wait=off",
      "-serial",     "chardev:serial",
      "-bios",       c["firmware"],
      "-drive",      "file=" + dir + "/overlay.qcow2,if=none,id=disk,format=qcow2",
      "-device",     "virtio-blk-pci,drive=disk"};
  a.insert(a.end(), {"-cpu", arch == "linux/ppc64le"     ? "power9"
                             : c["accelerator"] == "kvm" ? "host"
                                                         : "max"});
  if (c.contains("kernel")) {
    a.insert(a.end(), {"-kernel", c["kernel"], "-initrd", c["initrd"], "-append",
                       arch == "linux/ppc64le" ? "console=hvc0 rdinit=/init panic=-1"
                       : arch == "linux/arm64" ? "console=ttyAMA0 rdinit=/init panic=-1"
                                               : "console=ttyS0 rdinit=/init panic=-1"});
  }
  int i = 0;
  for (const auto &nic : c["nics"]) {
    auto id = "net" + std::to_string(i++);
    a.insert(
        a.end(),
        {"-netdev",
         "tap,id=" + id + ",ifname=" + nic["tap"].get<std::string>() + ",script=no,downscript=no",
         "-device", "virtio-net-pci,netdev=" + id + ",mac=" + nic["mac"].get<std::string>()});
  }
  return a;
}
void preflight(const Json &t, const Json &lock, const std::filesystem::path &root) {
  for (const auto &n : t["nodes"]) {
    if (n["kind"] != "qemu")
      continue;
    auto w = lock["workloads"][n["workload"].get<std::string>()], v = w["vm"];
    auto arch = w["platform"].get<std::string>();
    if (arch != "linux/arm64" && arch != "linux/ppc64le" && arch != "linux/amd64")
      throw runtime::Failure("unsupported_guest_architecture");
    auto binary = arch == "linux/ppc64le" ? "/usr/bin/qemu-system-ppc64"
                  : arch == "linux/arm64" ? "/usr/bin/qemu-system-aarch64"
                                          : "/usr/bin/qemu-system-x86_64";
    if (access(binary, X_OK) || access("/usr/bin/qemu-img", X_OK))
      throw runtime::Failure("qemu_unavailable");
    if (v["accelerator"] == "kvm" && access("/dev/kvm", R_OK | W_OK))
      throw runtime::Failure("kvm_unavailable");
    utsname host{};
    if (uname(&host))
      throw runtime::Failure("host_architecture_unavailable");
    if (v["accelerator"] == "kvm" &&
        arch != (std::string(host.machine) == "aarch64"  ? "linux/arm64"
                 : std::string(host.machine) == "x86_64" ? "linux/amd64"
                                                         : "linux/ppc64le"))
      throw runtime::Failure("kvm_guest_host_architecture_mismatch");
    if (cmd({binary, "-machine", "help"}).find(v["machine"].get<std::string>()) ==
        std::string::npos)
      throw runtime::Failure("pinned_machine_unavailable");
    auto disk = artifact(root, w["diskSha256"]);
    auto info = Json::parse(cmd({"/usr/bin/qemu-img", "info", "--output=json", disk.string()}));
    if (info["format"] != "raw")
      throw runtime::Failure("vm_base_must_be_raw");
    artifact(root, v["firmwareSha256"]);
    if (v.contains("kernelSha256")) {
      artifact(root, v["kernelSha256"]);
      artifact(root, v["initrdSha256"]);
    }
  }
}
Json prepare(const Json &r, const Json &n, const std::filesystem::path &root) {
  auto w = r["artifacts"]["workloads"][n["configuration"]["workload"].get<std::string>()],
       v = w["vm"];
  auto c = terminal::descriptor(r, n["logical"], root / "vms", "qemu");
  c["directory"] = (root / "vms" / runtime::resource_name(r, n["key"])).string();
  c["unit"] = "graphlab-vm-" + runtime::resource_name(r, n["key"]) + ".service";
  c["platform"] = w["platform"];
  c["machine"] = v["machine"];
  c["accelerator"] = v["accelerator"];
  c["firmware"] = artifact(root, v["firmwareSha256"]).string();
  c["base"] = artifact(root, w["diskSha256"]).string();
  c["guestMemoryMiB"] = w["contract"]["resources"]["memoryMiB"];
  c["memoryMiB"] = c["guestMemoryMiB"].get<int>() + 512;
  c["cpus"] = w["contract"]["resources"]["cpus"];
  if (v.contains("kernelSha256")) {
    c["kernel"] = artifact(root, v["kernelSha256"]).string();
    c["initrd"] = artifact(root, v["initrdSha256"]).string();
  }
  c["nics"] = Json::array();
  for (const auto &[port, p] : n["configuration"]["ports"].items()) {
    auto tap = tap_name(r, n["logical"], port);
    auto hash = lab_support::digest(Json::array({r["id"], n["logical"], port}));
    std::string mac = "02";
    for (int i = 0; i < 5; ++i)
      mac += ":" + hash.substr(7 + 2 * i, 2);
    std::string owner = r["id"].get<std::string>() + ":node/" + n["logical"].get<std::string>();
    for (const auto &e : r["topology"]["edges"])
      for (const auto &ep : e["endpoints"])
        if (ep == n["logical"].get<std::string>() + ":" + port)
          owner = r["id"].get<std::string>() + ":edge/" + e["id"].get<std::string>();
    c["nics"].push_back({{"port", port}, {"tap", tap}, {"mac", mac}, {"owner", owner}});
  }
  // Persist node intent before any TAP exists. The agent's node resource intent is already durable.
  auto dir = std::filesystem::path(c["directory"].get<std::string>());
  std::filesystem::create_directories(dir.parent_path());
  chmod(dir.parent_path().c_str(), 0700);
  if (!std::filesystem::create_directory(dir))
    throw runtime::Failure("vm_directory_conflict");
  chmod(dir.c_str(), 0700);
  capture::atomic_json(dir / "config.json", c);
  runtime::detail::checkpoint("qemu.config");
  for (const auto &nic : c["nics"]) {
    auto tap = nic["tap"].get<std::string>();
#ifdef __linux__
    // A nonpersistent TAP disappears if the executor dies before ownership is
    // attached. Only make it persistent after its alias is visible to recovery.
    int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0)
      throw runtime::Failure("tap_open_failed");
    struct Guard {
      int fd;
      ~Guard() { close(fd); }
    } guard{fd};
    ifreq request{};
    if (tap.size() >= IFNAMSIZ)
      throw runtime::Failure("tap_name_limit");
    std::copy(tap.begin(), tap.end(), request.ifr_name);
    request.ifr_flags = static_cast<short>(IFF_TAP | IFF_NO_PI | IFF_TUN_EXCL);
    if (ioctl(fd, TUNSETIFF, &request))
      throw runtime::Failure("tap_create_failed");
    runtime::detail::checkpoint("qemu.tap-created");
    runtime::detail::checkpoint("qemu.tap-created." + nic["port"].get<std::string>());
    cmd({"/usr/sbin/ip", "link", "set", "dev", tap, "alias", nic["owner"]});
    if (ioctl(fd, TUNSETPERSIST, 1))
      throw runtime::Failure("tap_persist_failed");
    runtime::detail::checkpoint("qemu.tap-owned");
    runtime::detail::checkpoint("qemu.tap-owned." + nic["port"].get<std::string>());
#else
    throw runtime::Failure("qemu_requires_linux");
#endif
  }
  for (const auto &a : r["topology"]["managementAttachments"]) {
    auto ep = a["endpoint"].get<std::string>();
    if (!ep.starts_with(n["logical"].get<std::string>() + ":"))
      continue;
    auto net = runtime::docker_json(
        "GET", "/v1.52/networks/" +
                   runtime::resource_name(r, "management/" + a["network"].get<std::string>()));
    auto bridge = "br-" + net["Id"].get<std::string>().substr(0, 12);
    auto tap = tap_name(r, n["logical"], ep.substr(ep.find(':') + 1));
    cmd({"/usr/sbin/ip", "link", "set", tap, "master", bridge});
    cmd({"/usr/sbin/ip", "link", "set", tap, "up"});
    runtime::detail::checkpoint("qemu.management");
  }
  cmd({"/usr/bin/qemu-img", "create", "-f", "qcow2", "-F", "raw", "-b", c["base"],
       (dir / "overlay.qcow2").string()});
  runtime::detail::checkpoint("qemu.overlay");
  // launch() normally creates a fresh directory. This VM config was durably created before TAP
  // effects.
  c["preparedDirectory"] = true;
  terminal::launch(c);
  runtime::detail::checkpoint("qemu.created");
  for (int i = 0; i < 100; ++i) {
    try {
      auto s = terminal::request(c, "status", r["controllerGeneration"]);
      if (s.value("serialReady", false) && s.value("vmState", "") == "paused")
        return c;
    } catch (...) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw runtime::Failure("paused_vm_not_ready");
}
Json command(const Json &r, const Json &n, const std::string &op) {
  return terminal::request(n["identity"], op, r["controllerGeneration"]);
}
void remove(const Json &r, const Json &n, const std::filesystem::path &root) {
  auto dir = root / "vms" / runtime::resource_name(r, n["key"]);
  if (!std::filesystem::exists(dir))
    return;
  auto c = config(r, n, root);
  terminal::stop(c, r["controllerGeneration"]);
  for (const auto &nic : c["nics"]) {
    auto name = nic["tap"].get<std::string>();
    auto found = runtime::detail::lookup_link(
        runtime::process({"/usr/sbin/ip", "-j", "link", "show"}), name);
    if (found.is_null())
      continue;
    if (found.value("ifalias", "") != nic["owner"].get<std::string>())
      throw runtime::Failure("tap_ownership_conflict");
    cmd({"/usr/sbin/ip", "link", "delete", name});
  }
}
} // namespace graphlab::qemu
