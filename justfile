set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

default: build

build:
    ./build.sh

configure-release:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{ justfile_directory() }}"
    extra=()
    if [ -d build/_deps/whispercpp-src ]; then
        extra+=( -DFETCHCONTENT_SOURCE_DIR_WHISPERCPP="$PWD/build/_deps/whispercpp-src" )
    fi
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr "${extra[@]}"

build-release:
    cmake --build build-release -j$(nproc)

install:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{ justfile_directory() }}"
    extra=()
    if [ -d build/_deps/whispercpp-src ]; then
        extra+=( -DFETCHCONTENT_SOURCE_DIR_WHISPERCPP="$PWD/build/_deps/whispercpp-src" )
    fi
    cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr "${extra[@]}"
    cmake --build build-release -j"$(nproc)"
    exec cmake --install build-release --strip

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

clean:
    rm -rf build build-release

test:
    ctest --test-dir build

test-core:
    #!/usr/bin/env bash
    set -euo pipefail
    for bin in "{{ justfile_directory() }}"/build/core/*test; do
        [ -x "$bin" ] || continue
        echo "== $bin"
        "$bin"
    done

test-gui:
    #!/usr/bin/env bash
    set -euo pipefail
    for bin in "{{ justfile_directory() }}"/build/gui/tests/*test; do
        [ -x "$bin" ] || continue
        echo "== $bin"
        "$bin"
    done