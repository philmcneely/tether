#pragma once

#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/objects.hpp"

#include <chrono>
#include <gio/gio.h>
#include <mutex>
#include <optional>
#include <string>

// Moves the iPhone's audio between this machine and the phone by opening or closing its A2DP stream.
namespace tether::bluetooth {

    // A WirePlumber 0.5 fragment that turns off auto-connect for one device.
    std::string wireplumber_rule(const std::string& address);

    // Writes the rule for `address`, or removes it when empty, and restarts WirePlumber
    // when the file changed. False when the file could not be written.
    bool install_wireplumber_rule(const std::string& address);

    // ConnectProfile or DisconnectProfile on the phone's A2DP source. Blocks for up to 30s.
    bool set_phone_audio_route(GDBusConnection* conn, const std::string& device_path, bool here, std::string& err);

    // "here", "phone", or "unavailable" when there is no Classic link or no sink role.
    std::string phone_audio_route(const BluezObjects& objects, const std::string& address);

    // Applies the remembered choice once per Classic connection, without fighting a
    // later change made on the iPhone.
    class PhoneAudioFollower {
    public:
        void configure(const std::string& address, PhoneAudio choice);
        // Main loop, on every snapshot change.
        void update(const BluezObjects& objects, GDBusConnection* conn);
        // Moves the audio now, if the phone is connected.
        void apply_now(GDBusConnection* conn);

    private:
        // `settle` waits for a stream the iPhone just opened to finish opening first.
        void act(GDBusConnection* conn, bool here, bool settle = false);

        std::mutex mutex_;
        std::string address_;
        PhoneAudio choice_ = PhoneAudio::Auto;
        std::string device_path_;
        std::optional<std::chrono::steady_clock::time_point> link_since_;
        bool acted_ = false;
        // Bumped on every link change, so a delayed action from an old connection is dropped.
        unsigned generation_ = 0;
    };

    extern PhoneAudioFollower* g_phone_audio;

} // namespace tether::bluetooth
