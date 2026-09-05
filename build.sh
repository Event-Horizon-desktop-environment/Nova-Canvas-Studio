#!/usr/bin/env bash
# ============================================================================
#  Nova Canvas Studio — Build Script
#  Builds the project from source and optionally installs dependencies.
#  Supports: Fedora, Arch Linux, Debian/Ubuntu, PikaOS, and derivatives.
# ============================================================================

set -euo pipefail

# ── Configuration ───────────────────────────────────────────────────────────
BUILD_DIR="build"
BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || echo 4)"
INSTALL_DEPS=0
CLEAN_BUILD=0
RUN_BUILD=1

# ── Colors & Formatting ─────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    BOLD="\033[1m"
    DIM="\033[2m"
    RESET="\033[0m"
    RED="\033[1;31m"
    GREEN="\033[1;32m"
    YELLOW="\033[1;33m"
    BLUE="\033[1;34m"
    MAGENTA="\033[1;35m"
    CYAN="\033[1;36m"
    GRAY="\033[90m"
else
    BOLD="" DIM="" RESET="" RED="" GREEN="" YELLOW="" BLUE=""
    MAGENTA="" CYAN="" GRAY=""
fi

# ── Helpers ──────────────────────────────────────────────────────────────────
info()    { printf "${BLUE}  ▸${RESET} %s\n" "$*"; }
ok()      { printf "${GREEN}  ✓${RESET} %s\n" "$*"; }
warn()    { printf "${YELLOW}  ⚠${RESET} %s\n" "$*"; }
fail()    { printf "${RED}  ✗${RESET} %s\n" "$*"; exit 1; }
step()    { printf "\n${CYAN}${BOLD}  ━━ %s ──${RESET}\n" "$*"; }

hr() {
    local w
    w=$(($(tput cols 2>/dev/null || echo 72) - 4))
    printf "${GRAY}"
    printf '  %.0s─' $(seq 1 "$w")
    printf "${RESET}\n"
}

spinner() {
    local pid=$1 msg=${2:-}
    local frames=('⠋' '⠙' '⠹' '⠸' '⠼' '⠴' '⠦' '⠧' '⠇' '⠏')
    local i=0
    while kill -0 "$pid" 2>/dev/null; do
        printf "\r  ${MAGENTA}%s${RESET} %s" "${frames[i]}" "$msg"
        i=$(( (i + 1) % ${#frames[@]} ))
        sleep 0.1
    done
    wait "$pid" 2>/dev/null
    local rc=$?
    if [[ $rc -eq 0 ]]; then
        printf "\r  ${GREEN}✓${RESET} %s\n" "$msg"
    else
        printf "\r  ${RED}✗${RESET} %s (exit %d)\n" "$msg" "$rc"
        return 1
    fi
}

# ── Banner ──────────────────────────────────────────────────────────────────
banner() {
    printf "\n"
    printf "    ${BOLD}${CYAN}Nova Canvas Studio${RESET} ${DIM}— Nonlinear Video Editor${RESET}\n"
    hr
}

# ── Distro Detection ────────────────────────────────────────────────────────
detect_distro() {
    step "Detecting Distribution"

    if [[ -f /etc/os-release ]]; then
        # shellcheck source=/dev/null
        . /etc/os-release
        DISTRO_ID="${ID:-unknown}"
        DISTRO_LIKE="${ID_LIKE:-$ID}"
        DISTRO_NAME="${PRETTY_NAME:-$ID}"
    else
        fail "Cannot detect distribution: /etc/os-release not found."
    fi

    case "$DISTRO_ID" in
        fedora)             PKG_MGR="dnf"   ; DISTRO_FAMILY="fedora"  ;;
        arch|manjaro|endeavouros|garuda) PKG_MGR="pacman"; DISTRO_FAMILY="arch"    ;;
        debian|ubuntu|linuxmint|pop|pika*)
            # PikaOS detection — check both ID and ID_LIKE
            if [[ "$DISTRO_ID" == pika* ]] || [[ "$DISTRO_LIKE" == *pika* ]]; then
                PKG_MGR="apt"
                DISTRO_FAMILY="pikaos"
            elif [[ "$DISTRO_LIKE" == *debian* ]] || [[ "$DISTRO_LIKE" == *ubuntu* ]]; then
                PKG_MGR="apt"
                DISTRO_FAMILY="debian"
            else
                PKG_MGR="apt"
                DISTRO_FAMILY="debian"
            fi
            ;;
        *)
            if [[ "$DISTRO_LIKE" == *fedora* ]]; then
                PKG_MGR="dnf"; DISTRO_FAMILY="fedora"
            elif [[ "$DISTRO_LIKE" == *arch* ]]; then
                PKG_MGR="pacman"; DISTRO_FAMILY="arch"
            elif [[ "$DISTRO_LIKE" == *debian* ]] || [[ "$DISTRO_LIKE" == *ubuntu* ]]; then
                PKG_MGR="apt"; DISTRO_FAMILY="debian"
            else
                fail "Unsupported distribution: $DISTRO_ID ($DISTRO_LIKE)."
            fi
            ;;
    esac

    ok "Detected: ${BOLD}$DISTRO_NAME${RESET} (family: $DISTRO_FAMILY, pkg: $PKG_MGR)"
}

