#pragma once
#include <graphlab/terminal.hpp>
namespace graphlab::qemu {
using runtime::Json;
Json qmp(const std::filesystem::path &, const std::string &command);
void preflight(const Json &, const Json &, const std::filesystem::path &);
Json prepare(const Json &, const Json &, const std::filesystem::path &);
Json command(const Json &, const Json &, const std::string &);
void remove(const Json &, const Json &, const std::filesystem::path &);
std::string tap_name(const Json &, const std::string &node, const std::string &port);
std::vector<std::string> arguments(const Json &config);
} // namespace graphlab::qemu
