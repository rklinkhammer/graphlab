#include <graphlab/qemu.hpp>
namespace graphlab::qemu {
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
  if (c.contains("kernel") && c.contains("guestAddresses"))
    for (const auto &[port, address] : c["guestAddresses"].items())
      a.back() += " graphlab." + port + "=" + address.get<std::string>();
  int i = 0;
  for (const auto &nic : c["nics"]) {
    auto id = "net" + std::to_string(i++);
    a.insert(
        a.end(),
        {"-netdev",
         "tap,id=" + id + ",ifname=" + nic["tap"].get<std::string>() + ",script=no,downscript=no",
         "-device",
         "virtio-net-pci,netdev=" + id + ",mac=" + nic["mac"].get<std::string>() + ",romfile="});
  }
  return a;
}
} // namespace graphlab::qemu