# ── Package Maps ────────────────────────────────────────────────────────────
#
#  Package availability checked against:
#    Fedora  : https://packages.fedoraproject.org/
#    Arch    : https://archlinux.org/packages/
#    Debian  : https://www.debian.org/distrib/packages
#    PikaOS  : https://packages.pika-os.com/
#

# -- Fedora packages (dnf) --
# qt6-qtbase-devel, qt6-qtsvg-devel        — Qt6 Widgets + Svg
# ffmpeg-devel                               — FFmpeg (libavformat, libavcodec, etc.)
# pipewire-devel                             — PipeWire
# alsa-lib-devel                             — ALSA
# nlohmann-json-devel                        — JSON
# cmake, ninja-build, gcc-c++, pkgconf       — Build toolchain
FEDORA_DEPS=(
    cmake
    ninja-build
    gcc-c++
    pkgconf
    qt6-qtbase-devel
    qt6-qtsvg-devel
    ffmpeg-devel
    pipewire-devel
    alsa-lib-devel
    nlohmann-json-devel
)

# -- Arch packages (pacman) --
# qt6-base, qt6-svg                          — Qt6 Widgets + Svg
# ffmpeg                                      — FFmpeg
# pipewire                                    — PipeWire
# alsa-lib                                    — ALSA
# nlohmann-json                               — JSON
# cmake, ninja, gcc, pkgconf                  — Build toolchain
ARCH_DEPS=(
    cmake
    ninja
    gcc
    pkgconf
    qt6-base
    qt6-svg
    ffmpeg
    pipewire
    alsa-lib
    nlohmann-json
)

# -- Debian / Ubuntu packages (apt) --
# qt6-base-dev, libqt6svg6-dev               — Qt6 Widgets + Svg
# libavformat-dev, libavcodec-dev,            — FFmpeg dev headers
#   libavutil-dev, libswscale-dev
# libpipewire-0.3-dev                         — PipeWire
# libasound2-dev                              — ALSA
# nlohmann-json3-dev                          — JSON
# build-essential, cmake, ninja-build,        — Build toolchain
#   pkg-config
DEBIAN_DEPS=(
    build-essential
    cmake
    ninja-build
    pkg-config
    qt6-base-dev
    libqt6svg6-dev
    libavformat-dev
    libavcodec-dev
    libavutil-dev
    libswscale-dev
    libswresample-dev
    libpipewire-0.3-dev
    libasound2-dev
    nlohmann-json3-dev
)

# -- PikaOS packages (apt, Debian-based) --
# Same as Debian — PikaOS ships standard Debian/Ubuntu packages.
PIKAOS_DEPS=("${DEBIAN_DEPS[@]}")

# ── Package Installation ────────────────────────────────────────────────────
# Pick the privilege-elevation tool: prefer pkexec (GUI auth prompt),
# fall back to sudo if pkexec isn't available.
pick_elevator() {
    if command -v pkexec &>/dev/null; then
        ELEV="pkexec"
    elif command -v sudo &>/dev/null; then
        ELEV="sudo"
    else
        fail "Neither pkexec nor sudo found — cannot elevate to install packages."
    fi
}

run_elevated() {
    info "Running: $ELEV $*"
    "$ELEV" "$@" 2>&1 | while IFS= read -r line; do
        printf "    ${DIM}%s${RESET}\n" "$line"
    done
}

install_deps() {
    step "Installing Dependencies"

    local -n deps=DISTRO_DEPS

    case "$DISTRO_FAMILY" in
        fedora)  local -n deps=FEDORA_DEPS  ;;
        arch)    local -n deps=ARCH_DEPS    ;;
        debian)  local -n deps=DEBIAN_DEPS  ;;
        pikaos)  local -n deps=PIKAOS_DEPS  ;;
    esac

    info "Packages to install:"
    for pkg in "${deps[@]}"; do
        printf "    ${DIM}•${RESET} %s\n" "$pkg"
    done
    echo

    # Ask for confirmation before touching the system.
    read -r -p "  ${YELLOW}?${RESET} Install these dependencies now? [y/N] " answer
    case "${answer,,}" in
        y|yes)
            ;;
        *)
            warn "Dependency installation cancelled."
            return 1
            ;;
    esac

    pick_elevator

    case "$PKG_MGR" in
        dnf)
            run_elevated dnf install -y "${deps[@]}"
            ;;
        pacman)
            run_elevated pacman -Syu --noconfirm "${deps[@]}"
            ;;
        apt)
            run_elevated apt update -qq
            run_elevated apt install -y "${deps[@]}"
            ;;
    esac

    ok "All dependencies installed."
}

