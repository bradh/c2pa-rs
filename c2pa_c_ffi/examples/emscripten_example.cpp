// Example: Using c2pa library in an Emscripten C++ project
// Build: see build_emscripten_example.sh
//
// HTTP resolver note:
//   The sections below that use c2pa_http_resolver_create() require:
//     - Compiling with: -s FETCH=1
//     - Running in a Web Worker (EMSCRIPTEN_FETCH_SYNCHRONOUS is not
//       available on the browser main thread).
//   When building for Node.js there is no such restriction.

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "c2pa.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/fetch.h>
#endif

// Helper class for automatic string cleanup
class C2paString {
    char* str_;
public:
    explicit C2paString(char* str) : str_(str) {}
    ~C2paString() { if (str_) c2pa_string_free(str_); }
    
    const char* get() const { return str_; }
    bool is_null() const { return str_ == nullptr; }
    
    // Prevent copying
    C2paString(const C2paString&) = delete;
    C2paString& operator=(const C2paString&) = delete;
};

// ---------------------------------------------------------------------------
// Custom HTTP resolver backed by emscripten_fetch
// ---------------------------------------------------------------------------
//
// This callback is invoked by the Rust SDK whenever it needs to make an HTTP
// request (remote manifest fetch, OCSP, timestamp, etc.).  It must be called
// from a Web Worker when targeting the browser because synchronous fetch is
// only permitted off the main thread.  Under Node.js there is no restriction.
//
// Signature matches C2paHttpResolverCallback:
//   >= 0  → bytes written into resp_buf
//   < 0   → error; if abs(return) > resp_cap the SDK retries with a larger
//            buffer (abs(return) is treated as the required size)
// ---------------------------------------------------------------------------

#ifdef __EMSCRIPTEN__

// Parse a "Name: Value\nName2: Value2\n" string into a NULL-terminated
// char* array suitable for emscripten_fetch_attr_t::requestHeaders.
// The returned vector owns the storage; its .data() is the pointer to pass.
static std::vector<const char*> parse_headers(const std::string& raw) {
    std::vector<const char*> result;
    static std::vector<std::string> storage; // lifetime tied to the call
    storage.clear();

    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        storage.push_back(line.substr(0, colon));          // name
        storage.push_back(line.substr(colon + 2));         // value (skip ": ")
    }
    for (auto& s : storage) result.push_back(s.c_str());
    result.push_back(nullptr); // terminator
    return result;
}

static int32_t emscripten_http_handler(
    void*          /*ctx*/,
    const char*    url,
    const char*    method,
    const char*    headers_str,
    const uint8_t* body,
    size_t         body_len,
    uint8_t*       resp_buf,
    size_t         resp_cap,
    int32_t*       status_out)
{
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);

    // Synchronous mode: blocks until the response arrives.
    // Only valid in a Web Worker; will abort on the browser main thread.
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY
                    | EMSCRIPTEN_FETCH_SYNCHRONOUS;

    // HTTP method (GET, POST, …)
    strncpy(attr.requestMethod, method, sizeof(attr.requestMethod) - 1);
    attr.requestMethod[sizeof(attr.requestMethod) - 1] = '\0';

    // Request body (may be nullptr / 0 for GET)
    if (body && body_len > 0) {
        attr.requestData     = reinterpret_cast<const char*>(body);
        attr.requestDataSize = body_len;
    }

    // Request headers
    auto header_vec = parse_headers(headers_str ? headers_str : "");
    if (header_vec.size() > 1) { // more than just the nullptr terminator
        attr.requestHeaders = header_vec.data();
    }

    emscripten_fetch_t* fetch = emscripten_fetch(&attr, url);
    if (!fetch) {
        c2pa_error_set_last("emscripten_fetch returned null");
        return -1;
    }

    *status_out = static_cast<int32_t>(fetch->status);

    size_t needed = static_cast<size_t>(fetch->numBytes);
    if (needed > resp_cap) {
        // Signal the SDK to retry with a larger buffer.
        emscripten_fetch_close(fetch);
        return -static_cast<int32_t>(needed);
    }

    memcpy(resp_buf, fetch->data, needed);
    emscripten_fetch_close(fetch);
    return static_cast<int32_t>(needed);
}

