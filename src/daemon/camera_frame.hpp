#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tether {

    inline constexpr size_t CAMERA_FRAME_MAX_BYTES = 4u * 1024 * 1024;
    inline constexpr size_t CAMERA_FRAME_HEADER_BYTES = 4 + 1 + 8;

    // One camera frame
    struct CameraFrame {
        bool keyframe = false;
        uint64_t pts_us = 0;
        std::string_view payload;
    };

    // Parses the frame at the front of buf into out. Returns the bytes it spans,
    // 0 when buf holds only part of it, or -1 when the length is out of range.
    inline long parse_camera_frame(std::string_view buf, CameraFrame& out) {
        if (buf.size() < 4)
            return 0;
        const auto* p = reinterpret_cast<const unsigned char*>(buf.data());
        const size_t len = (size_t(p[0]) << 24) | (size_t(p[1]) << 16) | (size_t(p[2]) << 8) | p[3];
        if (len < CAMERA_FRAME_HEADER_BYTES - 4 || len > CAMERA_FRAME_MAX_BYTES)
            return -1;
        if (buf.size() < 4 + len)
            return 0;
        out.keyframe = p[4] & 1;
        out.pts_us = 0;
        for (int i = 0; i < 8; ++i)
            out.pts_us = (out.pts_us << 8) | p[5 + i];
        out.payload = buf.substr(CAMERA_FRAME_HEADER_BYTES, len - (CAMERA_FRAME_HEADER_BYTES - 4));
        return static_cast<long>(4 + len);
    }

} // namespace tether
