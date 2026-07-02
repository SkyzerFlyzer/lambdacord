#include <doctest/doctest.h>

#include <string>

#include "discord_interactions/idempotency.hpp"

using discord_interactions::CompletedInteractions;

TEST_CASE("unmarked id reports not completed") {
    CompletedInteractions completed{};
    CHECK(completed.was_completed("abc") == false);
}

TEST_CASE("marked id reports completed") {
    CompletedInteractions completed{};
    completed.mark_completed("abc");
    CHECK(completed.was_completed("abc") == true);
}

TEST_CASE("was_completed is a pure check with no side effect") {
    // Checking an id must never insert it: repeated checks on an unmarked id
    // stay false, and probing an id must not consume capacity or create a
    // phantom entry that later reads as completed.
    CompletedInteractions completed{2};
    CHECK(completed.was_completed("ghost") == false);
    CHECK(completed.was_completed("ghost") == false);
    CHECK(completed.was_completed("ghost") == false);

    // Fill capacity with two real completions. If the checks above had
    // inserted "ghost", it would now occupy a slot / evict a real one.
    completed.mark_completed("a");
    completed.mark_completed("b");
    CHECK(completed.was_completed("a") == true);
    CHECK(completed.was_completed("b") == true);
    CHECK(completed.was_completed("ghost") == false);
}

TEST_CASE("eviction at capacity drops the least-recently-marked id") {
    CompletedInteractions completed{2};
    completed.mark_completed("a");
    completed.mark_completed("b");
    completed.mark_completed("c");  // evicts "a" (oldest)

    CHECK(completed.was_completed("a") == false);
    CHECK(completed.was_completed("b") == true);
    CHECK(completed.was_completed("c") == true);
}

TEST_CASE("re-marking refreshes recency (true LRU, not FIFO)") {
    CompletedInteractions completed{2};
    completed.mark_completed("a");
    completed.mark_completed("b");
    completed.mark_completed("a");  // refresh "a" -> "b" is now least recent
    completed.mark_completed("c");  // must evict "b", not "a"

    CHECK(completed.was_completed("a") == true);
    CHECK(completed.was_completed("b") == false);
    CHECK(completed.was_completed("c") == true);
}

TEST_CASE("was_completed does not refresh recency") {
    // Only mark_completed touches recency; a const check must not save an id
    // from eviction.
    CompletedInteractions completed{2};
    completed.mark_completed("a");
    completed.mark_completed("b");
    (void)completed.was_completed("a");  // no side effect -> "a" still oldest
    completed.mark_completed("c");       // evicts "a"

    CHECK(completed.was_completed("a") == false);
    CHECK(completed.was_completed("b") == true);
    CHECK(completed.was_completed("c") == true);
}

TEST_CASE("capacity 1 keeps only the most recent id") {
    CompletedInteractions completed{1};
    completed.mark_completed("a");
    CHECK(completed.was_completed("a") == true);
    completed.mark_completed("b");  // evicts "a"
    CHECK(completed.was_completed("a") == false);
    CHECK(completed.was_completed("b") == true);
}

TEST_CASE("re-marking an existing id does not grow past capacity") {
    CompletedInteractions completed{2};
    completed.mark_completed("a");
    completed.mark_completed("a");  // same id: refresh, not a second slot
    completed.mark_completed("b");
    // Both distinct ids fit; nothing evicted because there are only two.
    CHECK(completed.was_completed("a") == true);
    CHECK(completed.was_completed("b") == true);
}

TEST_CASE("empty id is treated like any other key") {
    CompletedInteractions completed{2};
    CHECK(completed.was_completed("") == false);
    completed.mark_completed("");
    CHECK(completed.was_completed("") == true);
    CHECK(completed.was_completed("other") == false);

    // The empty id participates in LRU like a normal key.
    completed.mark_completed("x");
    completed.mark_completed("y");  // evicts "" (oldest of "", "x")
    CHECK(completed.was_completed("") == false);
    CHECK(completed.was_completed("x") == true);
    CHECK(completed.was_completed("y") == true);
}

TEST_CASE("default capacity is 128") {
    CompletedInteractions completed{};  // default 128
    for (int i{}; i < 128; ++i) {
        completed.mark_completed("id-" + std::to_string(i));
    }
    // All 128 still present at exactly capacity.
    CHECK(completed.was_completed("id-0") == true);
    CHECK(completed.was_completed("id-127") == true);
    // One more evicts the oldest.
    completed.mark_completed("id-128");
    CHECK(completed.was_completed("id-0") == false);
    CHECK(completed.was_completed("id-128") == true);
}
