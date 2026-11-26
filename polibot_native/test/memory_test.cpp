// Runs on a PC, from the project folder:
// c++ -std=c++17 -I main/memory -I main/logic test/memory_test.cpp main/memory/*.cpp -o /tmp/memory_test && /tmp/memory_test
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "bot_memory.h"

int main() {
    char folder[] = "/tmp/polibot-memory-XXXXXX";
    assert(mkdtemp(folder));
    const std::string dir = folder;
    const Clock now = {1000, 1800000000};
    const Clock unsynced = {1000, 0};

    // seen: one entry per person and network, the quietest forgotten first, survives a restart
    {
        BotMemory m;
        m.load(dir, now); // as at start up: the interval for "seen" counts from here
        m.seen.note("net", "Alice", "a@h", 'm', "#c", "hello\tthere\n", now, 2);
        m.seen.note("net", "bob", "b@h", 'j', "#c", "", now, 2);
        m.seen.note("net", "ALICE", "", 'q', "#c", "bye", now, 2); // keeps the known host
        m.seen.note("net", "carol", "c@h", 'j', "#c", std::string(200, 'x'), now, 2);
        SeenRecord r;
        assert(!m.seen.find("net", "bob", r) && !m.seen.find("other", "alice", r));
        assert(m.seen.find("net", "alice", r) && r.action == 'q' && std::string(r.userhost) == "a@h" && std::string(r.text) == "bye");
        assert(m.seen.find("net", "carol", r) && std::string(r.text).size() == sizeof(r.text) - 1);
        assert(m.save(dir, now, 600, false)); // too early for "seen"
        BotMemory early;
        early.load(dir, now);
        assert(!early.seen.find("net", "alice", r));
        assert(m.save(dir, now, 600, true));
        BotMemory again;
        again.load(dir, now);
        assert(again.seen.find("net", "alice", r) && r.when == now.epoch && std::string(r.text) == "bye");
        again.seen.forget_older_than({1000, now.epoch + 3 * 86400}, 2);
        assert(!again.seen.find("net", "alice", r));
    }
    // tell: limits per recipient, per sender and in total; delivered once
    {
        BotMemory m;
        assert(m.mail.add("net", "bob", "al", "one", now, 2, 3, 4) == TellResult::stored);
        assert(m.mail.add("net", "BOB", "cy", "two", now, 2, 3, 4) == TellResult::stored);
        assert(m.mail.add("net", "bob", "di", "three", now, 2, 3, 4) == TellResult::mailbox_full);
        assert(m.mail.add("net", "ed", "al", "x", now, 2, 1, 4) == TellResult::sender_full);
        assert(m.mail.add("net", "ed", "al", "a:b c\tx", now, 2, 3, 4) == TellResult::stored);
        assert(m.mail.add("net", "fy", "al", "x", now, 2, 3, 4) == TellResult::stored);
        assert(m.mail.add("net", "gus", "hal", "x", now, 2, 3, 4) == TellResult::full);
        assert(m.save(dir, now, 600, false)); // messages are written at once
        BotMemory again;
        again.load(dir, now);
        auto waiting = again.mail.take("net", "Bob");
        assert(waiting.size() == 2 && std::string(waiting[0].text) == "one" && again.mail.take("net", "bob").empty());
        assert(std::string(again.mail.take("net", "ed")[0].text) == "a:b c x");
    }
    // bans: expiry, lookup by nickname, and the end time across restarts with and without a clock
    {
        BotMemory m;
        m.bans.add("net", "#c", "+b *!*@h", "troll", 600, now);
        m.bans.add("net", "#c", "+q *!*@h", "troll", 0, now);
        m.bans.add("net", "#c", "+b *!*@h", "troll", 60, now); // replaces the first
        assert(m.bans.for_nick("net", "#C", "TROLL").size() == 2);
        assert(m.bans.expired("net", {1059, 0}).empty() && m.bans.expired("net", {1060, 0}).size() == 1);
        assert(m.save(dir, now, 600, false));
        BotMemory later; // restarted 30 s later, clock known
        later.load(dir, {5, now.epoch + 30});
        assert(later.bans.expired("net", {34, now.epoch + 59}).empty() && later.bans.expired("net", {35, now.epoch + 60}).size() == 1);
        BotMemory blind; // restarted without a clock: waits for it
        blind.load(dir, {5, 0});
        assert(blind.bans.expired("net", {100000, 0}).empty());
        assert(blind.bans.expired("net", {100000, now.epoch + 61}).size() == 1);
        BotMemory dark; // saved without ever knowing the time: the remaining time is kept
        dark.bans.add("net", "#c", "+b x!*@*", "x", 120, unsynced);
        assert(dark.save(dir, {1060, 0}, 600, false));
        BotMemory woken;
        woken.load(dir, {10, 0});
        assert(woken.bans.expired("net", {69, 0}).empty() && woken.bans.expired("net", {70, 0}).size() == 1);
        assert(woken.bans.remove("net", "#c", "+b x!*@*") && !woken.bans.remove("net", "#c", "+b x!*@*"));
        for (size_t i = 0; i < BanList::MAX + 5; i++) woken.bans.add("net", "#c", "+b n" + std::to_string(i), "n", 0, now);
        assert(woken.bans.for_nick("net", "#c", "n").size() == BanList::MAX);
    }
    // timed voice: earned by account or host, a loss outweighs it, kept small and across restarts
    {
        BotMemory m;
        m.load(dir, now);
        auto state = [&](BotMemory& b, const char* userhost, const char* account) { return b.voices.find("net", "#c", userhost, account); };
        assert(state(m, "a@h", "") == VoiceState::unknown);
        m.voices.earn("net", "#c", "a@h", "", now, 10);
        m.voices.earn("NET", "#C", "b@h", "Bob", now, 10);
        assert(state(m, "A@H", "") == VoiceState::earned && state(m, "elsewhere@x", "bob") == VoiceState::earned);
        assert(state(m, "b@h", "") == VoiceState::unknown); // signed in, so kept by account
        assert(m.voices.find("net", "#other", "a@h", "") == VoiceState::unknown && m.voices.find("two", "#c", "a@h", "") == VoiceState::unknown);
        assert(m.save(dir, now, 600, false)); // earned: not written before its time
        BotMemory early;
        early.load(dir, now);
        assert(state(early, "a@h", "") == VoiceState::unknown);
        m.voices.revoke("net", "#c", "b@h", "bob", now, 10); // lost: both names, written at once
        assert(state(m, "b@h", "") == VoiceState::revoked && state(m, "other@y", "bob") == VoiceState::revoked);
        assert(m.save(dir, now, 600, false));
        BotMemory again;
        again.load(dir, now);
        assert(state(again, "a@h", "") == VoiceState::earned && state(again, "b@h", "bob") == VoiceState::revoked);
        again.voices.restore("net", "#c", "a@h", ""); // being voiced does not undo what was earned
        again.voices.restore("net", "#c", "b@h", "bob");
        assert(state(again, "a@h", "") == VoiceState::earned && state(again, "b@h", "bob") == VoiceState::unknown);
        // full: the oldest earned entry goes first, a loss of voice stays
        BotMemory full;
        full.voices.revoke("net", "#c", "bad@h", "", now, 3);
        for (int i = 0; i < 10; i++) full.voices.earn("net", "#c", "u" + std::to_string(i) + "@h", "", now, 3);
        assert(full.voices.size() == 3 && state(full, "bad@h", "") == VoiceState::revoked);
        assert(state(full, "u9@h", "") == VoiceState::earned && state(full, "u7@h", "") == VoiceState::unknown);
        full.voices.earn("net", "#c", "u9@h", "", now, 2); // the limit was lowered
        assert(full.voices.size() == 2);
        again.voices.forget_older_than({0, now.epoch + 3 * 86400}, 2);
        assert(again.voices.size() == 0);
    }
    const std::string cleanup = "rm -rf " + dir;
    assert(system(cleanup.c_str()) == 0);
    std::puts("memory: all checks passed");
}
