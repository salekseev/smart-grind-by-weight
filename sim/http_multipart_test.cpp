// Host tests for the streaming multipart/form-data reader used by the firmware
// and screensaver upload endpoints. The reader has to reassemble payloads whose
// boundary straddles two reads, which is the normal case for a 1.5 MB firmware
// image arriving in 512-byte chunks.
#include "network/http_multipart.cpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const char* kBoundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";

std::string content_type() {
    return std::string("multipart/form-data; boundary=") + kBoundary;
}

std::string build_body(const std::string& payload, const char* filename) {
    std::string body;
    body += std::string("--") + kBoundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"firmware\"; filename=\"";
    body += filename;
    body += "\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body += payload;
    body += std::string("\r\n--") + kBoundary + "--\r\n";
    return body;
}

/** Feed `body` in fixed-size slices and return the reassembled payload. */
std::string run(const std::string& body, size_t chunk_size, MultipartReader& reader) {
    std::string received;
    const auto sink = [&received](const uint8_t* data, size_t length) {
        received.append(reinterpret_cast<const char*>(data), length);
        return true;
    };
    for (size_t offset = 0; offset < body.size(); offset += chunk_size) {
        const size_t length = std::min(chunk_size, body.size() - offset);
        const bool ok =
            reader.feed(reinterpret_cast<const uint8_t*>(body.data() + offset), length, sink);
        assert(ok);
    }
    return received;
}

void test_single_chunk() {
    const std::string payload = "\xE9hello firmware";
    MultipartReader reader;
    assert(reader.begin(content_type()));
    const std::string received = run(build_body(payload, "fw.bin"), 4096, reader);
    assert(received == payload);
    assert(reader.complete());
    assert(reader.saw_file());
    assert(reader.filename() == "fw.bin");
    assert(reader.bytes_forwarded() == payload.size());
}

void test_boundary_split_across_chunks() {
    // A payload long enough that every chunk size below splits the closing
    // boundary at a different offset.
    std::string payload;
    for (int i = 0; i < 4096; ++i) payload += static_cast<char>(i & 0xFF);
    const std::string body = build_body(payload, "fw.bin");

    for (size_t chunk : {1u, 3u, 7u, 64u, 512u, 1000u}) {
        MultipartReader reader;
        assert(reader.begin(content_type()));
        const std::string received = run(body, chunk, reader);
        if (received != payload || !reader.complete()) {
            std::printf("FAIL: chunk size %zu produced %zu of %zu bytes (complete=%d)\n",
                        chunk, received.size(), payload.size(), reader.complete());
            assert(false);
        }
    }
}

void test_payload_containing_boundary_prefix() {
    // Bytes that look like the start of a boundary but are payload.
    std::string payload = "before\r\n--";
    payload += kBoundary;
    payload.pop_back();  // one character short of a real boundary
    payload += "X after";

    MultipartReader reader;
    assert(reader.begin(content_type()));
    const std::string received = run(build_body(payload, "fw.bin"), 13, reader);
    assert(received == payload);
    assert(reader.complete());
}

void test_quoted_and_parameterised_boundary() {
    MultipartReader quoted;
    assert(quoted.begin(std::string("multipart/form-data; boundary=\"") + kBoundary + "\""));

    MultipartReader trailing;
    assert(trailing.begin(std::string("multipart/form-data; boundary=") + kBoundary +
                          "; charset=utf-8"));

    MultipartReader mixed_case;
    assert(mixed_case.begin(std::string("Multipart/Form-Data; BOUNDARY=") + kBoundary));
}

void test_rejects_non_multipart() {
    MultipartReader reader;
    assert(!reader.begin("application/x-www-form-urlencoded"));
    assert(!reader.begin("multipart/form-data"));
    assert(!reader.begin("multipart/form-data; boundary="));
}

void test_aborting_sink_stops_the_stream() {
    MultipartReader reader;
    assert(reader.begin(content_type()));
    std::string payload(2048, 'x');
    const std::string body = build_body(payload, "fw.bin");
    const bool ok = reader.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                [](const uint8_t*, size_t) { return false; });
    assert(!ok);
    assert(!reader.complete());
}

}  // namespace

int main() {
    test_single_chunk();
    test_boundary_split_across_chunks();
    test_payload_containing_boundary_prefix();
    test_quoted_and_parameterised_boundary();
    test_rejects_non_multipart();
    test_aborting_sink_stops_the_stream();
    std::printf("http_multipart: all tests passed\n");
    return 0;
}
