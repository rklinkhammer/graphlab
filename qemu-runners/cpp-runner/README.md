# C++ QEMU container runner

Build `lab-qemu-runner` through the workspace C++23 CMake build. It shares argument construction with host QEMU and uses the common LabSupport contract/document library. It is deliberately separate from ordinary `docker-nodes/` workloads: it is a backend runner, with tun/KVM device access and an explicit shared-host-network boundary.

See [M7 backends](../../docs/m7-backends.md) for the immutable image build, workload-lock selector, supervision policy, test commands and rejected isolation mode. `build-image.sh` assembles an offline local ARM64 qualification image and records the exact native binaries/libraries. No Python runs in the image or its lab support.
