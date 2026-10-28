#include "camera.hpp"
#include "camera_frame.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <sys/socket.h>
#include <tether/log.hpp>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace tether {

    namespace {

        // fd of the stream currently feeding the node, so a new one can end it.
        std::mutex g_active_mutex;
        int g_active_fd = -1;

        // A PipeWire Video/Source that drives the graph with I420 frames as they arrive.
        class PipeWireCamera {
        public:
            PipeWireCamera(const std::string& description, int width, int height, int fps)
                : description_(description), width_(width), height_(height) {
                static std::once_flag init;
                std::call_once(init, [] { pw_init(nullptr, nullptr); });

                events_.version = PW_VERSION_STREAM_EVENTS;
                events_.state_changed = on_state_changed;
                events_.param_changed = on_param_changed;
                events_.process = on_process;

                loop_ = pw_thread_loop_new("tether-camera", nullptr);
                pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE,
                                                         "Video",
                                                         PW_KEY_MEDIA_CATEGORY,
                                                         "Capture",
                                                         PW_KEY_MEDIA_CLASS,
                                                         "Video/Source",
                                                         // the xdg-desktop-portal Camera portal only exposes this role
                                                         PW_KEY_MEDIA_ROLE,
                                                         "Camera",
                                                         PW_KEY_NODE_NAME,
                                                         "tether-camera",
                                                         PW_KEY_NODE_DESCRIPTION,
                                                         description.c_str(),
                                                         nullptr);
                stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "tether-camera", props, &events_, this);

                uint8_t buf[1024];
                spa_pod_builder b{};
                spa_pod_builder_init(&b, buf, sizeof(buf));
                spa_video_info_raw info{};
                info.format = SPA_VIDEO_FORMAT_I420;
                info.size = {uint32_t(width), uint32_t(height)};
                info.framerate = {uint32_t(fps), 1};
                const spa_pod* params[] = {spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

                if (!stream_ || pw_stream_connect(stream_,
                                                  PW_DIRECTION_OUTPUT,
                                                  PW_ID_ANY,
                                                  pw_stream_flags(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS),
                                                  params,
                                                  1) < 0) {
                    debug::log(ERR, "camera: could not create the PipeWire node");
                    return;
                }
                pw_thread_loop_start(loop_);
            }

            ~PipeWireCamera() {
                pw_thread_loop_stop(loop_);
                if (stream_)
                    pw_stream_destroy(stream_);
                pw_thread_loop_destroy(loop_);
            }

            PipeWireCamera(const PipeWireCamera&) = delete;
            PipeWireCamera& operator=(const PipeWireCamera&) = delete;

            bool matches(const AVFrame* f, const std::string& description) const {
                return f->width == width_ && f->height == height_ && description == description_;
            }

            void push(const AVFrame* f) {
                std::vector<uint8_t> i420(size_t(width_) * height_ * 3 / 2);
                uint8_t* out = i420.data();
                for (int plane = 0; plane < 3; ++plane) {
                    const int w = plane ? width_ / 2 : width_;
                    const int h = plane ? height_ / 2 : height_;
                    for (int y = 0; y < h; ++y, out += w)
                        std::memcpy(out, f->data[plane] + size_t(y) * f->linesize[plane], w);
                }
                submit(std::move(i420));
            }

            // Shown between streams so viewers do not keep the last picture.
            void push_black() {
                const size_t luma = size_t(width_) * height_;
                std::vector<uint8_t> i420(luma * 3 / 2, 128);
                std::fill_n(i420.begin(), luma, 16);
                submit(std::move(i420));
            }

        private:
            void submit(std::vector<uint8_t> i420) {
                pw_thread_loop_lock(loop_);
                pending_.swap(i420);
                if (streaming_)
                    pw_stream_trigger_process(stream_);
                pw_thread_loop_unlock(loop_);
            }

            static void on_state_changed(void* data, pw_stream_state, pw_stream_state state, const char* error) {
                auto* self = static_cast<PipeWireCamera*>(data);
                self->streaming_ = state == PW_STREAM_STATE_STREAMING;
                if (state == PW_STREAM_STATE_ERROR)
                    debug::log(ERR, "camera: PipeWire stream error: {}", error ? error : "unknown");
            }

            static void on_param_changed(void* data, uint32_t id, const spa_pod* param) {
                auto* self = static_cast<PipeWireCamera*>(data);
                if (!param || id != SPA_PARAM_Format)
                    return;
                uint8_t buf[1024];
                spa_pod_builder b{};
                spa_pod_builder_init(&b, buf, sizeof(buf));
                const int size = self->width_ * self->height_ * 3 / 2;
                const spa_pod* params[] = {static_cast<const spa_pod*>(spa_pod_builder_add_object(
                    &b,
                    SPA_TYPE_OBJECT_ParamBuffers,
                    SPA_PARAM_Buffers,
                    SPA_PARAM_BUFFERS_buffers,
                    SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
                    SPA_PARAM_BUFFERS_blocks,
                    SPA_POD_Int(1),
                    SPA_PARAM_BUFFERS_size,
                    SPA_POD_Int(size),
                    SPA_PARAM_BUFFERS_stride,
                    SPA_POD_Int(self->width_),
                    SPA_PARAM_BUFFERS_dataType,
                    SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))))};
                pw_stream_update_params(self->stream_, params, 1);
            }

            // Runs on the loop thread with the loop lock held.
            static void on_process(void* data) {
                auto* self = static_cast<PipeWireCamera*>(data);
                if (self->pending_.empty())
                    return;
                pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
                if (!b)
                    return;
                spa_data& d = b->buffer->datas[0];
                if (d.data && d.maxsize >= self->pending_.size()) {
                    std::memcpy(d.data, self->pending_.data(), self->pending_.size());
                    d.chunk->offset = 0;
                    d.chunk->size = uint32_t(self->pending_.size());
                    d.chunk->stride = self->width_;
                }
                self->pending_.clear();
                pw_stream_queue_buffer(self->stream_, b);
            }

            const std::string description_;
            const int width_;
            const int height_;
            pw_thread_loop* loop_ = nullptr;
            pw_stream* stream_ = nullptr;
            pw_stream_events events_{};
            // Guarded by the loop lock.
            bool streaming_ = false;
            std::vector<uint8_t> pending_;
        };

        std::mutex g_camera_mutex;
        PipeWireCamera* g_camera = nullptr;

        struct CodecDeleter {
            void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
            void operator()(AVPacket* p) const { av_packet_free(&p); }
            void operator()(AVFrame* f) const { av_frame_free(&f); }
        };

        void run_stream(int fd, SSL* ssl, std::string buf, std::string description, int fps) {
            // The epoll loop no longer owns this fd; block on it, and give up on a phone that goes quiet.
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
            timeval timeout{10, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            std::unique_ptr<AVCodecContext, CodecDeleter> ctx(
                avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264)));
            std::unique_ptr<AVPacket, CodecDeleter> pkt(av_packet_alloc());
            std::unique_ptr<AVFrame, CodecDeleter> frame(av_frame_alloc());

            if (ctx) {
                ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            }
            if (!ctx || !pkt || !frame || avcodec_open2(ctx.get(), ctx->codec, nullptr) < 0) {
                debug::log(ERR, "camera: no H.264 decoder");
            } else {
                debug::log(INFO, "camera: streaming \"{}\"", description);
                char rbuf[65536];
                for (;;) {
                    CameraFrame f;
                    long n;
                    while ((n = parse_camera_frame(buf, f)) > 0) {
                        if (av_new_packet(pkt.get(), int(f.payload.size())) == 0) {
                            std::memcpy(pkt->data, f.payload.data(), f.payload.size());
                            avcodec_send_packet(ctx.get(), pkt.get());
                            av_packet_unref(pkt.get());
                        }
                        while (avcodec_receive_frame(ctx.get(), frame.get()) == 0) {
                            if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
                                continue;
                            std::lock_guard lock(g_camera_mutex);
                            if (!g_camera || !g_camera->matches(frame.get(), description)) {
                                delete g_camera;
                                g_camera = new PipeWireCamera(description, frame->width, frame->height, fps);
                            }
                            g_camera->push(frame.get());
                        }
                        buf.erase(0, size_t(n));
                    }
                    if (n < 0) {
                        debug::log(ERR, "camera: malformed frame; ending stream");
                        break;
                    }
                    const int r = SSL_read(ssl, rbuf, sizeof(rbuf));
                    if (r <= 0)
                        break;
                    buf.append(rbuf, size_t(r));
                }
                debug::log(INFO, "camera: stream ended");
            }

            bool replaced;
            {
                std::lock_guard lock(g_active_mutex);
                replaced = g_active_fd != fd;
                if (!replaced)
                    g_active_fd = -1;
            }
            if (!replaced) {
                std::lock_guard lock(g_camera_mutex);
                if (g_camera)
                    g_camera->push_black();
            }
            SSL_free(ssl);
            close(fd);
        }

    } // namespace

    void start_camera_stream(
        int fd, SSL* ssl, const nlohmann::json& start, std::string leftover, const std::string& device_name) {
        if (start.value("codec", "h264") != "h264") {
            debug::log(ERR, "camera: unsupported codec {}", start.value("codec", ""));
            SSL_free(ssl);
            close(fd);
            return;
        }
        {
            std::lock_guard lock(g_active_mutex);
            if (g_active_fd >= 0)
                shutdown(g_active_fd, SHUT_RDWR);
            g_active_fd = fd;
        }
        const int fps = std::clamp(start.value("fps", 30), 1, 120);
        std::string description = (device_name.empty() ? std::string("iPhone") : device_name) + " Camera";
        std::thread(run_stream, fd, ssl, std::move(leftover), std::move(description), fps).detach();
    }

} // namespace tether
