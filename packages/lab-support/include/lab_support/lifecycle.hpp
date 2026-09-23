#pragma once
namespace lab_support {
inline constexpr unsigned node_gate_minor = 1;
// C++ startup gate and data-interface-only UDP echo/probe fixture protocol v1.
int run_node(int argc, char **argv, const char *application);
} // namespace lab_support
