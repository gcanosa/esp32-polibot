#include "ban_list.h"
#include <algorithm>
#include "record_file.h"

void BanList::add(const std::string& network, const std::string& channel, const std::string& mode, const std::string& nick,
                  int seconds, const Clock& now) {
    std::lock_guard<std::mutex> lock(mutex_);
    BanRecord r = {};
    record::set(r.network, network);
    record::set(r.channel, channel);
    record::set(r.mode, mode);
    record::set(r.nick, nick);
    r.deadline = seconds > 0 ? now.steady + seconds : 0;
    changed_ = true;
    for (auto& old : records_) {
        if (record::is(old.network, network) && record::is(old.channel, channel) && record::is(old.mode, mode)) {
            old = r;
            return;
        }
    }
    if (records_.size() >= MAX) {
        // make room by forgetting the oldest permanent one (it stays set on the channel)
        auto it = std::find_if(records_.begin(), records_.end(), [](const BanRecord& b) { return b.deadline == 0; });
        records_.erase(it != records_.end() ? it : records_.begin());
    }
    records_.push_back(r);
}

bool BanList::remove(const std::string& network, const std::string& channel, const std::string& mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (record::is(it->network, network) && record::is(it->channel, channel) && record::is(it->mode, mode)) {
            records_.erase(it);
            changed_ = true;
            return true;
        }
    }
    return false;
}

std::vector<std::string> BanList::for_nick(const std::string& network, const std::string& channel, const std::string& nick) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> modes;
    for (const auto& r : records_) {
        if (r.nick[0] && record::is(r.nick, nick) && record::is(r.network, network) && record::is(r.channel, channel)) modes.push_back(r.mode);
    }
    return modes;
}

PsramVector<BanRecord> BanList::in_channel(const std::string& network, const std::string& channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    PsramVector<BanRecord> found;
    for (const auto& r : records_) {
        if (record::is(r.network, network) && record::is(r.channel, channel)) found.push_back(r);
    }
    return found;
}

std::vector<BanRecord> BanList::expired(const std::string& network, const Clock& now) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BanRecord> due;
    for (auto& r : records_) {
        if (r.deadline == -1 && now.epoch) r.deadline = now.steady + std::max<int64_t>(r.until - now.epoch, 0);
        if (r.deadline > 0 && now.steady >= r.deadline && record::is(r.network, network)) due.push_back(r);
    }
    return due;
}

// The last field is "never", "E<wall clock end>" or "L<seconds left>" (saved while the clock was unknown)
void BanList::load(const std::string& path, const Clock& now) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    record::read_lines(path, [&](const std::vector<std::string>& f) {
        if (f.size() != 5 || f[4].empty()) return;
        BanRecord r = {};
        record::set(r.network, f[0]);
        record::set(r.channel, f[1]);
        record::set(r.mode, f[2]);
        record::set(r.nick, f[3]);
        if (f[4][0] == 'L') {
            r.deadline = now.steady + std::max<long long>(record::number(f[4].substr(1)), 1);
        } else if (f[4][0] == 'E') {
            r.until = record::number(f[4].substr(1));
            r.deadline = now.epoch ? now.steady + std::max<int64_t>(r.until - now.epoch, 0) : -1;
        }
        records_.push_back(r);
    });
    changed_ = false;
}

bool BanList::save(const std::string& path, const Clock& now) {
    PsramVector<BanRecord> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!changed_) return true;
        copy = records_;
        changed_ = false;
    }
    const bool ok = record::write_file(path, [&](FILE* f) {
        for (const auto& r : copy) {
            std::string end = "never";
            if (r.deadline == -1) end = "E" + std::to_string(r.until);
            else if (r.deadline > 0 && now.epoch) end = "E" + std::to_string(now.epoch + r.deadline - now.steady);
            else if (r.deadline > 0) end = "L" + std::to_string(std::max<int64_t>(r.deadline - now.steady, 1));
            if (fprintf(f, "%s\t%s\t%s\t%s\t%s\n", r.network, r.channel, r.mode, r.nick, end.c_str()) < 0) return false;
        }
        return true;
    });
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        changed_ = true;
    }
    return ok;
}
