#include "scoped_env.hpp"

#include <gtest/gtest.h>
#include <tether/bluetooth/messages.hpp>
#include <tether/bluetooth/removed_threads.hpp>
#include <tether/secret_store.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace tether;
using namespace tether::bluetooth;

namespace {

    using tether::testing::ScopedEnv;

    class ScopedHome {
    public:
        explicit ScopedHome(const std::filesystem::path& dir, Retention mode = Retention::Encrypted)
            : home_("HOME", dir) {
            secret::reset_for_test();
            secret::set_retention(mode);
        }

        ~ScopedHome() { secret::reset_for_test(); }

    private:
        ScopedEnv home_;
    };

    std::filesystem::path scratch(const std::string& name) {
        const auto dir = std::filesystem::temp_directory_path() / ("tether-removed-" + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    std::filesystem::path store_dir(const std::filesystem::path& home) { return home / ".local" / "share" / "tether"; }

    Message make(const std::string& handle, const std::string& thread, int64_t ts, bool read = false) {
        Message m;
        m.handle = handle;
        m.thread_key = thread;
        m.peer_address = thread;
        m.body = "body " + handle;
        m.timestamp = ts;
        m.read = read;
        return m;
    }

    MessageStore::Visible visible_with(const RemovedThreads& removed) {
        return [&removed](const Message& m) { return !removed.hides(m); };
    }

} // namespace

TEST(RemovedThreads, HidesUpToAndIncludingTheCutoff) {
    RemovedThreads removed;
    removed.remove("tel:+15551234567", 100);

    EXPECT_TRUE(removed.hides(make("a", "tel:+15551234567", 99)));
    EXPECT_TRUE(removed.hides(make("b", "tel:+15551234567", 100)));
    EXPECT_FALSE(removed.hides(make("c", "tel:+15551234567", 101))) << "a newer message must bring the thread back";
    EXPECT_FALSE(removed.hides(make("d", "tel:+15559999999", 50))) << "other threads are untouched";
}

TEST(RemovedThreads, NothingRemovedHidesNothing) {
    RemovedThreads removed;
    EXPECT_FALSE(removed.hides(make("a", "tel:+15551234567", 0)));
    removed.remove("", 100);
    EXPECT_EQ(removed.size(), 0u);
    EXPECT_FALSE(removed.hides(make("b", "", 0)));
}

// The phone lists the same person both ways, and the store merges them.
TEST(RemovedThreads, EverySpellingOfANumberSharesOneEntry) {
    RemovedThreads removed;
    removed.remove("tel:5551234567", 100);
    removed.remove("tel:+15551234567", 50);

    EXPECT_EQ(removed.size(), 1u);
    EXPECT_TRUE(removed.hides(make("a", "tel:+15551234567", 100))) << "repeats keep the later cutoff";
    EXPECT_TRUE(removed.hides(make("b", "tel:5551234567", 100)));
}

TEST(RemovedThreads, GroupAndEmailKeysAreTheirOwnEntry) {
    RemovedThreads removed;
    removed.remove("group:abc", 100);
    removed.remove("email:someone@example.com", 100);

    EXPECT_TRUE(removed.hides(make("a", "group:abc", 100)));
    EXPECT_TRUE(removed.hides(make("b", "email:someone@example.com", 100)));
    EXPECT_FALSE(removed.hides(make("c", "group:abd", 100)));
}

TEST(RemovedThreads, ThreadsLeaveOutHiddenMessagesBeforeCounting) {
    MessageStore store;
    store.add(make("old1", "tel:+15551234567", 100));
    store.add(make("old2", "tel:+15551234567", 200));
    store.add(make("other", "tel:+15559999999", 150));

    RemovedThreads removed;
    removed.remove("tel:+15551234567", 200);
    const auto visible = visible_with(removed);

    auto threads = store.threads(visible);
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_EQ(threads[0].key, "tel:+15559999999");

    store.add(make("new", "tel:+15551234567", 300));
    threads = store.threads(visible);
    ASSERT_EQ(threads.size(), 2u);
    EXPECT_EQ(threads[0].key, "tel:+15551234567");
    EXPECT_EQ(threads[0].count, 1) << "the returning thread counts only what is shown";
    EXPECT_EQ(threads[0].unread, 1) << "hidden unread messages must not reach the tray badge";
    EXPECT_EQ(threads[0].last_body, "body new");
    EXPECT_EQ(threads[0].last_timestamp, 300);

    auto messages = store.messages("tel:+15551234567", 200, visible);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].handle, "new");

    EXPECT_EQ(store.threads().size(), 2u);
    EXPECT_EQ(store.messages("tel:+15551234567").size(), 3u) << "nothing is deleted";
}

TEST(RemovedThreads, ReplyStateComesFromWhatIsShown) {
    MessageStore store;
    Message reply = make("sent", "tel:+15551234567", 100, true);
    reply.outgoing = true;
    store.add(reply);
    store.add(make("new", "tel:+15551234567", 300));

    RemovedThreads removed;
    removed.remove("tel:+15551234567", 100);

    auto threads = store.threads(visible_with(removed));
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_FALSE(threads[0].replied);
}

