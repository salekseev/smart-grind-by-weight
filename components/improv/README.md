# improv

Vendored copy of the [Improv Wi-Fi C++ SDK](https://github.com/improv-wifi/sdk-cpp)
(v1.2.6), used by `ProvisioningService` to speak the Improv serial protocol.

The upstream sources are unmodified. Every Arduino-specific declaration in them is
already guarded by `#ifdef ARDUINO`, so the library builds as a plain ESP-IDF
component. Refresh it by copying `src/improv.h` and `src/improv.cpp` from
upstream; do not edit the files in place.
