// context.hpp - conversation context versioning / rollback support.
//
// The agent's "context" is the running list of chat messages sent to the LLM
// (system + user/assistant/tool turns). This module lets the user snapshot
// that context at any point and roll back to a previously saved version,
// discarding everything that came after it (linear, git-checkout-like model).
//
//   context::History history;
//   history.save(messages, "init");          // create version #1
//   ... agent turn ...
//   history.save(messages, "add README");    // create version #2
//   history.rollback(1, messages);           // restore version #1, drop #2
//   history.undo(messages);                  // convenience: one step back
//
#pragma once

#include "llm.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <string>
#include <vector>

namespace context {

struct Snapshot {
    int id = 0;
    std::string label;
    std::string timestamp;                 // human-readable wall-clock time
    std::vector<llm::Message> messages;    // full copy of the context
};

// A linear stack of context snapshots. Rolling back to an earlier version
// truncates the versions that came after it, so the history always describes a
// single line of evolution (no branching). Token-usage accounting is left to
// the caller; only the message context is versioned here.
class History {
public:
    // Save the current messages as a new version. Returns the new version id.
    int save(std::vector<llm::Message> messages, std::string label) {
        Snapshot s;
        s.id = next_id_++;
        s.label = std::move(label);
        s.timestamp = now_string();
        s.messages = std::move(messages);
        current_id_ = s.id;
        snapshots_.push_back(std::move(s));
        return snapshots_.back().id;
    }

    // Roll back to (restore) the snapshot with the given id. All snapshots
    // after it are discarded. Returns false if no such id exists.
    bool rollback(int id, std::vector<llm::Message>& out) {
        auto it = std::find_if(snapshots_.begin(), snapshots_.end(),
                               [&](const Snapshot& s) { return s.id == id; });
        if (it == snapshots_.end()) return false;
        out = it->messages;
        snapshots_.erase(it + 1, snapshots_.end());
        current_id_ = id;
        return true;
    }

    // Convenience: undo the most recent version (restore the previous one).
    bool undo(std::vector<llm::Message>& out) {
        if (snapshots_.size() < 2) return false;
        int prev = snapshots_[snapshots_.size() - 2].id;
        return rollback(prev, out);
    }

    [[nodiscard]] int current_id() const { return current_id_; }
    [[nodiscard]] bool empty() const { return snapshots_.empty(); }
    [[nodiscard]] std::size_t size() const { return snapshots_.size(); }

    // Wipe all history (used by /clear). Version ids restart at 1.
    void clear() {
        snapshots_.clear();
        current_id_ = 0;
        next_id_ = 1;
    }

    // Human-readable listing of all versions; '*' marks the current one.
    [[nodiscard]] std::string describe() const {
        std::string out = "context versions (* = current):\n";
        for (const auto& s : snapshots_) {
            out += std::format("{} #{}  {}  {} msgs  {}\n",
                               (s.id == current_id_) ? "*" : " ",
                               s.id, s.timestamp, s.messages.size(),
                               s.label.empty() ? "(unlabeled)" : s.label);
        }
        return out;
    }

private:
    static std::string now_string() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&t, &tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return std::string(buf);
    }

    std::vector<Snapshot> snapshots_;
    int next_id_ = 1;
    int current_id_ = 0;
};

} // namespace context