# ── Build ───────────────────────────────────────────────────────────────────
do_build() {
    step "Building Nova Canvas Studio"

    local cmake_args=("-DCMAKE_BUILD_TYPE=$BUILD_TYPE")
    local generator="Ninja"

    # Enable the CUDA-GPU encode path when an NVIDIA CUDA toolkit is installed.
    # nvcc (in /usr/local/cuda/bin) is often not on the default PATH; without it
    # nvcc isn't found, CANVAS_HAVE_CUDA stays OFF, and the exporter's CUDA calls
    # fail to link. Detect common install layouts and expose nvcc so the GPU
    # path builds. Plain build.sh thus never needs sudo/pkexec for this.
    if ! command -v nvcc &>/dev/null; then
        for cuda_dir in "${CUDA_HOME:-}" /usr/local/cuda /opt/cuda /usr/cuda; do
            if [[ -n "$cuda_dir" && -x "$cuda_dir/bin/nvcc" ]]; then
                info "Found CUDA at $cuda_dir — enabling GPU encode path."
                export CUDA_HOME="$cuda_dir"
                export PATH="$cuda_dir/bin:$PATH"
                break
            fi
        done
    fi

    # Prefer Ninja if available, fall back to Unix Makefiles
    if command -v ninja &>/dev/null; then
        cmake_args+=("-G" "Ninja")
    else
        warn "Ninja not found — falling back to Unix Makefiles."
        cmake_args+=("-G" "Unix Makefiles")
    fi

    # Clean build requested?
    if [[ "$CLEAN_BUILD" -eq 1 ]]; then
        info "Cleaning previous build directory..."
        rm -rf "$BUILD_DIR"
    fi

    # Configure only if not already configured — otherwise just rebuild.
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        info "First-time config: cmake -B $BUILD_DIR ${cmake_args[*]}"
        cmake -B "$BUILD_DIR" "${cmake_args[@]}" 2>&1 | while IFS= read -r line; do
            printf "    ${DIM}%s${RESET}\n" "$line"
        done
    else
        info "Already configured — rebuilding incrementally."
        local cached_type
        cached_type=$(grep -E '^CMAKE_BUILD_TYPE:' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2) || true
        if [[ -n "$cached_type" && "$BUILD_TYPE" != "$cached_type" ]]; then
            warn "Re-running cmake to apply build type '$BUILD_TYPE' (cache has '$cached_type')."
            cmake -B "$BUILD_DIR" "${cmake_args[@]}" >/dev/null
        fi
    fi

    # Build (incremental — only recompiles changes)
    info "cmake --build $BUILD_DIR -j$JOBS"
    cmake --build "$BUILD_DIR" -j"$JOBS" 2>&1 | while IFS= read -r line; do
        printf "    ${DIM}%s${RESET}\n" "$line"
    done

    hr
    ok "${BOLD}Build complete!${RESET}"

    # Headless Qt-free guard (splitplan Phase 21): core/ and the extracted
    # GUI headless modules must never gain a <Q...> include.
    info "check_qtdep: verifying no Qt in headless modules"
    if ! "$(dirname "$0")/scripts/check_qtdep.sh" -q; then
        fail "check_qtdep FAILED — a headless module includes Qt"
    fi
    ok "check_qtdep: headless modules are Qt-free"

    printf "\n    ${CYAN}Binary:${RESET} ./$BUILD_DIR/gui/canvas\n"
    printf "    ${CYAN}Run:${RESET}    ./$BUILD_DIR/gui/canvas [file]\n\n"
}

# ── Usage ───────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}Usage:${RESET} $(basename "$0") [OPTIONS]

${BOLD}Options:${RESET}
  -d, --install-deps    Install build dependencies via system package manager
  -c, --clean           Remove build directory before configuring
  -r, --no-build        Skip build step (useful with -d only)
  -t, --type TYPE       Build type: Release, Debug, RelWithDebInfo (default: Release)
  -j, --jobs N          Parallel build jobs (default: $(nproc 2>/dev/null || echo 4))
  -h, --help            Show this help message
EOF
}

# ── Parse Arguments ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--install-deps) INSTALL_DEPS=1; shift ;;
        -c|--clean)        CLEAN_BUILD=1; shift ;;
        -r|--no-build)     RUN_BUILD=0; shift ;;
        -t|--type)         BUILD_TYPE="$2"; shift 2 ;;
        -j|--jobs)         JOBS="$2"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        *)                 warn "Unknown option: $1"; usage; exit 1 ;;
    esac
done

# ── Main ────────────────────────────────────────────────────────────────────
banner
detect_distro

if [[ "$INSTALL_DEPS" -eq 1 ]]; then
    install_deps
fi

if [[ "$RUN_BUILD" -eq 1 ]]; then
    do_build
else
    info "Build skipped (--no-build)."
fi

ok "${GREEN}${BOLD}Done!${RESET}"