TEST(RemovedThreads, MessagesFilterBeforeTheLimit) {
    MessageStore store;
    for (int i = 0; i < 10; ++i)
        store.add(make("h" + std::to_string(i), "tel:+15551234567", 100 + i));

    RemovedThreads removed;
    removed.remove("tel:+15551234567", 107);

    auto messages = store.messages("tel:+15551234567", 5, visible_with(removed));
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].handle, "h8");
    EXPECT_EQ(messages[1].handle, "h9");
}

TEST(RemovedThreads, StaysHiddenAfterTheStoreRekeysTheThread) {
    MessageStore store;
    store.add(make("a", "tel:5551234567", 100));

    RemovedThreads removed;
    removed.remove("tel:5551234567", 100);

    // A better spelling arrives and the store moves the conversation to it.
    store.add(make("b", "tel:+15551234567", 90));
    ASSERT_EQ(store.threads().size(), 1u);
    EXPECT_EQ(store.threads()[0].key, "tel:+15551234567");

    EXPECT_TRUE(store.threads(visible_with(removed)).empty());
}

TEST(RemovedThreads, RoundTripsEncryptedWithNothingInTheClear) {
    const auto home = scratch("encrypted");
    {
        ScopedHome scoped(home);
        RemovedThreads removed;
        removed.remove("tel:+15551234567", 100);
        ASSERT_TRUE(save_removed_threads(removed));

        const auto path = store_dir(home) / "removed_threads.json.enc";
        ASSERT_TRUE(std::filesystem::exists(path));
        std::ifstream in(path);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("5551234567"), std::string::npos) << "the phone number is on disk in the clear";
        EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all,
                  std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

        auto loaded = load_removed_threads();
        EXPECT_EQ(loaded.size(), 1u);
        EXPECT_TRUE(loaded.hides(make("a", "tel:+15551234567", 100)));
        EXPECT_FALSE(loaded.hides(make("b", "tel:+15551234567", 101)));
    }
    std::filesystem::remove_all(home);
}

TEST(RemovedThreads, ACorruptFileHidesNothing) {
    const auto home = scratch("corrupt");
    {
        ScopedHome scoped(home);
        std::filesystem::create_directories(store_dir(home));
        std::ofstream(store_dir(home) / "removed_threads.json.enc") << "not a sealed record";
        EXPECT_EQ(load_removed_threads().size(), 0u);
    }
    std::filesystem::remove_all(home);
}

TEST(RemovedThreads, MalformedEntriesAreSkipped) {
    const auto home = scratch("malformed");
    {
        ScopedHome scoped(home, Retention::Plaintext);
        auto removed = deserialize_removed_threads(
            R"({"removed": {"tel:5551234567": 100, "tel:5559999999": "100", "tel:5558888888": 1.5}})");
        EXPECT_EQ(removed.size(), 1u);
        EXPECT_TRUE(removed.hides(make("a", "tel:+15551234567", 100)));

        EXPECT_EQ(deserialize_removed_threads(R"({"removed": []})").size(), 0u);
        EXPECT_EQ(deserialize_removed_threads(R"([1, 2])").size(), 0u);
        EXPECT_EQ(deserialize_removed_threads("{").size(), 0u);
    }
    std::filesystem::remove_all(home);
}

TEST(RemovedThreads, RetentionNoneWritesNothing) {
    const auto home = scratch("none");
    {
        ScopedHome scoped(home, Retention::None);
        RemovedThreads removed;
        removed.remove("tel:+15551234567", 100);
        EXPECT_FALSE(save_removed_threads(removed));
        EXPECT_FALSE(std::filesystem::exists(store_dir(home) / "removed_threads.json"));
        EXPECT_FALSE(std::filesystem::exists(store_dir(home) / "removed_threads.json.enc"));
    }
    std::filesystem::remove_all(home);
}

TEST(RemovedThreads, MigratesToTheOtherModeOnLoad) {
    const auto home = scratch("migrate");
    {
        ScopedHome scoped(home, Retention::Plaintext);
        RemovedThreads removed;
        removed.remove("tel:+15551234567", 100);
        ASSERT_TRUE(save_removed_threads(removed));
        ASSERT_TRUE(std::filesystem::exists(store_dir(home) / "removed_threads.json"));
    }
    {
        ScopedHome scoped(home);
        auto loaded = load_removed_threads();
        EXPECT_TRUE(loaded.hides(make("a", "tel:+15551234567", 100))) << "migration lost the entry";
        EXPECT_FALSE(std::filesystem::exists(store_dir(home) / "removed_threads.json"));
        EXPECT_TRUE(std::filesystem::exists(store_dir(home) / "removed_threads.json.enc"));
    }
    std::filesystem::remove_all(home);
}
