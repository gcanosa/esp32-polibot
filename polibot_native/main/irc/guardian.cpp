#include "guardian.h"
#include "mode_lock.h"
#include "spam.h"
#include <esp_timer.h>
#include <esp_log.h>
#include <algorithm>
#include <cctype>
#include <strings.h>

static const char* TAG = "GUARDIAN";

GuardianEngine::GuardianEngine(ConfigManager& config_mgr, SecurityManager& security_mgr)
    : config_mgr_(config_mgr), security_mgr_(security_mgr) {
    config_mgr_.register_rehash_listener([this](cJSON* root) {
        this->on_rehash(root);
    });
}

void GuardianEngine::load_rules() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = config_mgr_.get_root();
    if (!root) return;

    cJSON* g = cJSON_GetObjectItemCaseSensitive(root, "guardian");
    if (cJSON_IsObject(g)) {
        // Anti-Flood
        cJSON* flood = cJSON_GetObjectItemCaseSensitive(g, "anti_flood");
        if (cJSON_IsObject(flood)) {
            cJSON* en = cJSON_GetObjectItemCaseSensitive(flood, "enabled");
            cJSON* ml = cJSON_GetObjectItemCaseSensitive(flood, "max_lines");
            cJSON* tw = cJSON_GetObjectItemCaseSensitive(flood, "time_window");
            cJSON* ac = cJSON_GetObjectItemCaseSensitive(flood, "action");
            cJSON* kr = cJSON_GetObjectItemCaseSensitive(flood, "kick_reason");

            flood_enabled_ = en ? cJSON_IsTrue(en) : true;
            flood_max_lines_ = cJSON_IsNumber(ml) ? std::min(std::max(ml->valueint, 1), 100) : 4;
            flood_time_window_ = cJSON_IsNumber(tw) ? std::min(std::max(tw->valueint, 1), 600) : 4;
            flood_action_ = (ac && ac->valuestring) ? ac->valuestring : "kick";
            flood_kick_reason_ = (kr && kr->valuestring) ? kr->valuestring : "Flood protection triggered";
        }

        // Anti-Caps
        cJSON* caps = cJSON_GetObjectItemCaseSensitive(g, "anti_caps");
        if (cJSON_IsObject(caps)) {
            cJSON* en = cJSON_GetObjectItemCaseSensitive(caps, "enabled");
            cJSON* ml = cJSON_GetObjectItemCaseSensitive(caps, "min_length");
            cJSON* ct = cJSON_GetObjectItemCaseSensitive(caps, "caps_threshold");
            cJSON* ac = cJSON_GetObjectItemCaseSensitive(caps, "action");
            cJSON* kr = cJSON_GetObjectItemCaseSensitive(caps, "kick_reason");

            caps_enabled_ = en ? cJSON_IsTrue(en) : false;
            caps_min_length_ = cJSON_IsNumber(ml) ? std::min(std::max(ml->valueint, 1), 400) : 15;
            caps_threshold_ = cJSON_IsNumber(ct) ? std::min(std::max(ct->valueint, 1), 100) : 75;
            caps_action_ = (ac && ac->valuestring) ? ac->valuestring : "warn";
            caps_kick_reason_ = (kr && kr->valuestring) ? kr->valuestring : "Excessive caps detected";
        }

        // Anti-Badwords
        cJSON* bw = cJSON_GetObjectItemCaseSensitive(g, "anti_badwords");
        if (cJSON_IsObject(bw)) {
            cJSON* en = cJSON_GetObjectItemCaseSensitive(bw, "enabled");
            cJSON* ac = cJSON_GetObjectItemCaseSensitive(bw, "action");
            cJSON* kr = cJSON_GetObjectItemCaseSensitive(bw, "kick_reason");
            cJSON* words = cJSON_GetObjectItemCaseSensitive(bw, "words");

            badwords_enabled_ = en ? cJSON_IsTrue(en) : true;
            badwords_action_ = (ac && ac->valuestring) ? ac->valuestring : "kick";
            badwords_kick_reason_ = (kr && kr->valuestring) ? kr->valuestring : "Forbidden term detected";

            global_badwords_.clear();
            if (cJSON_IsArray(words)) {
                int sz = cJSON_GetArraySize(words);
                for (int i = 0; i < sz; i++) {
                    cJSON* w = cJSON_GetArrayItem(words, i);
                    if (w && w->valuestring) {
                        std::string word = w->valuestring;
                        std::transform(word.begin(), word.end(), word.begin(), ::tolower);
                        global_badwords_.push_back(word);
                    }
                }
            }
        }

        // Auto op admins
        cJSON* ao = cJSON_GetObjectItemCaseSensitive(g, "auto_op_admins");
        auto_op_admins_ = ao ? cJSON_IsTrue(ao) : true;

        load_access_locked(g);

        akicks_.clear();
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(g, "akick")) {
            auto text = [&](const char* key) -> std::string {
                cJSON* v = cJSON_GetObjectItemCaseSensitive(entry, key);
                return cJSON_IsString(v) ? v->valuestring : "";
            };
            AkickEntry a{text("network"), text("channel"), text("mask"), text("reason")};
            // a mask that matches everybody would empty the channel
            if (mask_is_specific(a.mask) && a.mask.find(' ') == std::string::npos && akicks_.size() < MAX_AKICKS) akicks_.push_back(a);
        }

        // Channel settings
        channel_settings_.clear();
        cJSON* chs = cJSON_GetObjectItemCaseSensitive(g, "channel_settings");
        if (cJSON_IsObject(chs)) {
            cJSON* ch = nullptr;
            cJSON_ArrayForEach(ch, chs) {
                if (ch->string) {
                    std::string ch_name = ch->string;
                    std::transform(ch_name.begin(), ch_name.end(), ch_name.begin(), ::tolower);
                    ChannelGuardianConfig cfg;

                    cJSON* dyn = cJSON_GetObjectItemCaseSensitive(ch, "dynamic_limit");
                    if (cJSON_IsObject(dyn)) {
                        cJSON* en = cJSON_GetObjectItemCaseSensitive(dyn, "enabled");
                        cJSON* off = cJSON_GetObjectItemCaseSensitive(dyn, "offset");
                        cJSON* ml = cJSON_GetObjectItemCaseSensitive(dyn, "min_limit");
                        cfg.dynamic_limit.enabled = en ? cJSON_IsTrue(en) : true;
                        cfg.dynamic_limit.offset = off ? off->valueint : 3;
                        cfg.dynamic_limit.min_limit = ml ? ml->valueint : 5;
                    }

                    cJSON* em = cJSON_GetObjectItemCaseSensitive(ch, "entrymsg");
                    if (cJSON_IsObject(em)) {
                        cJSON* en = cJSON_GetObjectItemCaseSensitive(em, "enabled");
                        cJSON* nt = cJSON_GetObjectItemCaseSensitive(em, "notice");
                        cJSON* msg = cJSON_GetObjectItemCaseSensitive(em, "message");
                        cfg.entrymsg.enabled = en ? cJSON_IsTrue(en) : false;
                        cfg.entrymsg.notice = nt ? cJSON_IsTrue(nt) : true;
                        cfg.entrymsg.message = (msg && msg->valuestring) ? msg->valuestring : "";
                    }

                    cJSON* words = cJSON_GetObjectItemCaseSensitive(ch, "badwords");
                    if (cJSON_IsArray(words)) {
                        int sz = cJSON_GetArraySize(words);
                        for (int i = 0; i < sz; i++) {
                            cJSON* w = cJSON_GetArrayItem(words, i);
                            if (w && w->valuestring) {
                                std::string word = w->valuestring;
                                std::transform(word.begin(), word.end(), word.begin(), ::tolower);
                                cfg.badwords.push_back(word);
                            }
                        }
                    }

                    cJSON* lk = cJSON_GetObjectItemCaseSensitive(ch, "lockdown");
                    cfg.lockdown = lk ? cJSON_IsTrue(lk) : false;
                    cfg.auto_lockdown = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ch, "auto_lockdown"));

                    cJSON* voice = cJSON_GetObjectItemCaseSensitive(ch, "timed_voice");
                    cfg.timed_voice.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(voice, "enabled"));
                    cJSON* minutes = cJSON_GetObjectItemCaseSensitive(voice, "minutes");
                    if (cJSON_IsNumber(minutes)) cfg.timed_voice.minutes = std::min(std::max(minutes->valueint, 1), MAX_VOICE_MINUTES);
                    cJSON* op = cJSON_GetObjectItemCaseSensitive(ch, "auto_op");
                    if (cJSON_IsBool(op)) cfg.auto_op = cJSON_IsTrue(op);
                    cJSON* topic = cJSON_GetObjectItemCaseSensitive(ch, "topic_lock");
                    cJSON* kept = cJSON_GetObjectItemCaseSensitive(topic, "topic");
                    cfg.topic_lock = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(topic, "enabled"));
                    if (cJSON_IsString(kept)) cfg.topic = kept->valuestring;
                    cJSON* modes = cJSON_GetObjectItemCaseSensitive(ch, "mode_lock");
                    if (cJSON_IsString(modes)) cfg.mode_lock = clean_mode_lock(modes->valuestring);

                    channel_settings_[ch_name] = cfg;
                }
            }
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Guardian rules loaded.");
}

