#include "../src/daemon/camera_frame.hpp"
#include <gtest/gtest.h>
#include <string>

using tether::CameraFrame;
using tether::parse_camera_frame;

static std::string frame(uint32_t len, uint8_t flags, uint64_t pts, const std::string& payload) {
    std::string s;
    for (int i = 3; i >= 0; --i)
        s += char(len >> (i * 8));
    s += char(flags);
    for (int i = 7; i >= 0; --i)
        s += char(pts >> (i * 8));
    return s + payload;
}

TEST(CameraFrameTest, ParsesWholeFrame) {
    const std::string buf = frame(9 + 3, 1, 0x0102030405060708ull, "abc") + "next";
    CameraFrame f;
    EXPECT_EQ(parse_camera_frame(buf, f), 13 + 3);
    EXPECT_TRUE(f.keyframe);
    EXPECT_EQ(f.pts_us, 0x0102030405060708ull);
    EXPECT_EQ(f.payload, "abc");
}

TEST(CameraFrameTest, WaitsForMoreBytes) {
    const std::string buf = frame(9 + 3, 0, 7, "abc");
    CameraFrame f;
    for (size_t n = 0; n < buf.size(); ++n)
        EXPECT_EQ(parse_camera_frame(std::string_view(buf).substr(0, n), f), 0) << n;
    EXPECT_EQ(parse_camera_frame(buf, f), long(buf.size()));
    EXPECT_FALSE(f.keyframe);
}

TEST(CameraFrameTest, RejectsBadLengths) {
    CameraFrame f;
    EXPECT_EQ(parse_camera_frame(frame(8, 0, 0, ""), f), -1);
    EXPECT_EQ(parse_camera_frame(frame(tether::CAMERA_FRAME_MAX_BYTES + 1, 0, 0, ""), f), -1);
}
