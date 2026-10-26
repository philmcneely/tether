#include "tether/bluetooth/phone_audio.hpp"

#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"
#include "tether/service.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace tether::bluetooth {

    PhoneAudioFollower* g_phone_audio = nullptr;

    namespace {

        constexpr int PROFILE_TIMEOUT_MS = 30000;
        // iOS opens the stream itself about a second after the link comes up
        constexpr auto PHONE_WINDOW = std::chrono::seconds(30);
        // Long enough for iOS to open the stream first, so Here does not race it.
        constexpr auto HERE_DELAY = std::chrono::seconds(5);
        constexpr auto BUSY_RETRY = std::chrono::seconds(2);
        constexpr int BUSY_ATTEMPTS = 5;
        constexpr auto OPEN_SETTLE = std::chrono::seconds(3);

        std::filesystem::path rule_path() {
            std::filesystem::path base;
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            if (xdg && xdg[0] == '/')
                base = xdg;
            else if (const char* home = std::getenv("HOME"); home && home[0])
                base = std::filesystem::path(home) / ".config";
            else
                return {};
            // zz- sorts last, so this rule wins over the user's own for the same device.
            return base / "wireplumber" / "wireplumber.conf.d" / "zz-tether-phone-audio.conf";
        }

        std::string read_file(const std::filesystem::path& path) {
            std::ifstream in(path);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        void restart_wireplumber() {
            if (!systemd_booted()) {
                debug::log(WARN, "phone audio: restart WirePlumber to load {}", rule_path().string());
                return;
            }
            gint status = 0;
            if (!g_spawn_command_line_sync(
                    "timeout 10 systemctl --user try-restart wireplumber", nullptr, nullptr, &status, nullptr) ||
                status != 0)
                debug::log(WARN, "phone audio: could not restart WirePlumber");
        }

        const Device* find_phone(const BluezObjects& objects, const std::string& address) {
            const Device* found = nullptr;
            for (const auto& device : objects.devices) {
                if (device.address != address)
                    continue;
                if (device.classic_link_up())
                    return &device;
                found = found ? found : &device;
            }
            return found;
        }

        bool offers_sink(const BluezObjects& objects, const Device& device) {
            const Adapter* adapter = objects.find_adapter(device.adapter_path);
            return adapter && std::any_of(adapter->uuids.begin(), adapter->uuids.end(), [](const std::string& uuid) {
                       return g_ascii_strcasecmp(uuid.c_str(), UUID_A2DP_SINK) == 0;
                   });
        }

    } // namespace

    std::string wireplumber_rule(const std::string& address) {
        return "# Written by Tether for tether --bt-phone-audio; 'auto' removes it.\n"
               "monitor.bluez.rules = [\n"
               "  {\n"
               "    matches = [ { api.bluez5.address = \"" +
               address +
               "\" } ]\n"
               "    actions = { update-props = { bluez5.auto-connect = [ ] } }\n"
               "  }\n"
               "]\n";
    }

    bool install_wireplumber_rule(const std::string& address) {
        const auto path = rule_path();
        if (path.empty())
            return false;
        std::error_code ec;
        if (address.empty()) {
            if (!std::filesystem::remove(path, ec))
                return !ec;
            restart_wireplumber();
            return true;
        }

        // Interpolated into WirePlumber's config, so nothing but an address gets in.
        const bool valid = address.size() == 17 && std::all_of(address.begin(), address.end(), [](char c) {
                               return std::isxdigit(static_cast<unsigned char>(c)) || c == ':';
                           });
        if (!valid)
            return false;
        const std::string text = wireplumber_rule(address);
        if (read_file(path) == text)
            return true;
        std::filesystem::create_directories(path.parent_path(), ec);
        const auto temp = path.string() + ".tmp";
        {
            std::ofstream out(temp, std::ios::trunc);
            out << text;
            if (!out.flush())
                return false;
        }
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        restart_wireplumber();
        return true;
    }

    bool set_phone_audio_route(GDBusConnection* conn, const std::string& device_path, bool here, std::string& err) {
        if (!conn || device_path.empty()) {
            err = "no phone";
            return false;
        }
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      "org.bluez",
                                                      device_path.c_str(),
                                                      "org.bluez.Device1",
                                                      here ? "ConnectProfile" : "DisconnectProfile",
                                                      g_variant_new("(s)", UUID_A2DP_SOURCE),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      PROFILE_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            err = error ? error->message : "unknown error";
            g_clear_error(&error);
            return false;
        }
        g_variant_unref(reply);
        return true;
    }

    std::string phone_audio_route(const BluezObjects& objects, const std::string& address) {
        const Device* device = find_phone(objects, address);
        if (!device || !device->classic_link_up() || !offers_sink(objects, *device))
            return "unavailable";
        return objects.find_sink_transport(device->path) ? "here" : "phone";
    }

    void PhoneAudioFollower::configure(const std::string& address, PhoneAudio choice) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (address != address_) {
            link_since_.reset();
            acted_ = false;
            ++generation_;
        }
        address_ = address;
        choice_ = choice;
    }

    void PhoneAudioFollower::update(const BluezObjects& objects, GDBusConnection* conn) {
        std::unique_lock<std::mutex> lock(mutex_);
        const Device* device = address_.empty() ? nullptr : find_phone(objects, address_);
        if (!device || !device->classic_link_up() || !offers_sink(objects, *device)) {
            if (link_since_)
                ++generation_;
            link_since_.reset();
            acted_ = false;
            return;
        }
        device_path_ = device->path;
        const auto now = std::chrono::steady_clock::now();
        if (!link_since_) {
            link_since_ = now;
            // Nothing else may change in the snapshot once the link settles.
            if (choice_ == PhoneAudio::Here) {
                std::thread([this, conn] {
                    std::this_thread::sleep_for(HERE_DELAY);
                    if (g_bluez)
                        update(g_bluez->snapshot(), conn);
                }).detach();
            }
        }
        if (acted_ || choice_ == PhoneAudio::Auto)
            return;

        const bool open = objects.find_sink_transport(device->path) != nullptr;
        const auto elapsed = now - *link_since_;
        if (choice_ == PhoneAudio::Phone) {
            if (open && elapsed < PHONE_WINDOW) {
                acted_ = true;
                lock.unlock();
                act(conn, false, true);
            } else if (elapsed >= PHONE_WINDOW) {
                acted_ = true;
            }
            return;
        }
        if (open) {
            acted_ = true;
        } else if (elapsed >= HERE_DELAY) {
            acted_ = true;
            lock.unlock();
            act(conn, true);
        }
    }

    void PhoneAudioFollower::apply_now(GDBusConnection* conn) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!link_since_ || choice_ == PhoneAudio::Auto)
            return;
        acted_ = true;
        const bool here = choice_ == PhoneAudio::Here;
        lock.unlock();
        act(conn, here);
    }

    void PhoneAudioFollower::act(GDBusConnection* conn, bool here, bool settle) {
        std::string path;
        unsigned generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path = device_path_;
            generation = generation_;
        }
        std::thread([this, conn, path, generation, here, settle] {
            if (settle)
                std::this_thread::sleep_for(OPEN_SETTLE);
            for (int attempt = 1;; ++attempt) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (generation != generation_)
                        return;
                }
                std::string err;
                if (set_phone_audio_route(conn, path, here, err)) {
                    debug::log(INFO, "phone audio: {}", here ? "playing here" : "back on the iPhone");
                    return;
                }
                // bluetoothd refuses while the link is still settling.
                if (err.find("InProgress") == std::string::npos || attempt == BUSY_ATTEMPTS) {
                    debug::log(WARN, "phone audio: {} failed: {}", here ? "ConnectProfile" : "DisconnectProfile", err);
                    return;
                }
                std::this_thread::sleep_for(BUSY_RETRY);
            }
        }).detach();
    }

} // namespace tether::bluetooth