void GuardianEngine::on_rehash(cJSON* root) {
    load_rules();
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.clear();
    ESP_LOGI(TAG, "Guardian message history reset via rehash.");
}

static std::string rules_key(const std::string& network, const std::string& channel) {
    std::string key = network + "/" + channel;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    return key;
}

const ChannelGuardianConfig* GuardianEngine::find_channel_locked(const std::string& network, const std::string& channel) const {
    for (const auto& key : {rules_key(network, channel), irc_lower(channel)}) {
        auto it = channel_settings_.find(key);
        if (it != channel_settings_.end()) return &it->second;
    }
    return nullptr;
}

ChannelGuardianConfig GuardianEngine::get_channel_config(const std::string& network, const std::string& channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChannelGuardianConfig* stored = find_channel_locked(network, channel);
    return stored ? *stored : ChannelGuardianConfig();
}

void GuardianEngine::save_channel_config(const std::string& network, const std::string& channel, const ChannelGuardianConfig& cfg) {
    const std::string ch_lower = rules_key(network, channel);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel_settings_[ch_lower] = cfg;
    }

    cJSON* root = config_mgr_.get_root();
    if (!root) return;

    // Detach: set_item() takes ownership, so the node must not stay linked into the temporary copy
    cJSON* g = cJSON_DetachItemFromObjectCaseSensitive(root, "guardian");
    cJSON_Delete(root);
    if (!g) g = cJSON_CreateObject();
    cJSON* chs = cJSON_GetObjectItemCaseSensitive(g, "channel_settings");
    if (!chs) {
        chs = cJSON_CreateObject();
        cJSON_AddItemToObject(g, "channel_settings", chs);
    }

    cJSON* ch_obj = cJSON_CreateObject();
    cJSON* dyn = cJSON_CreateObject();
    cJSON_AddBoolToObject(dyn, "enabled", cfg.dynamic_limit.enabled);
    cJSON_AddNumberToObject(dyn, "offset", cfg.dynamic_limit.offset);
    cJSON_AddNumberToObject(dyn, "min_limit", cfg.dynamic_limit.min_limit);
    cJSON_AddItemToObject(ch_obj, "dynamic_limit", dyn);

    cJSON* em = cJSON_CreateObject();
    cJSON_AddBoolToObject(em, "enabled", cfg.entrymsg.enabled);
    cJSON_AddBoolToObject(em, "notice", cfg.entrymsg.notice);
    cJSON_AddStringToObject(em, "message", cfg.entrymsg.message.c_str());
    cJSON_AddItemToObject(ch_obj, "entrymsg", em);

    cJSON* bw = cJSON_CreateArray();
    for (const auto& w : cfg.badwords) {
        cJSON_AddItemToArray(bw, cJSON_CreateString(w.c_str()));
    }
    cJSON_AddItemToObject(ch_obj, "badwords", bw);
    cJSON_AddBoolToObject(ch_obj, "lockdown", cfg.lockdown);
    cJSON_AddBoolToObject(ch_obj, "auto_lockdown", cfg.lockdown && cfg.auto_lockdown);
    cJSON* voice = cJSON_AddObjectToObject(ch_obj, "timed_voice");
    cJSON_AddBoolToObject(voice, "enabled", cfg.timed_voice.enabled);
    cJSON_AddNumberToObject(voice, "minutes", cfg.timed_voice.minutes);
    if (cfg.auto_op >= 0) cJSON_AddBoolToObject(ch_obj, "auto_op", cfg.auto_op == 1);
    if (cfg.topic_lock || !cfg.topic.empty()) {
        cJSON* topic = cJSON_AddObjectToObject(ch_obj, "topic_lock");
        cJSON_AddBoolToObject(topic, "enabled", cfg.topic_lock);
        cJSON_AddStringToObject(topic, "topic", cfg.topic.c_str());
    }
    if (!cfg.mode_lock.empty()) cJSON_AddStringToObject(ch_obj, "mode_lock", cfg.mode_lock.c_str());

    cJSON_DeleteItemFromObjectCaseSensitive(chs, ch_lower.c_str());
    cJSON_AddItemToObject(chs, ch_lower.c_str(), ch_obj);

    config_mgr_.set_item("guardian", g);
    config_mgr_.save();
}

