#!/bin/bash
# Build script for the Emscripten C++ example

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WORKSPACE_ROOT="$(dirname "$PROJECT_ROOT")"
BUILD_TYPE="${1:-release}"  # debug or release

echo "Building c2pa Emscripten example..."
echo "Build type: $BUILD_TYPE"

# Step 1: Build the c2pa Rust library for Emscripten
echo ""
echo "Step 1: Building c2pa-c-ffi library..."
cd "$PROJECT_ROOT"

# Emscripten requires rebuilding the stdlib with +atomics,+bulk-memory so that
# panic_unwind (and the rest of std) is compatible with USE_PTHREADS=1.
# That requires nightly + -Z build-std.
if ! rustup toolchain list | grep -q '^nightly'; then
    echo "Error: nightly toolchain not found. Run: rustup toolchain install nightly"
    exit 1
fi
rustup target add --toolchain nightly wasm32-unknown-emscripten 2>/dev/null || true

if [ "$BUILD_TYPE" = "debug" ]; then
    cargo +nightly build -Z build-std=std,panic_unwind \
        -p c2pa-c-ffi \
        --target wasm32-unknown-emscripten \
        --no-default-features \
        --features "rust_native_crypto,file_io"

    C2PA_LIB="$WORKSPACE_ROOT/target/wasm32-unknown-emscripten/debug/libc2pa_c.a"
    C2PA_HEADER="$WORKSPACE_ROOT/target/wasm32-unknown-emscripten/debug/c2pa.h"
else
    cargo +nightly build -Z build-std=std,panic_unwind \
        -p c2pa-c-ffi \
        --target wasm32-unknown-emscripten \
        --release \
        --no-default-features \
        --features "rust_native_crypto,file_io"

    C2PA_LIB="$WORKSPACE_ROOT/target/wasm32-unknown-emscripten/release/libc2pa_c.a"
    C2PA_HEADER="$WORKSPACE_ROOT/target/wasm32-unknown-emscripten/release/c2pa.h"
fi

# Verify library was built
if [ ! -f "$C2PA_LIB" ]; then
    echo "Error: Library not found at $C2PA_LIB"
    exit 1
fi

if [ ! -f "$C2PA_HEADER" ]; then
    echo "Error: Header not found at $C2PA_HEADER"
    exit 1
fi

echo "✓ Library built: $C2PA_LIB"
echo "✓ Header found: $C2PA_HEADER"

# Step 2: Build the C++ example with Emscripten
echo ""
echo "Step 2: Building C++ example with Emscripten..."

# Check if emcc is available
if ! command -v emcc &> /dev/null; then
    echo "Error: emcc not found. Please install Emscripten SDK."
    echo "Visit: https://emscripten.org/docs/getting_started/downloads.html"
    exit 1
fi

OUTPUT_DIR="$WORKSPACE_ROOT/target/emscripten-example"
mkdir -p "$OUTPUT_DIR"

# Set optimization flags based on build type
if [ "$BUILD_TYPE" = "debug" ]; then
    OPT_FLAGS="-g -s ASSERTIONS=1 -s SAFE_HEAP=1"
    echo "Building with debug flags..."
else
    OPT_FLAGS="-O3 -s ASSERTIONS=0"
    echo "Building with release optimization..."
fi

# The Rust library was compiled with USE_PTHREADS=1 and +atomics,+bulk-memory
# (see .cargo/config.toml).  The emcc main module must match those settings.
emcc "$SCRIPT_DIR/emscripten_example.cpp" \
    -I"$(dirname "$C2PA_HEADER")" \
    "$C2PA_LIB" \
    -o "$OUTPUT_DIR/c2pa_example.js" \
    -pthread \
    -fwasm-exceptions \
    -s WASM=1 \
    -s USE_PTHREADS=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=256MB \
    -s MAXIMUM_MEMORY=2GB \
    -s EXPORTED_FUNCTIONS='["_main"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS"]' \
    -s FORCE_FILESYSTEM=1 \
    -s FETCH=1 \
    -s ENVIRONMENT='node,worker' \
    -s NODERAWFS=1 \
    -std=c++17 \
    $OPT_FLAGS

echo ""
echo "✓ Build complete!"
echo ""
echo "Output files:"
echo "  - $OUTPUT_DIR/c2pa_example.js"
echo "  - $OUTPUT_DIR/c2pa_example.wasm"
echo ""
echo "To run with Node.js:"
echo "  node $OUTPUT_DIR/c2pa_example.js path/to/image.jpg"
echo ""
echo "Note: the custom HTTP resolver (emscripten_fetch) requires a Web Worker"
echo "when running in a browser.  Under Node.js it works on the main thread."
