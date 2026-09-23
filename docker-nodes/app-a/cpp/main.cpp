#include <lab_support/lifecycle.hpp>
int main(int argc, char **argv) {
#ifdef GRAPHLAB_APPLICATION_TELEMETRY
  return lab_support::run_node(argc, argv, "app-a", true);
#else
  return lab_support::run_node(argc, argv, "app-a");
#endif
}