AutoModeRules GuardianEngine::auto_modes(const std::string& network, const std::string& channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChannelGuardianConfig* stored = find_channel_locked(network, channel);
    AutoModeRules rules;
    rules.admins = stored && stored->auto_op >= 0 ? stored->auto_op == 1 : auto_op_admins_;
    rules.listed = access_.any(network, channel);
    if (stored) {
        rules.voice_minutes = stored->timed_voice.enabled ? stored->timed_voice.minutes : 0;
        rules.lockdown = stored->lockdown;
    }
    return rules;
}

std::vector<std::string> GuardianEngine::get_global_badwords() {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_badwords_;
}

std::vector<AkickEntry> GuardianEngine::get_akicks() {
    std::lock_guard<std::mutex> lock(mutex_);
    return akicks_;
}

static bool same(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) == 0; }

bool GuardianEngine::akick_match(const std::string& network, const std::string& channel, const std::string& hostmask, AkickEntry& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& a : akicks_) {
        if ((a.network.empty() || same(a.network, network)) && (a.channel.empty() || same(a.channel, channel)) && wildcard_match(a.mask, hostmask)) {
            out = a;
            return true;
        }
    }
    return false;
}

// Writes the list to guardian.akick and saves; call without holding mutex_
static bool save_akicks(ConfigManager& config_mgr, const std::vector<AkickEntry>& list) {
    return config_mgr.update([&](cJSON* root) {
        cJSON* g = cJSON_GetObjectItemCaseSensitive(root, "guardian");
        if (!g) g = cJSON_AddObjectToObject(root, "guardian");
        cJSON_DeleteItemFromObjectCaseSensitive(g, "akick");
        cJSON* arr = cJSON_AddArrayToObject(g, "akick");
        for (const auto& a : list) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "network", a.network.c_str());
            cJSON_AddStringToObject(o, "channel", a.channel.c_str());
            cJSON_AddStringToObject(o, "mask", a.mask.c_str());
            cJSON_AddStringToObject(o, "reason", a.reason.c_str());
            cJSON_AddItemToArray(arr, o);
        }
        return true;
    });
}

