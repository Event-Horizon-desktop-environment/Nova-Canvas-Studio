set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

default: build

# Develop / debug build into ./build (via build.sh)
build:
    ./build.sh

# Configure the dedicated release build dir (installs into /usr, not /usr/local)
configure-release:
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

# Compile the release build (no install)
build-release:
    cmake --build build-release -j$(nproc)

# Full release build + install into /usr. Always reconfigures, recompiles and
# installs the newest code. Run elevated: `sudo just install`.
#     sudo just install
install:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{ justfile_directory() }}"
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build-release -j"$(nproc)"
    exec cmake --install build-release --strip

# Build as your user, then install into /usr (linux only elevation for the
# final install step via sudo). Also safe when run as root.
install-release:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{ justfile_directory() }}"
    if [ "$(id -u)" -eq 0 ]; then
        test -f build-release/build.ninja || { echo >&2 "error: missing build-release/ — run: just build-release (as your user)"; exit 1; }
        exec cmake --install build-release --strip
    fi
    just build-release
    exec sudo cmake --install build-release --strip

# Remove the system-wide install from /usr. Also cleans up the stale
# event-horizon launcher/binary/icon from the pre-Nova-Canvas name.
uninstall:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{ justfile_directory() }}"
    exec sudo rm -f /usr/bin/canvas \
        /usr/share/applications/canvas.desktop \
        /usr/share/icons/hicolor/scalable/apps/canvas.svg \
        /usr/bin/event-horizon \
        /usr/share/applications/event-horizon.desktop \
        /usr/share/icons/hicolor/scalable/apps/event-horizon.svg

# Remove build directories
clean:
    rm -rf build build-release

# Run the test suite (roundtrip + export sweep)
test:
    ctest --test-dir build