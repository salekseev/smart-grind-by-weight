#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

//==============================================================================
// CAPTIVE-PORTAL DNS RESPONDER
//==============================================================================
// While the setup access point is up, every name lookup has to resolve to the
// grinder so a phone's captive-portal detector opens the Wi-Fi setup page. This
// answers any A query with the access point's own address.
//
// It replaces the Arduino DNSServer with a small lwIP socket listener on its own
// task, so it never runs on the grind, load-cell or UI tasks.

class CaptiveDns {
public:
    /** Start answering queries with `address`, given in host byte order. */
    bool start(uint32_t address);
    void stop();

private:
    static void task_entry(void* context);
    void serve();

    TaskHandle_t task_ = nullptr;
    int socket_ = -1;
    uint32_t address_ = 0;
    volatile bool stop_requested_ = false;
};
