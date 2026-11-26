// IRCClient: modes given without being asked: the access lists, admins in a channel, and timed voice
#include "irc_client.h"
#include "irc_internal.h"
#include "bot_memory.h"
#include <cstring>

static constexpr size_t GRANTS_PER_PASS = 12; // the send queue paces them; the next look takes the rest

static bool holds(const Member& m, uint8_t bit) {
    if (bit == AUTO_OP) return m.has_mode('o') || m.has_mode('a') || m.has_mode('q');
    return m.has_mode(bit == AUTO_HALFOP ? 'h' : 'v');
}

static std::string hostmask_of(const Member& m) { return std::string(m.nick) + "!" + m.userhost; }

// Something changed (the lists, the rules, the bot's own rank): look at everybody again soon
void IRCClient::review_modes(const std::string& channel) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& entry : channels_) {
        if (!channel.empty() && !same(entry.second.name, channel)) continue;
        entry.second.review = true;
        entry.second.grant_check = 0;
    }
}

// Somebody has just identified (on the connection task)
void IRCClient::grant_everywhere(const std::string& nick) {
    for (const auto& channel : get_joined_channels()) grant_modes(channel, nick, true);
}

// Gives what is due in one channel, to `only` or else to everybody. With `everyone` the lists and the
// memory are asked about each of them; without it only those whose waiting time is up are looked at,
// so that the regular look costs next to nothing.
void IRCClient::grant_modes(const std::string& channel, const std::string& only, bool everyone) {
    if (is_report_channel(channel)) return;
    const AutoModeRules rules = guardian_.auto_modes(name_, channel);
    if (!rules.listed && !rules.admins && !rules.voice_minutes) return;
    const uint32_t now = (uint32_t)seconds_now();
    const uint32_t wait = (uint32_t)rules.voice_minutes * 60;
    const std::string me = current_nick();
    PsramVector<Member> candidates;
    size_t per_line;
    uint8_t can = 0;
    bool sure, paused, ask_modes = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end()) return;
        ChannelState& state = it->second;
        // before the first timed voice the bot has to know whether the channel is moderated
        if (rules.voice_minutes && !state.modes_asked) state.modes_asked = ask_modes = true;
        paused = state.moderated || rules.lockdown;
        // the waiting time stands still while the channel is moderated or locked down
        if (rules.voice_minutes && paused && !state.paused_at) {
            state.paused_at = now;
        } else if (!paused && state.paused_at) {
            for (Member& m : state.members.all()) {
                if (m.since) m.since = after_pause(m.since, state.paused_at, now);
            }
            state.paused_at = 0;
        }
        per_line = support_.max_modes;
        const Member* self = state.members.find(me);
        // as for every action, not knowing whether it is an operator does not stop the bot from trying
        // (timed voice does wait until it is sure)
        sure = state.op == 1;
        if (state.op != 0) {
            // a half-operator can give voice, and nothing more
            const bool half = self && self->has_mode('h') && !holds(*self, AUTO_OP);
            can = half ? AUTO_VOICE : AUTO_OP | AUTO_HALFOP | AUTO_VOICE;
            if (support_.prefix_modes.find('h') == std::string::npos) can &= ~AUTO_HALFOP;
        }
        const Member* wanted = only.empty() ? nullptr : state.members.find(only);
        for (Member& m : state.members.all()) {
            if (!only.empty() && &m != wanted) continue;
            if (!m.since) m.since = now;
            if (!can || &m == self || !m.userhost[0]) continue;
            uint8_t open = can & ~m.settled;
            for (uint8_t bit : {AUTO_OP, AUTO_HALFOP, AUTO_VOICE}) {
                if (holds(m, bit)) open &= ~bit;
            }
            if (!open) continue;
            const bool due = sure && wait && !paused && !ask_modes && (open & AUTO_VOICE) && !m.is_operator() && now - m.since >= wait;
            if (due || everyone) candidates.push_back(m);
        }
    }
    if (ask_modes) send_raw("MODE " + channel); // answered by 324

    const auto settings = g_settings.get();
    std::vector<std::pair<char, std::string>> grants;
    std::vector<std::pair<std::string, uint8_t>> settled;
    bool more = false;
    for (const Member& m : candidates) {
        if (grants.size() >= GRANTS_PER_PASS) {
            more = true;
            break;
        }
        const std::string hostmask = hostmask_of(m);
        const Identity who = security_mgr_.identify(name_, m.nick, hostmask, channel);
        uint8_t held = 0;
        for (uint8_t bit : {AUTO_OP, AUTO_HALFOP, AUTO_VOICE}) {
            if (holds(m, bit)) held |= bit;
        }
        uint8_t want = rules.listed ? guardian_.listed_modes(name_, channel, hostmask, who.account, who.user) : 0;
        if (rules.admins && who.level >= get_role_level("admin")) want |= AUTO_OP;
        bool timed = false;
        if (sure && wait && !(want & can) && !m.is_operator() && !holds(m, AUTO_VOICE)) {
            const VoiceState known = g_memory.voices.find(name_, channel, m.userhost, who.account);
            // earned before: at once, moderated or not; otherwise when the time is up
            timed = known == VoiceState::earned ||
                    (known == VoiceState::unknown && !paused && !ask_modes && now - m.since >= wait);
            if (timed) want = AUTO_VOICE;
            if (known == VoiceState::revoked) settled.push_back({m.nick, AUTO_VOICE});
        }
        want &= can & ~(m.settled | held);
        if (!want) continue;
        // never past a ban or a mute (on some servers voice talks through it), nor to somebody to be removed
        AkickEntry listed;
        if (g_memory.bans.any_of(name_, channel, [&](const char* mode) {
                const char* space = strchr(mode, ' ');
                return space && mask_restricts(space + 1, hostmask, who.account);
            }) || (who.level < get_role_level("mod") && guardian_.akick_match(name_, channel, hostmask, listed))) {
            settled.push_back({m.nick, want});
            continue;
        }
        if (timed) g_memory.voices.earn(name_, channel, m.userhost, who.account, bot_clock(), settings->voice_max);
        for (char mode : {'o', 'h', 'v'}) {
            if (want & auto_bit(mode)) grants.push_back({mode, m.nick});
        }
        if (sure) settled.push_back({m.nick, want}); // otherwise tried again once the bot knows it can
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end()) return;
        for (const auto& s : settled) {
            if (Member* m = it->second.members.find(s.first)) m->settled |= s.second;
        }
        if (more) it->second.review = true;
    }
    for (const auto& line : grant_lines(grants, per_line)) {
        log("Automatic modes in " + channel + ": " + line);
        send_raw("MODE " + channel + " " + line);
    }
}

