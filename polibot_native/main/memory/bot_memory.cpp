#include "bot_memory.h"

BotMemory g_memory;

void BotMemory::load(const std::string& directory, const Clock& now) {
    seen.load(directory + "/seen.tsv", now);
    mail.load(directory + "/tell.tsv");
    bans.load(directory + "/bans.tsv", now);
    voices.load(directory + "/voices.tsv", now);
}

bool BotMemory::save(const std::string& directory, const Clock& now, int seen_interval, bool force) {
    std::lock_guard<std::mutex> lock(saving_);
    const bool ok_seen = seen.save(directory + "/seen.tsv", now, seen_interval, force);
    const bool ok_mail = mail.save(directory + "/tell.tsv");
    const bool ok_bans = bans.save(directory + "/bans.tsv", now);
    const bool ok_voices = voices.save(directory + "/voices.tsv", now, seen_interval, force);
    return ok_seen && ok_mail && ok_bans && ok_voices;
}
