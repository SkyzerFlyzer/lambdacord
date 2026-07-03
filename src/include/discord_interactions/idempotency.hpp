#pragma once

// In-process duplicate-delivery guard for async worker Lambdas (AD-9).
//
// AWS asynchronous invocation is at-least-once: the same successful event can
// be delivered more than once, and a failed invocation is retried. This guard
// is a COMPLETION MARKER, not a claim. It records interaction ids only AFTER
// their user-visible effect has landed, so:
//   * a duplicate delivery of an already-completed interaction is skipped, but
//   * a retry of a run that CRASHED before completing is NOT suppressed
//     (nothing was marked), so crash recovery keeps working.
//
// Usage contract (non-negotiable):
//     check -> act -> PATCH -> mark
//   1. if (completed.was_completed(id)) return;   // skip finished work
//   2. do the work and PATCH @original            // the user-visible effect
//   3. completed.mark_completed(id);              // ONLY after the PATCH
//   succeeded
// Never call mark_completed before the user-visible effect has happened: a
// crash between the mark and the PATCH would leave the user on "thinking…"
// forever, because the retry would see the marker and skip.
//
// Scope, stated honestly: this catches duplicates that land on the SAME warm
// container only. Retries minutes later frequently hit a cold container with a
// fresh, empty guard. Durable cross-container dedup is the Phase 5 DynamoDB
// primitives; this header is a zero-dependency best-effort layer in front of
// it. Claim-at-start semantics (drop-on-duplicate for non-idempotent side
// effects) are a separate, explicit opt-in and are NOT what this class does.

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>

namespace discord_interactions {

// Fixed-capacity, in-process LRU set of interaction ids that have COMPLETED.
// Not thread-safe; a Lambda invocation is single-threaded per container.
class CompletedInteractions {
public:
    explicit CompletedInteractions(size_t capacity = 128) : capacity_{capacity} {}

    // Check BEFORE acting. Const and side-effect-free: it never inserts an id
    // and never refreshes recency, so probing an id cannot save it from
    // eviction nor create a phantom completion.
    bool was_completed(const std::string& interaction_id) const {
        return index_.find(interaction_id) != index_.end();
    }

    // Call ONLY AFTER the PATCH succeeded. Inserts the id as most-recently-used,
    // or refreshes its recency if already present, then evicts the true
    // least-recently-used id once capacity is exceeded.
    void mark_completed(const std::string& interaction_id) {
        auto existing = index_.find(interaction_id);
        if (existing != index_.end()) {
            // Refresh recency: move to the front of the LRU order.
            order_.splice(order_.begin(), order_, existing->second);
            return;
        }
        order_.push_front(interaction_id);
        index_.emplace(interaction_id, order_.begin());
        while (index_.size() > capacity_) {
            const std::string& evicted = order_.back();
            index_.erase(evicted);
            order_.pop_back();
        }
    }

private:
    // Front = most recently marked, back = least recently marked (evicted next).
    size_t capacity_{128};
    std::list<std::string> order_{};
    std::unordered_map<std::string, std::list<std::string>::iterator> index_{};
};

}  // namespace discord_interactions
