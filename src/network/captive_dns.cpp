#include "captive_dns.h"

#include <lwip/sockets.h>

#include <cstring>
#include <vector>

#include "../config/constants.h"

namespace {

constexpr uint16_t kDnsPort = 53;
constexpr size_t kMaxQuerySize = 512;
constexpr uint32_t kAnswerTtlSeconds = 60;
constexpr uint32_t kTaskStackSize = 3072;
constexpr UBaseType_t kTaskPriority = 1;

// A DNS header is six 16-bit fields; only the flags and counts are rewritten.
constexpr size_t kHeaderSize = 12;
constexpr size_t kFlagsOffset = 2;
constexpr size_t kQuestionCountOffset = 4;
constexpr size_t kAnswerCountOffset = 6;

void write_be16(uint8_t* buffer, uint16_t value) {
    buffer[0] = static_cast<uint8_t>(value >> 8);
    buffer[1] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t read_be16(const uint8_t* buffer) {
    return static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);
}

/** Length of the QNAME plus QTYPE and QCLASS, or 0 when malformed. */
size_t question_length(const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const uint8_t label = data[offset];
        if (label == 0) {
            // Root label, then QTYPE and QCLASS.
            return offset + 1 + 4 <= size ? offset + 5 : 0;
        }
        // Compression pointers are not valid in a question section.
        if ((label & 0xC0) != 0) return 0;
        offset += label + 1;
    }
    return 0;
}

}  // namespace

bool CaptiveDns::start(uint32_t address) {
    if (task_) return true;

    address_ = address;
    stop_requested_ = false;

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        LOG_BLE("[DNS] Could not create socket\n");
        return false;
    }

    sockaddr_in bind_address = {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_address.sin_port = htons(kDnsPort);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)) < 0) {
        LOG_BLE("[DNS] Could not bind port %u\n", kDnsPort);
        close(socket_);
        socket_ = -1;
        return false;
    }

    // A receive timeout lets the task notice a stop request promptly.
    timeval timeout = {};
    timeout.tv_sec = 1;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (xTaskCreate(task_entry, "captive_dns", kTaskStackSize, this, kTaskPriority, &task_) !=
        pdPASS) {
        LOG_BLE("[DNS] Could not start responder task\n");
        close(socket_);
        socket_ = -1;
        task_ = nullptr;
        return false;
    }

    LOG_BLE("[DNS] Captive portal responder listening\n");
    return true;
}

void CaptiveDns::stop() {
    if (!task_) return;
    stop_requested_ = true;
    // The task closes the socket and clears task_ on its way out.
    while (task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOG_BLE("[DNS] Captive portal responder stopped\n");
}

void CaptiveDns::task_entry(void* context) {
    static_cast<CaptiveDns*>(context)->serve();
}

void CaptiveDns::serve() {
    std::vector<uint8_t> query(kMaxQuerySize);
    std::vector<uint8_t> response(kMaxQuerySize + 16);

    while (!stop_requested_) {
        sockaddr_in from = {};
        socklen_t from_length = sizeof(from);
        const int received = recvfrom(socket_, query.data(), query.size(), 0,
                                     reinterpret_cast<sockaddr*>(&from), &from_length);
        if (received < static_cast<int>(kHeaderSize)) continue;

        const size_t size = static_cast<size_t>(received);
        const size_t question_size = question_length(query.data() + kHeaderSize,
                                                     size - kHeaderSize);
        // Only answer a single-question standard query for an address record.
        if (question_size == 0 || read_be16(query.data() + kQuestionCountOffset) != 1) continue;

        const uint8_t* question = query.data() + kHeaderSize;
        const uint16_t qtype = read_be16(question + question_size - 4);
        if (qtype != 1 /* A */) continue;

        const size_t query_end = kHeaderSize + question_size;
        std::memcpy(response.data(), query.data(), query_end);
        // Standard response, authoritative, no error.
        write_be16(response.data() + kFlagsOffset, 0x8180);
        write_be16(response.data() + kAnswerCountOffset, 1);

        size_t offset = query_end;
        // Answer: a pointer back to the question's name, then A/IN, TTL, 4 bytes.
        write_be16(response.data() + offset, 0xC000 | kHeaderSize);
        offset += 2;
        write_be16(response.data() + offset, 1);  // type A
        offset += 2;
        write_be16(response.data() + offset, 1);  // class IN
        offset += 2;
        write_be16(response.data() + offset, static_cast<uint16_t>(kAnswerTtlSeconds >> 16));
        offset += 2;
        write_be16(response.data() + offset, static_cast<uint16_t>(kAnswerTtlSeconds & 0xFFFF));
        offset += 2;
        write_be16(response.data() + offset, 4);  // address length
        offset += 2;
        response[offset++] = static_cast<uint8_t>((address_ >> 24) & 0xFF);
        response[offset++] = static_cast<uint8_t>((address_ >> 16) & 0xFF);
        response[offset++] = static_cast<uint8_t>((address_ >> 8) & 0xFF);
        response[offset++] = static_cast<uint8_t>(address_ & 0xFF);

        sendto(socket_, response.data(), offset, 0, reinterpret_cast<sockaddr*>(&from),
               from_length);
    }

    close(socket_);
    socket_ = -1;
    TaskHandle_t self = task_;
    task_ = nullptr;
    vTaskDelete(self);
}
