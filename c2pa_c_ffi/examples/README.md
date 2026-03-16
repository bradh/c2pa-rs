# C2PA Emscripten Examples

This directory contains examples demonstrating how to use the c2pa library in Emscripten/WebAssembly C++ projects.

## Prerequisites

1. **Rust nightly toolchain** - Required to rebuild the stdlib with `+atomics,+bulk-memory`:
   ```bash
   rustup toolchain install nightly
   rustup target add --toolchain nightly wasm32-unknown-emscripten
   ```

2. **Emscripten SDK** - Install from [emscripten.org](https://emscripten.org/docs/getting_started/downloads.html):
   ```bash
   git clone https://github.com/emscripten-core/emsdk.git
   cd emsdk
   ./emsdk install latest
   ./emsdk activate latest
   source ./emsdk_env.sh
   ```

## Quick Start

### Option 1: Direct Build with Emscripten

Build and run the example using the shell script:

```bash
# Build release version
./build_emscripten_example.sh release

# Or build debug version
./build_emscripten_example.sh debug
```

Output will be in `build/` (relative to this directory).

### Option 2: CMake Build

For integration into larger CMake projects:

```bash
# Build release version
./build_cmake_example.sh Release

# Or build debug version
./build_cmake_example.sh Debug
```

Output will be in `build-emscripten-Release/` or `build-emscripten-Debug/`

## Running the Examples

### With Node.js (Recommended)

```bash
node c2pa_example.js path/to/image_with_c2pa.jpg
```

### In a Web Browser

> **Note**: The output is a `.js` module, not a standalone `.html` page. Running in the
> browser requires a Web Worker (because `emscripten_fetch` synchronous mode is not
> permitted on the main thread) and the page must be served with COOP/COEP headers for
> `SharedArrayBuffer`:
>
> ```
> Cross-Origin-Opener-Policy: same-origin
> Cross-Origin-Embedder-Policy: require-corp
> ```

## Example Code Overview

### emscripten_example.cpp

The example demonstrates:

1. **Version Information**: Getting the c2pa library version
2. **File Reading**: Reading C2PA manifests from files (`c2pa_read_file`)
3. **Stream API**: Reading C2PA data from memory buffers via `c2pa_reader_from_stream`
4. **Custom HTTP Resolver**: Reading remote manifests using `emscripten_fetch` via
   `c2pa_http_resolver_create` / `c2pa_context_builder_set_http_resolver`
5. **Memory Management**: Proper cleanup of C strings with `C2paString` RAII wrapper
6. **Error Handling**: Checking and displaying error messages

Key patterns:

```cpp
// Reading from a file
char* manifest = c2pa_read_file(path, nullptr);
c2pa_string_free(manifest);

// Reading from a memory stream
C2paStream* stream = c2pa_create_stream(
    reinterpret_cast<StreamContext*>(&ctx), read_cb, seek_cb, nullptr, nullptr);
C2paReader* reader = c2pa_reader_from_stream("image/jpeg", stream);

// Reading with a custom HTTP resolver (wasm32 only)
C2paContextBuilder* builder  = c2pa_context_builder_new();
C2paHttpResolver*   resolver = c2pa_http_resolver_create(nullptr, my_http_handler);
c2pa_context_builder_set_http_resolver(builder, resolver);
C2paContext* ctx    = c2pa_context_builder_build(builder);
C2paReader*  reader = c2pa_reader_from_context(ctx);
reader = c2pa_reader_with_stream(reader, "image/jpeg", stream);
```

## Integration into Your Project

### Method 1: Static Linking (Recommended)

1. Build the c2pa library for the wasm32 target. **Nightly is required** to rebuild the
   stdlib with `+atomics,+bulk-memory` (needed because the Rust lib uses `USE_PTHREADS=1`):
   ```bash
   cargo +nightly build -Z build-std=std,panic_unwind \
     -p c2pa-c-ffi \
     --target wasm32-unknown-emscripten \
     --release \
     --no-default-features \
     --features "rust_native_crypto,file_io"
   ```

2. Link in your project. The `-pthread` and `-fwasm-exceptions` flags **must** match
   how the Rust library was compiled:
   ```bash
   emcc your_code.cpp \
     -I../target/wasm32-unknown-emscripten/release \
     ../target/wasm32-unknown-emscripten/release/libc2pa_c.a \
     -o output.js \
     -pthread \
     -fwasm-exceptions \
     -s WASM=1 \
     -s USE_PTHREADS=1 \
     -s ALLOW_MEMORY_GROWTH=1 \
     -s INITIAL_MEMORY=256MB \
     -s MAXIMUM_MEMORY=2GB \
     -s FETCH=1 \
     -s ENVIRONMENT=node,worker \
     -s NODERAWFS=1
   ```

### Method 2: CMake Integration

Add to your `CMakeLists.txt`:

```cmake
# Set c2pa library path
set(C2PA_LIB_DIR "path/to/c2pa-rs/target/wasm32-unknown-emscripten/release")

# Include headers
target_include_directories(your_target PRIVATE ${C2PA_LIB_DIR})

# Link library
target_link_libraries(your_target ${C2PA_LIB_DIR}/libc2pa_c.a)

# Set Emscripten flags (-pthread and -fwasm-exceptions are required)
set_target_properties(your_target PROPERTIES
    LINK_FLAGS "-pthread -fwasm-exceptions -s WASM=1 -s USE_PTHREADS=1 \
                -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=268435456 \
                -s FETCH=1 -s ENVIRONMENT=node,worker -s NODERAWFS=1 \
                -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS']"
)
```

## API Reference

See the generated `c2pa.h` header file for complete API documentation:
- `../target/wasm32-unknown-emscripten/release/c2pa.h`

Key functions:

- `c2pa_version()` - Get library version
- `c2pa_read_file()` - Read C2PA manifest from file
- `c2pa_reader_from_stream()` - Read from memory stream
- `c2pa_reader_from_context()` - Create a reader from a context (for custom HTTP resolver)
- `c2pa_reader_with_stream()` - Configure a reader with a stream
- `c2pa_http_resolver_create()` - Create a C-callback-based HTTP resolver (wasm32 only)
- `c2pa_context_builder_set_http_resolver()` - Attach resolver to a context builder (wasm32 only)
- `c2pa_string_free()` - Free returned strings
- `c2pa_free()` - Free any C2PA object (reader, context, stream, etc.)
- `c2pa_error()` - Get last error message

## Memory Management

**Critical**: Always free strings returned by the C2PA library:

```cpp
char* manifest = c2pa_read_file(path, nullptr);
if (manifest) {
    // Use the manifest...
    c2pa_string_free(manifest);  // REQUIRED!
}
```

The `C2paString` helper class in the example does this automatically via RAII.

## Common Issues

### Build Errors

**Problem**: `emcc not found`
- **Solution**: Install and activate Emscripten SDK

**Problem**: `libc2pa_c.a not found`
- **Solution**: Build the Rust library first with `cargo +nightly build -Z build-std=std,panic_unwind ...`

**Problem**: `--shared-memory is disallowed` / atomics error
- **Solution**: Use `cargo +nightly build -Z build-std=std,panic_unwind` — plain `cargo build` does not rebuild the stdlib with the required flags

**Problem**: `__cpp_exception` undefined / wasm-exceptions linker error
- **Solution**: Add `-fwasm-exceptions` to your emcc command — the Rust lib requires it

**Problem**: Undefined symbols during linking
- **Solution**: Ensure all required features are enabled; check that `-pthread -s USE_PTHREADS=1` are present

### Runtime Errors

**Problem**: Memory allocation errors
- **Solution**: Increase memory limits: `-s INITIAL_MEMORY=256MB -s MAXIMUM_MEMORY=2GB -s ALLOW_MEMORY_GROWTH=1`

**Problem**: File not found (`std::ifstream` returns empty)
- **Solution**: Add `-s NODERAWFS=1` to let Emscripten access the host filesystem under Node.js

**Problem**: Async operations / remote manifests not working
- **Solution**: Register a custom HTTP resolver via `c2pa_http_resolver_create`; see
  [EMSCRIPTEN_USAGE.md](../EMSCRIPTEN_USAGE.md#custom-http-handler-wasm32-only)

**Problem**: `emscripten_fetch` aborts in the browser
- **Solution**: `EMSCRIPTEN_FETCH_SYNCHRONOUS` only works from a Web Worker. Move the
  fetch call off the main thread, or use the async fetch API on the main thread.

## Performance Tips

### For Production Builds

```bash
emcc ... \
  -O3 \
  -s ASSERTIONS=0 \
  -flto \
  --closure 1
```

### For Development/Debugging

```bash
emcc ... \
  -g \
  -s ASSERTIONS=1 \
  -s SAFE_HEAP=1 \
  -s STACK_OVERFLOW_CHECK=2
```

## Additional Documentation

- [EMSCRIPTEN_USAGE.md](../EMSCRIPTEN_USAGE.md) - Comprehensive integration guide
- [C2PA C FFI README](../README.md) - C API documentation
- [Emscripten Documentation](https://emscripten.org/docs/)

## Testing with Real Images

To test with images containing C2PA data:

1. Download sample images from [C2PA Test Fixtures](https://github.com/contentauth/c2pa-rs/tree/main/sdk/tests/fixtures)
2. Use the C2PA tool to add manifests: [c2patool](https://github.com/contentauth/c2patool)

Example:
```bash
# Download test image
curl -O https://raw.githubusercontent.com/contentauth/c2pa-rs/main/sdk/tests/fixtures/C.jpg

# Run the example
node c2pa_example.js C.jpg
```
