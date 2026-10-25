#pragma once

#include "tether/bluetooth/messages.hpp"
#include "tether/secret_store.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace tether::bluetooth {

    // Conversations the user removed from Tether. Display only: nothing is
    // deleted, a removed thread just hides every message at or before its
    // cutoff, so anything newer brings it back.
    class RemovedThreads {
    public:
        // `cutoff` is the newest timestamp the user saw in the thread. Repeats
        // keep the later cutoff.
        void remove(const std::string& thread_key, int64_t cutoff);

        bool hides(const Message& message) const;

        size_t size() const { return cutoffs_.size(); }
        const std::map<std::string, int64_t>& cutoffs() const { return cutoffs_; }

    private:
        // Keyed by thread_bucket, so every spelling of a number shares one entry.
        std::map<std::string, int64_t> cutoffs_;
    };

    std::string removed_threads_path(Retention mode);

    // Rewrites the file from one mode's path into the other's and removes the
    // source. No-op unless the source exists and the destination does not.
    bool migrate_removed_threads(Retention from, Retention to);

    bool save_removed_threads(const RemovedThreads& removed);
    // Empty when retention is off, the wallet is locked, or the file is unreadable.
    RemovedThreads load_removed_threads();

    std::string serialize_removed_threads(const RemovedThreads& removed);
    // Anything malformed yields fewer entries, never a broader one.
    RemovedThreads deserialize_removed_threads(const std::string& text);

    // Guarded by message_store_mutex().
    RemovedThreads& removed_threads();

} // namespace tether::bluetooth
