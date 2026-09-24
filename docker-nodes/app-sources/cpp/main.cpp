#include <lab_support/lifecycle.hpp>
int main(int argc, char **argv) {
  return lab_support::run_node(argc, argv, "app-sources", false, false, "10.231.17.1");
}
