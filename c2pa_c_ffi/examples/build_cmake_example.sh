#!/bin/bash
# Build script using CMake for the Emscripten C++ example

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WORKSPACE_ROOT="$(dirname "$PROJECT_ROOT")"
BUILD_TYPE="${1:-Release}"  # Debug or Release

echo "Building c2pa Emscripten example with CMake..."
echo "Build type: $BUILD_TYPE"

# Step 1: Build the c2pa Rust library
echo ""
echo "Step 1: Building c2pa-c-ffi library..."
cd "$WORKSPACE_ROOT"

# Emscripten requires rebuilding the stdlib with +atomics,+bulk-memory.
if ! rustup toolchain list | grep -q '^nightly'; then
    echo "Error: nightly toolchain not found. Run: rustup toolchain install nightly"
    exit 1
fi
rustup target add --toolchain nightly wasm32-unknown-emscripten 2>/dev/null || true

if [ "$BUILD_TYPE" = "Debug" ]; then
    cargo +nightly build -Z build-std=std,panic_unwind \
        -p c2pa-c-ffi \
        --target wasm32-unknown-emscripten \
        --no-default-features \
        --features "rust_native_crypto,file_io"
else
    cargo +nightly build -Z build-std=std,panic_unwind \
        -p c2pa-c-ffi \
        --target wasm32-unknown-emscripten \
        --release \
        --no-default-features \
        --features "rust_native_crypto,file_io"
fi

echo "✓ Rust library built"

# Step 2: Check for Emscripten
if ! command -v emcc &> /dev/null; then
    echo "Error: emcc not found. Please install Emscripten SDK."
    exit 1
fi

# Step 3: Build with CMake
echo ""
echo "Step 2: Building with CMake..."

BUILD_DIR="$SCRIPT_DIR/build-emscripten-$BUILD_TYPE"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cd "$BUILD_DIR"

# Configure
emcmake cmake .. \
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    -DC2PA_ROOT="$WORKSPACE_ROOT"

# Build
emmake make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "✓ Build complete!"
echo ""
echo "Output files in: $BUILD_DIR"
echo "  - c2pa_example.js"
echo "  - c2pa_example.wasm"
echo ""
echo "To run with Node.js:"
echo "  node $BUILD_DIR/c2pa_example.js path/to/image.jpg"
echo ""
echo "Note: browser use requires SharedArrayBuffer (COOP/COEP headers)"
