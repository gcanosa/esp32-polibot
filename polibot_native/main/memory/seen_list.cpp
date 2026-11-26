#include "seen_list.h"
#include <algorithm>
#include "record_file.h"

void SeenList::note(const std::string& network, const std::string& nick, const std::string& userhost, char action,
                    const std::string& channel, const std::string& text, const Clock& now, size_t max_entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (max_entries == 0) return;
    SeenRecord* found = nullptr;
    for (auto& r : records_) {
        if (record::is(r.nick, nick) && record::is(r.network, network)) {
            found = &r;
            break;
        }
    }
    if (!found) {
        // the limit may have been lowered: forget the oldest until there is room
        while (records_.size() >= max_entries) {
            records_.erase(std::min_element(records_.begin(), records_.end(),
                                            [](const SeenRecord& a, const SeenRecord& b) { return a.order < b.order; }));
        }
        if (records_.capacity() < max_entries) records_.reserve(max_entries);
        records_.push_back(SeenRecord{});
        found = &records_.back();
        record::set(found->network, network);
    }
    record::set(found->nick, nick);
    if (!userhost.empty()) record::set(found->userhost, userhost);
    record::set(found->channel, channel);
    record::set(found->text, text);
    found->action = action;
    found->when = now.epoch;
    found->order = ++order_;
    changed_ = true;
}

bool SeenList::find(const std::string& network, const std::string& nick, SeenRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : records_) {
        if (record::is(r.nick, nick) && record::is(r.network, network)) {
            out = r;
            return true;
        }
    }
    return false;
}

void SeenList::forget_older_than(const Clock& now, int days) {
    if (!now.epoch) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const SeenRecord& r) {
        return r.when && now.epoch - r.when > (int64_t)days * 86400;
    }), records_.end());
    if (records_.size() != before) changed_ = true;
}

void SeenList::load(const std::string& path, const Clock& now) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    order_ = 0;
    record::read_lines(path, [&](const std::vector<std::string>& f) {
        if (f.size() != 8 || f[5].size() != 1) return;
        SeenRecord r = {};
        record::set(r.network, f[0]);
        record::set(r.nick, f[1]);
        record::set(r.userhost, f[2]);
        record::set(r.channel, f[3]);
        r.when = record::number(f[4]);
        r.action = f[5][0];
        r.order = (uint32_t)record::number(f[6]);
        record::set(r.text, f[7]);
        order_ = std::max(order_, r.order);
        records_.push_back(r);
    });
    changed_ = false;
    saved_at_ = now.steady;
}

bool SeenList::save(const std::string& path, const Clock& now, int interval, bool force) {
    PsramVector<SeenRecord> copy; // written without the lock, so IRC never waits for the flash
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!changed_ || (!force && now.steady - saved_at_ < interval)) return true;
        copy = records_;
        changed_ = false;
        saved_at_ = now.steady;
    }
    const bool ok = record::write_file(path, [&](FILE* f) {
        for (const auto& r : copy) {
            if (fprintf(f, "%s\t%s\t%s\t%s\t%lld\t%c\t%u\t%s\n", r.network, r.nick, r.userhost, r.channel,
                        (long long)r.when, r.action, (unsigned)r.order, r.text) < 0) return false;
        }
        return true;
    });
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        changed_ = true; // try again next time
    }
    return ok;
}
