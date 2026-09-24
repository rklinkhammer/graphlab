#include <lab_support/lifecycle.hpp>
int main(int argc, char **argv) {
  return lab_support::run_node(argc, argv, "app-streams", true, true);
}
