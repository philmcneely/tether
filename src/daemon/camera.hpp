#pragma once

#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <string>

namespace tether {

    // Takes ownership of a TLS connection that sent camera_start and, on a worker
    // thread, decodes its H.264 frames into a PipeWire Video/Source node. A new
    // stream replaces the running one. The node goes away when the connection ends.
    void start_camera_stream(
        int fd, SSL* ssl, const nlohmann::json& start, std::string leftover, const std::string& device_name);

} // namespace tether