bool GuardianEngine::add_akick(const AkickEntry& entry) {
    std::vector<AkickEntry> list = get_akicks();
    auto it = std::find_if(list.begin(), list.end(), [&](const AkickEntry& a) {
        return same(a.network, entry.network) && same(a.channel, entry.channel) && same(a.mask, entry.mask);
    });
    if (it != list.end()) *it = entry;
    else if (list.size() >= MAX_AKICKS) return false;
    else list.push_back(entry);
    if (!save_akicks(config_mgr_, list)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    akicks_ = list;
    return true;
}

bool GuardianEngine::remove_akick(const std::string& network, const std::string& channel, const std::string& mask) {
    std::vector<AkickEntry> list = get_akicks();
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const AkickEntry& a) {
        // an entry written in the panel for every network (empty) can be removed from any of them
        return (a.network.empty() || same(a.network, network)) && same(a.channel, channel) && same(a.mask, mask);
    }), list.end());
    if (list.size() == before || !save_akicks(config_mgr_, list)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    akicks_ = list;
    return true;
}

GuardianAction GuardianEngine::verdict_locked(const std::string& action, const std::string& reason, const std::string& channel,
                                              const std::string& nick, const std::string& offender, const BotSettings& settings) {
    GuardianAction act;
    act.type = action;
    act.channel = channel;
    act.nick = nick;
    act.reason = reason;
    if (action == "ban") act.seconds = settings.ban_minutes * 60;
    if (action == "quiet") act.seconds = settings.quiet_minutes * 60;
    if (action == "escalate") {
        const int offence = strikes_.add(offender, esp_timer_get_time() / 1000000, settings.strike_reset_minutes * 60);
        const std::vector<std::string> steps = parse_steps(settings.escalation_steps);
        act.type = steps.empty() ? "kick" : steps[std::min<size_t>(offence, steps.size()) - 1];
        if (act.type == "quiet") act.seconds = settings.escalate_quiet_minutes * 60;
        if (act.type == "ban") act.seconds = settings.escalate_ban_minutes * 60;
        act.reason += " (offence " + std::to_string(offence) + ")";
    }
    return act;
}

