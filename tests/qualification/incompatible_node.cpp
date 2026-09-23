// Negative wire fixture: the image label claims v1 but the actual peer says v2.
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc >= 3 && std::string(argv[1]) == "control") {
    std::string op = argv[2];
    if (op.starts_with("release"))
      std::ofstream("/run/release-attempted") << op;
    std::cout << "{\"apiVersion\":\"graphlab.gate/v2\",\"state\":\"held\",\"releaseAttempted\":"
              << (std::ifstream("/run/release-attempted").good() ? "true" : "false") << "}\n";
    return 0;
  }
  for (;;)
    pause();
}
