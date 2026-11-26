#include "voice_memory.h"
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include "record_file.h"

// 64-bit FNV-1a over "network channel who", ignoring case
static uint64_t key_of(const std::string& network, const std::string& channel, const char* kind, const std::string& who) {
    uint64_t hash = 14695981039346656037ull;
    auto add = [&](const std::string& part) {
        for (unsigned char c : part) hash = (hash ^ (unsigned char)std::tolower(c)) * 1099511628211ull;
        hash = (hash ^ 0xff) * 1099511628211ull; // so that "ab"+"c" differs from "a"+"bc"
    };
    add(network);
    add(channel);
    add(kind);
    add(who);
    return hash;
}

VoiceMemory::Record* VoiceMemory::find_locked(uint64_t key) {
    for (auto& r : records_) {
        if (r.key == key) return &r;
    }
    return nullptr;
}

VoiceState VoiceMemory::find(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Record* by_host = find_locked(key_of(network, channel, "h", userhost));
    const Record* by_account = account.empty() ? nullptr : find_locked(key_of(network, channel, "a", account));
    if ((by_host && by_host->revoked) || (by_account && by_account->revoked)) return VoiceState::revoked;
    return by_host || by_account ? VoiceState::earned : VoiceState::unknown;
}

// Moves the entry to the recent end; a full list drops the oldest earned one first, since forgetting
// a loss of voice would hand it back
void VoiceMemory::put_locked(uint64_t key, bool revoked, const Clock& now, size_t max_entries) {
    if (Record* old = find_locked(key)) {
        if (old->revoked != revoked) urgent_ = urgent_ || revoked;
        records_.erase(records_.begin() + (old - records_.data()));
    } else {
        urgent_ = urgent_ || revoked;
    }
    while (!records_.empty() && records_.size() >= max_entries) {
        auto it = std::find_if(records_.begin(), records_.end(), [](const Record& r) { return !r.revoked; });
        records_.erase(it != records_.end() ? it : records_.begin());
    }
    if (records_.capacity() < max_entries) records_.reserve(max_entries);
    records_.push_back({key, (uint32_t)now.epoch, revoked});
    changed_ = true;
}

void VoiceMemory::earn(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account,
                       const Clock& now, size_t max_entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    put_locked(account.empty() ? key_of(network, channel, "h", userhost) : key_of(network, channel, "a", account), false, now, max_entries);
}

void VoiceMemory::revoke(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account,
                         const Clock& now, size_t max_entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    put_locked(key_of(network, channel, "h", userhost), true, now, max_entries);
    if (!account.empty()) put_locked(key_of(network, channel, "a", account), true, now, max_entries);
}

void VoiceMemory::restore(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account) {
    const uint64_t host_key = key_of(network, channel, "h", userhost);
    const uint64_t account_key = account.empty() ? host_key : key_of(network, channel, "a", account);
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const Record& r) {
        return r.revoked && (r.key == host_key || r.key == account_key);
    }), records_.end());
    if (records_.size() != before) changed_ = urgent_ = true;
}

void VoiceMemory::forget_older_than(const Clock& now, int days) {
    if (!now.epoch) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const Record& r) {
        return r.when && now.epoch - (int64_t)r.when > (int64_t)days * 86400;
    }), records_.end());
    if (records_.size() != before) changed_ = true;
}

size_t VoiceMemory::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

// One line per entry: key in hexadecimal, when, "e" earned or "r" revoked
void VoiceMemory::load(const std::string& path, const Clock& now) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    record::read_lines(path, [&](const std::vector<std::string>& f) {
        if (f.size() != 3 || f[2].size() != 1) return;
        records_.push_back({strtoull(f[0].c_str(), nullptr, 16), (uint32_t)record::number(f[1]), f[2][0] == 'r'});
    });
    changed_ = urgent_ = false;
    saved_at_ = now.steady;
}

bool VoiceMemory::save(const std::string& path, const Clock& now, int interval, bool force) {
    PsramVector<Record> copy; // written without the lock, so IRC never waits for the flash
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!changed_ || (!force && !urgent_ && now.steady - saved_at_ < interval)) return true;
        copy = records_;
        changed_ = urgent_ = false;
        saved_at_ = now.steady;
    }
    const bool ok = record::write_file(path, [&](FILE* f) {
        for (const auto& r : copy) {
            if (fprintf(f, "%016" PRIx64 "\t%u\t%c\n", r.key, (unsigned)r.when, r.revoked ? 'r' : 'e') < 0) return false;
        }
        return true;
    });
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        changed_ = urgent_ = true; // try again next time
    }
    return ok;
}