std::vector<GuardianAction> GuardianEngine::inspect_message(const std::string& network, const std::string& channel, const std::string& nick, const std::string& hostmask,
                                                            const std::string& message, const LineContext& line) {
    std::vector<GuardianAction> actions;

    // Moderators or higher bypass sentry
    if (security_mgr_.has_permission(network, nick, hostmask, "mod", channel)) {
        return actions;
    }

    const auto settings = g_settings.get();
    const int64_t now_sec = esp_timer_get_time() / 1000000;
    // counted by user@host, so that changing nickname does not start a new count
    const std::string who = user_host(hostmask).empty() ? nick : user_host(hostmask);
    const std::string room = irc_lower(network + "/" + channel);
    const std::string offender = room + ":" + irc_lower(who);

    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Anti-Flood Check
    if (flood_enabled_) {
        const int recent = messages_.hit(offender, now_sec, flood_time_window_ + 1);
        if (recent > flood_max_lines_) {
            actions.push_back(verdict_locked(flood_action_, flood_kick_reason_, channel, nick, offender, *settings));
            messages_.clear(offender);
            ESP_LOGW(TAG, "Flood tripped by %s in %s (%d lines in %ds)", nick.c_str(), channel.c_str(), recent, flood_time_window_);
            return actions;
        }
    }

    // 2. Anti-Badwords Check
    if (badwords_enabled_) {
        const std::string msg_lower = irc_lower(message);

        // the channel's own words after the global ones, neither list copied
        const ChannelGuardianConfig* stored = find_channel_locked(network, channel);
        auto found = [&](const std::vector<std::string>& words) -> const std::string* {
            for (const auto& w : words) {
                if (!w.empty() && msg_lower.find(w) != std::string::npos) return &w;
            }
            return nullptr;
        };
        const std::string* word = found(global_badwords_);
        if (!word && stored) word = found(stored->badwords);
        if (word) {
            actions.push_back(verdict_locked(badwords_action_, badwords_kick_reason_ + " (" + *word + ")", channel, nick, offender, *settings));
            ESP_LOGW(TAG, "Badword '%s' tripped by %s in %s", word->c_str(), nick.c_str(), channel.c_str());
            return actions;
        }
    }

    // 3. Advertising: addresses and other channels
    if (settings->links_action != "off" && !(settings->links_spare_voiced && line.voiced)) {
        const std::string advert = find_advert(message, channel, settings->links_channels, settings->links_allowed);
        if (!advert.empty()) {
            actions.push_back(verdict_locked(settings->links_action, settings->links_reason + " (" + advert.substr(0, 60) + ")", channel, nick, offender, *settings));
            return actions;
        }
    }

    // 4. Naming many of those present at once
    if (settings->highlight_action != "off" && line.named >= settings->highlight_nicks) {
        actions.push_back(verdict_locked(settings->highlight_action, settings->highlight_reason, channel, nick, offender, *settings));
        return actions;
    }

    // 5. Anti-Caps Check
    if (caps_enabled_) {
        size_t letters = 0;
        size_t caps = 0;
        for (char c : message) {
            if (std::isalpha((unsigned char)c)) {
                letters++;
                if (std::isupper((unsigned char)c)) {
                    caps++;
                }
            }
        }
        if (letters >= (size_t)caps_min_length_) {
            int pct = (caps * 100) / letters;
            if (pct >= caps_threshold_) {
                actions.push_back(verdict_locked(caps_action_, caps_kick_reason_, channel, nick, offender, *settings));
                ESP_LOGW(TAG, "Caps (%d%%) tripped by %s in %s", pct, nick.c_str(), channel.c_str());
                return actions;
            }
        }
    }

    // 6. The same text from several users
    if (settings->clone_action != "off" && (int)message.size() >= settings->clone_min_length) {
        const std::vector<std::string> nicks = clones_.add(room, who, nick, message, now_sec, settings->clone_users, settings->clone_seconds);
        if (nicks.empty()) return actions;
        if (settings->clone_action == "report" || settings->clone_action == "lockdown") {
            std::string names;
            for (const auto& n : nicks) names += (names.empty() ? "" : ", ") + n;
            actions.push_back(verdict_locked(settings->clone_action, "Same text from several users: " + names.substr(0, 200), channel, nick, offender, *settings));
        } else {
            for (const auto& n : nicks) {
                // strikes of the earlier senders are counted by nickname: their host is not kept here
                const std::string key = n == nick ? offender : room + ":" + irc_lower(n);
                actions.push_back(verdict_locked(settings->clone_action, "Repeated text flood", channel, n, key, *settings));
            }
        }
    }

    return actions;
}

