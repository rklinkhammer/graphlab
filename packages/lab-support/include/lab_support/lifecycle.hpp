#pragma once
namespace lab_support {
inline constexpr unsigned node_gate_minor = 1;
// C++ startup gate and data-interface-only UDP echo/probe fixture protocol v1.
int run_node(int argc, char **argv, const char *application);
int run_node(int argc, char **argv, const char *application, bool application_telemetry);
int run_node(int argc, char **argv, const char *application, bool application_telemetry,
             bool edge_telemetry);
// Opt-in autonomous UDP sources; target is fixture configuration, never browser input.
int run_node(int argc, char **argv, const char *application, bool application_telemetry,
             bool edge_telemetry, const char *source_target);
} // namespace lab_support
