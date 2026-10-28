#pragma once

#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <string>

namespace tether {

    // Whether this build can decode the phone's H.264; without it, camera is not advertised.
    bool camera_supported();

    // Takes ownership of a TLS connection that sent camera_start and, on a worker
    // thread, decodes its H.264 frames into a PipeWire Video/Source node. A new
    // stream replaces the running one. When a stream ends the node stays and shows
    // black, so apps keep it selected for the next stream. Never throws.
    void start_camera_stream(
        int fd, SSL* ssl, const nlohmann::json& start, std::string leftover, const std::string& device_name);

} // namespace tether