std::vector<GuardianAction> GuardianEngine::on_user_join(const std::string& network, const std::string& channel, const std::string& nick, const std::string& hostmask, bool count_for_raid) {
    std::vector<GuardianAction> actions;
    const auto settings = g_settings.get();
    const bool staff = security_mgr_.has_permission(network, nick, hostmask, "mod", channel);

    // Auto-kick list
    AkickEntry listed;
    if (!staff && akick_match(network, channel, hostmask, listed)) {
        GuardianAction act;
        act.type = "akick";
        act.channel = channel;
        act.nick = nick;
        act.mode = listed.mask;
        act.reason = listed.reason.empty() ? "You are on the auto-kick list" : listed.reason;
        actions.push_back(act);
    }

    // Join flood (counted even for the listed ones: they are part of a raid too)
    if (!staff && count_for_raid && settings->join_action != "off") {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string room = irc_lower(network + "/" + channel);
        const int joins = joins_.hit(room, esp_timer_get_time() / 1000000, settings->join_seconds);
        if (joins >= settings->join_count) {
            joins_.clear(room);
            GuardianAction act;
            act.type = settings->join_action;
            act.channel = channel;
            act.reason = "Join flood: " + std::to_string(joins) + " joins within " + std::to_string(settings->join_seconds) + "s";
            actions.push_back(act);
        }
    }
    if (!actions.empty() && actions.front().type == "akick") return actions;

    // Per-channel entry message
    ChannelGuardianConfig ch_cfg = get_channel_config(network, channel);
    if (ch_cfg.entrymsg.enabled && !ch_cfg.entrymsg.message.empty()) {
        GuardianAction act;
        act.type = ch_cfg.entrymsg.notice ? "notice" : "privmsg";
        act.target = nick;
        act.message = ch_cfg.entrymsg.message;
        actions.push_back(act);
    }

    return actions;
}

std::vector<GuardianAction> GuardianEngine::on_nick_change(const std::string& network, const std::vector<std::string>& channels, const std::string& nick,
                                                           const std::string& hostmask) {
    std::vector<GuardianAction> actions;
    const auto settings = g_settings.get();
    if (settings->nick_action == "off" || channels.empty()) return actions;
    // staff in any of the channels are left alone altogether
    for (const auto& channel : channels) {
        if (security_mgr_.has_permission(network, nick, hostmask, "mod", channel)) return actions;
    }
    const std::string who = user_host(hostmask).empty() ? nick : user_host(hostmask);
    const std::string key = irc_lower(network + "/" + who);

    std::lock_guard<std::mutex> lock(mutex_);
    if (churn_.hit("nick " + key, esp_timer_get_time() / 1000000, settings->nick_seconds) <= settings->nick_changes) return actions;
    churn_.clear("nick " + key);
    for (const auto& channel : channels) {
        actions.push_back(verdict_locked(settings->nick_action, settings->nick_reason, channel, nick, irc_lower(network + "/" + channel + ":" + who), *settings));
        if (settings->nick_action == "report") break; // once is enough
    }
    return actions;
}

std::vector<GuardianAction> GuardianEngine::on_presence(const std::string& network, const std::string& channel, const std::string& nick,
                                                        const std::string& hostmask, bool joined) {
    std::vector<GuardianAction> actions;
    const auto settings = g_settings.get();
    if (settings->presence_action == "off" || security_mgr_.has_permission(network, nick, hostmask, "mod", channel)) return actions;
    const std::string who = user_host(hostmask).empty() ? nick : user_host(hostmask);
    const std::string offender = irc_lower(network + "/" + channel + ":" + who);

    std::lock_guard<std::mutex> lock(mutex_);
    const int count = churn_.hit("stay " + offender, esp_timer_get_time() / 1000000, settings->presence_seconds);
    if (!joined || count <= settings->presence_count) return actions;
    churn_.clear("stay " + offender);
    GuardianAction act = verdict_locked(settings->presence_action, settings->presence_reason, channel, nick, offender, *settings);
    act.seconds = settings->presence_minutes * 60;
    actions.push_back(act);
    return actions;
}
