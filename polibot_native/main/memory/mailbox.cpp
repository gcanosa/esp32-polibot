#include "mailbox.h"
#include <algorithm>
#include "record_file.h"

TellResult Mailbox::add(const std::string& network, const std::string& to, const std::string& from, const std::string& text,
                        const Clock& now, int per_recipient, int per_sender, int max_total) {
    std::lock_guard<std::mutex> lock(mutex_);
    int for_them = 0, from_them = 0;
    for (const auto& r : records_) {
        if (!record::is(r.network, network)) continue;
        if (record::is(r.to, to)) for_them++;
        if (record::is(r.from, from)) from_them++;
    }
    if ((int)records_.size() >= max_total) return TellResult::full;
    if (for_them >= per_recipient) return TellResult::mailbox_full;
    if (from_them >= per_sender) return TellResult::sender_full;
    TellRecord r = {};
    record::set(r.network, network);
    record::set(r.to, to);
    record::set(r.from, from);
    record::set(r.text, text);
    r.when = now.epoch;
    records_.push_back(r);
    changed_ = true;
    return TellResult::stored;
}

std::vector<TellRecord> Mailbox::take(const std::string& network, const std::string& nick) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TellRecord> taken;
    for (auto it = records_.begin(); it != records_.end();) {
        if (record::is(it->to, nick) && record::is(it->network, network)) {
            taken.push_back(*it);
            it = records_.erase(it);
            changed_ = true;
        } else {
            ++it;
        }
    }
    return taken;
}

void Mailbox::forget_older_than(const Clock& now, int days) {
    if (!now.epoch) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const TellRecord& r) {
        return r.when && now.epoch - r.when > (int64_t)days * 86400;
    }), records_.end());
    if (records_.size() != before) changed_ = true;
}

void Mailbox::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    record::read_lines(path, [&](const std::vector<std::string>& f) {
        if (f.size() != 5) return;
        TellRecord r = {};
        record::set(r.network, f[0]);
        record::set(r.to, f[1]);
        record::set(r.from, f[2]);
        r.when = record::number(f[3]);
        record::set(r.text, f[4]);
        records_.push_back(r);
    });
    changed_ = false;
}

bool Mailbox::save(const std::string& path) {
    PsramVector<TellRecord> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!changed_) return true;
        copy = records_;
        changed_ = false;
    }
    const bool ok = record::write_file(path, [&](FILE* f) {
        for (const auto& r : copy) {
            if (fprintf(f, "%s\t%s\t%s\t%lld\t%s\n", r.network, r.to, r.from, (long long)r.when, r.text) < 0) return false;
        }
        return true;
    });
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        changed_ = true;
    }
    return ok;
}