// What happened to somebody's voice: taken away (devoiced, kicked, muted) or given by somebody
void IRCClient::note_voice(const std::string& channel, const PsramVector<Member>& people, bool lost) {
    if (people.empty() || is_report_channel(channel) || !guardian_.auto_modes(name_, channel).voice_minutes) return;
    const auto settings = g_settings.get();
    for (const Member& m : people) {
        if (!m.userhost[0]) continue;
        const std::string account = security_mgr_.identify(name_, m.nick, hostmask_of(m), channel).account;
        if (lost) g_memory.voices.revoke(name_, channel, m.userhost, account, bot_clock(), settings->voice_max);
        else g_memory.voices.restore(name_, channel, m.userhost, account);
    }
}

// Before a ban or a mute: voice would let them talk through it on some servers
void IRCClient::devoice_restricted(const std::string& channel, const std::string& change) {
    const size_t space = change.find(' ');
    if (space == std::string::npos || !can_act(channel)) return;
    const std::string mask = change.substr(space + 1);
    const std::string me = current_nick();
    std::vector<std::pair<char, std::string>> voiced;
    size_t per_line;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end()) return;
        per_line = support_.max_modes;
        for (const Member& m : it->second.members.all()) {
            if (!m.has_mode('v') || !m.userhost[0] || same(m.nick, me)) continue;
            const std::string hostmask = hostmask_of(m);
            if (mask_restricts(mask, hostmask, security_mgr_.identify(name_, m.nick, hostmask, channel).account)) voiced.push_back({'v', m.nick});
        }
    }
    for (auto line : grant_lines(voiced, per_line)) {
        line[0] = '-';
        send_raw("MODE " + channel + " " + line);
    }
}

// What is typed for a list entry. A nickname never stands for itself: somebody here by that name is
// entered by services account, or else by hostmask; anybody else is taken to be an account name.
std::string IRCClient::access_who(const std::string& typed) {
    if (typed.rfind("account:", 0) == 0) return typed.substr(8);
    if (typed.find_first_of("!@:") != std::string::npos) return access_mask(typed);
    std::string userhost;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& entry : channels_) {
            const Member* m = entry.second.members.find(typed);
            if (m && m->userhost[0]) userhost = m->userhost;
        }
    }
    if (userhost.empty()) return typed;
    const std::string account = security_mgr_.identify(name_, typed, typed + "!" + userhost, "").account;
    return account.empty() ? "*!" + userhost : account;
}