// Demonstrates reading a JPEG that contains a *remote* manifest URL.
// The SDK will call emscripten_http_handler() to fetch that manifest.
static void read_with_http_resolver(const uint8_t* data, size_t data_size) {
    std::cout << "\nReading remote manifest with custom HTTP resolver" << std::endl;

    // 1. Build a context that uses our HTTP callback.
    C2paContextBuilder* builder  = c2pa_context_builder_new();
    C2paHttpResolver*   resolver = c2pa_http_resolver_create(
        nullptr,                  // opaque user context (unused here)
        emscripten_http_handler); // our callback

    if (c2pa_context_builder_set_http_resolver(builder, resolver) != 0) {
        char* err = c2pa_error();
        std::cerr << "set_http_resolver failed: " << (err ? err : "?") << std::endl;
        c2pa_string_free(err);
        c2pa_free(builder);
        return;
    }
    // resolver is now owned by the builder — do NOT free it separately.

    C2paContext* ctx = c2pa_context_builder_build(builder);
    // builder is consumed; ctx is the live handle.

    if (!ctx) {
        char* err = c2pa_error();
        std::cerr << "context build failed: " << (err ? err : "?") << std::endl;
        c2pa_string_free(err);
        return;
    }

    // 2. Create an in-memory stream over the JPEG bytes.
    //    StreamContext is an opaque empty struct; cast your own data through it.
    struct StreamCtx { const uint8_t* data; size_t size; size_t pos; };
    StreamCtx sctx = { data, data_size, 0 };

    // Callback signatures must exactly match ReadCallback / SeekCallback.
    ReadCallback read_cb = [](StreamContext* c, uint8_t* buf, intptr_t len) -> intptr_t {
        auto* s = reinterpret_cast<StreamCtx*>(c);
        size_t avail   = s->size - s->pos;
        size_t to_read = static_cast<size_t>(len) < avail
                       ? static_cast<size_t>(len) : avail;
        if (to_read) { memcpy(buf, s->data + s->pos, to_read); s->pos += to_read; }
        return static_cast<intptr_t>(to_read);
    };
    SeekCallback seek_cb = [](StreamContext* c, intptr_t off, C2paSeekMode mode) -> intptr_t {
        auto* s = reinterpret_cast<StreamCtx*>(c);
        size_t new_pos;
        switch (mode) {
            case Start:   new_pos = static_cast<size_t>(off); break;
            case Current: new_pos = static_cast<size_t>(static_cast<intptr_t>(s->pos) + off); break;
            case End:     new_pos = static_cast<size_t>(static_cast<intptr_t>(s->size) + off); break;
            default:      return -1;
        }
        if (new_pos > s->size) return -1;
        s->pos = new_pos;
        return static_cast<intptr_t>(new_pos);
    };

    C2paStream* stream = c2pa_create_stream(
        reinterpret_cast<StreamContext*>(&sctx), read_cb, seek_cb, nullptr, nullptr);
    if (!stream) {
        std::cerr << "Failed to create stream" << std::endl;
        c2pa_free(ctx);
        return;
    }

    // 3. Create a reader from the context (which carries the HTTP resolver),
    //    then configure it with the stream.  The first reader pointer is
    //    consumed by c2pa_reader_with_stream.
    C2paReader* reader = c2pa_reader_from_context(ctx);
    if (reader) {
        reader = c2pa_reader_with_stream(reader, "image/jpeg", stream);
    }
    if (reader) {
        char* json = c2pa_reader_json(reader);
        std::cout << "Manifest JSON:\n" << (json ? json : "(null)") << std::endl;
        c2pa_string_free(json);
        c2pa_free(reader);
    } else {
        char* err = c2pa_error();
        std::cerr << "Reader error: " << (err ? err : "?") << std::endl;
        c2pa_string_free(err);
    }

    c2pa_release_stream(stream);
    c2pa_free(ctx);
}

#endif // __EMSCRIPTEN__

// ---------------------------------------------------------------------------

void print_version() {
    C2paString version(c2pa_version());
    std::cout << "C2PA Library Version: " << version.get() << std::endl;
}

void read_manifest(const char* path) {
    std::cout << "\nReading C2PA manifest from: " << path << std::endl;

    C2paString manifest(c2pa_read_file(path, nullptr));

    if (manifest.is_null()) {
        C2paString error(c2pa_error());
        std::cerr << "Error reading file: " << error.get() << std::endl;
        return;
    }

    std::cout << "Manifest JSON:\n" << manifest.get() << std::endl;
}

