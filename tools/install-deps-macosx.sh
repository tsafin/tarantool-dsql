#!/usr/bin/env bash
#
# Install build and test dependencies for tarantool on macOS (Homebrew).
# Tested on macOS 15+ (arm64) with Apple clang 21 and CMake 4.x.
#
# Usage:
#   bash tools/install-deps-macosx.sh          # build deps only
#   bash tools/install-deps-macosx.sh --test   # build + test deps
#   bash tools/install-deps-macosx.sh --jit    # build + LLVM JIT deps
#   bash tools/install-deps-macosx.sh --all    # everything
#
set -euo pipefail

WANT_TEST=0
WANT_JIT=0

for arg in "$@"; do
    case "$arg" in
        --test) WANT_TEST=1 ;;
        --jit)  WANT_JIT=1  ;;
        --all)  WANT_TEST=1; WANT_JIT=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# 1. Homebrew build dependencies
# ---------------------------------------------------------------------------
echo "==> Installing Homebrew build dependencies..."
brew install \
    cmake \
    make \
    openssl@3 \
    readline \
    icu4c \
    libiconv \
    zlib \
    python3

# ---------------------------------------------------------------------------
# 2. LLVM (required for ENABLE_SQL_JIT or ENABLE_SQL_CNP)
#    llvm is keg-only — not symlinked into /opt/homebrew by default.
#    Pass -DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm to cmake.
#    Note: ENABLE_SQL_CNP disassembler is wired for x86 only; on arm64
#    LLVM is still required for cmake to find the package but no x86
#    libraries are linked.
# ---------------------------------------------------------------------------
if [ "$WANT_JIT" -eq 1 ]; then
    echo "==> Installing LLVM for JIT builds..."
    brew install llvm
fi

# ---------------------------------------------------------------------------
# 3. Python packages for the test harness (test-run)
# ---------------------------------------------------------------------------
if [ "$WANT_TEST" -eq 1 ]; then
    echo "==> Installing Python test dependencies..."
    pip3 install \
        pyyaml \
        gevent \
        python-daemon \
        six \
        msgpack \
        tarantool
fi

# ---------------------------------------------------------------------------
# 4. Git submodules (including nested ones inside test-run)
# ---------------------------------------------------------------------------
echo "==> Initialising git submodules..."
git submodule update --init --recursive
# test-run has its own nested submodules (luatest, checks, etc.)
git -C test-run submodule update --init 2>/dev/null || true

# ---------------------------------------------------------------------------
# 5. Ensure at least one annotated git tag exists so git describe works.
#    The top-level CMakeLists.txt uses `git describe --long HEAD` to set
#    the build version; without any tag it fails with a fatal error.
# ---------------------------------------------------------------------------
if ! git describe --long HEAD >/dev/null 2>&1; then
    echo "==> No annotated git tags found; creating a placeholder 2.11.0 tag..."
    HASH=$(git rev-parse --short HEAD)
    git tag -a 2.11.0 -m "placeholder version tag for build" HEAD
    echo "    Tagged HEAD ($HASH) as 2.11.0"
fi

echo ""
echo "==> Done."
echo ""
echo "Build with (Homebrew, arm64 example):"
echo ""
echo "  mkdir -p build && cd build"
if [ "$WANT_JIT" -eq 1 ]; then
echo "  cmake .. \\"
echo "    -DCMAKE_BUILD_TYPE=Debug \\"
echo "    -DENABLE_SQL_JIT=ON \\"
echo "    -DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm \\"
echo "    -DCMAKE_PREFIX_PATH=\"\$(brew --prefix openssl@3);\$(brew --prefix readline);\$(brew --prefix icu4c);\$(brew --prefix libiconv);\$(brew --prefix zlib);\$(brew --prefix llvm)\" \\"
echo "    -DDARWIN_BUILD_TYPE=None \\"
echo "    -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
else
echo "  cmake .. \\"
echo "    -DCMAKE_BUILD_TYPE=Debug \\"
echo "    -DCMAKE_PREFIX_PATH=\"\$(brew --prefix openssl@3);\$(brew --prefix readline);\$(brew --prefix icu4c);\$(brew --prefix libiconv);\$(brew --prefix zlib)\" \\"
echo "    -DDARWIN_BUILD_TYPE=None \\"
echo "    -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
fi
echo "  make -j\$(sysctl -n hw.ncpu) tarantool"