void read_with_stream(const uint8_t* data, size_t data_size) {
    std::cout << "\nReading C2PA manifest from memory stream" << std::endl;
    
    // Create stream context
    struct Context {
        const uint8_t* data;
        size_t size;
        size_t position;
    };
    
    Context ctx = { data, data_size, 0 };

    ReadCallback read_cb = [](StreamContext* context, uint8_t* buffer, intptr_t length) -> intptr_t {
        Context* ctx = reinterpret_cast<Context*>(context);
        size_t available = ctx->size - ctx->position;
        size_t to_read = static_cast<size_t>(length) < available
                       ? static_cast<size_t>(length) : available;
        if (to_read > 0) {
            std::memcpy(buffer, ctx->data + ctx->position, to_read);
            ctx->position += to_read;
        }
        return static_cast<intptr_t>(to_read);
    };

    SeekCallback seek_cb = [](StreamContext* context, intptr_t offset, C2paSeekMode mode) -> intptr_t {
        Context* ctx = reinterpret_cast<Context*>(context);
        size_t new_pos;
        switch (mode) {
            case Start:   new_pos = static_cast<size_t>(offset); break;
            case Current: new_pos = static_cast<size_t>(static_cast<intptr_t>(ctx->position) + offset); break;
            case End:     new_pos = static_cast<size_t>(static_cast<intptr_t>(ctx->size) + offset); break;
            default:      return -1;
        }
        if (new_pos > ctx->size) return -1;
        ctx->position = new_pos;
        return static_cast<intptr_t>(new_pos);
    };

    C2paStream* stream = c2pa_create_stream(
        reinterpret_cast<StreamContext*>(&ctx),
        read_cb, seek_cb,
        nullptr,  // no write
        nullptr   // no flush
    );

    if (!stream) {
        std::cerr << "Failed to create stream" << std::endl;
        return;
    }

    C2paReader* reader = c2pa_reader_from_stream("image/jpeg", stream);

    if (reader) {
        C2paString json(c2pa_reader_json(reader));
        std::cout << "Manifest JSON:\n" << json.get() << std::endl;

        bool embedded = c2pa_reader_is_embedded(reader);
        std::cout << "Manifest is " << (embedded ? "embedded" : "remote") << std::endl;

        c2pa_free(reader);
    } else {
        C2paString error(c2pa_error());
        std::cerr << "Error reading stream: " << error.get() << std::endl;
    }

    c2pa_release_stream(stream);
}

int main(int argc, char* argv[]) {
    std::cout << "=== C2PA Emscripten Example ===" << std::endl;
    
    // Print version
    print_version();
    
    // Example 1: Read from file path
    if (argc > 1) {
        read_manifest(argv[1]);
    } else {
        std::cout << "\nUsage: " << argv[0] << " <image_path>" << std::endl;
        std::cout << "No file provided, skipping file read example." << std::endl;
    }
    
    // Example 2: Read from memory stream
    if (argc > 1) {
        std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
        if (f) {
            std::streamsize sz = f.tellg();
            f.seekg(0, std::ios::beg);
            std::vector<uint8_t> image_data(static_cast<size_t>(sz));
            if (f.read(reinterpret_cast<char*>(image_data.data()), sz)) {
                read_with_stream(image_data.data(), image_data.size());
            }
        }
    }

#ifdef __EMSCRIPTEN__
    // Example 3: Read a JPEG that contains a remote manifest URL, using a
    // custom HTTP resolver backed by emscripten_fetch.
    //
    // IMPORTANT: emscripten_fetch in synchronous mode only works from a Web
    // Worker.  If running in the browser main thread, the fetch call will
    // abort with an error.  Under Node.js there is no such restriction.
    //
    // Supply an image file as the first argument to exercise this path.
    if (argc > 1) {
        // Re-read the file into memory for the stream-based example.
        std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
        if (f) {
            std::streamsize sz = f.tellg();
            f.seekg(0, std::ios::beg);
            std::vector<uint8_t> buf(static_cast<size_t>(sz));
            if (f.read(reinterpret_cast<char*>(buf.data()), sz)) {
                read_with_http_resolver(buf.data(), buf.size());
            }
        }
    }
#endif

    std::cout << "\n=== Example Complete ===" << std::endl;
    return 0;
}
