// IRCClient: who is where, kept up to date from the server
#include "irc_client.h"
#include "irc_internal.h"
#include "mode_lock.h"
#include <cstring>

static constexpr int NETSPLIT_MEMORY = 30 * 60;  // how long a split user may take to come back

bool IRCClient::channel_event(const std::string& command, const std::vector<std::string>& args, const std::string& trailing,
                              const std::string& nick, const std::string& hostmask, bool from_me) {
    const std::string first = args.empty() ? trailing : args[0];
    // numerics carry our own nickname first, then the channel
    const std::string subject = args.size() >= 2 ? args[1] : "";

    if (command == "JOIN") {
        // with extended-join the account and the real name follow the channel
        const std::string channel = args.empty() ? trailing : args[0];
        if (cap_extended_join_ && cap_account_notify_ && args.size() > 1) learn_account(nick, user_host(hostmask), args[1]);
        if (!channel.empty()) on_join(channel, nick, hostmask, from_me);
    } else if (command == "ACCOUNT") {
        if (cap_account_notify_ && learn_account(nick, user_host(hostmask), first)) grant_everywhere(nick);
    } else if (command == "PART") {
        const std::string channel = first;
        std::vector<std::string> leaving{nick};
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(irc_lower(channel));
            if (it == channels_.end()) return true;
            if (from_me) {
                for (const auto& m : it->second.members.all()) leaving.push_back(m.nick);
                channels_.erase(it);
            } else {
                it->second.members.remove(nick);
            }
        }
        forget_if_gone(leaving);
        if (!from_me) {
            note_seen(nick, hostmask, 'p', channel, args.empty() ? "" : trailing);
            note_presence(channel, nick, hostmask, false);
        }
    } else if (command == "KICK") {
        if (args.size() >= 2) on_kick(args[0], args[1], nick, trailing, same(args[1], current_nick()));
    } else if (command == "QUIT") {
        on_quit_or_nick(true, nick, hostmask, trailing, from_me);
    } else if (command == "NICK") {
        const std::string next = trailing.empty() ? first : trailing;
        if (!next.empty()) on_quit_or_nick(false, nick, hostmask, next, from_me);
    } else if (command == "MODE") {
        std::vector<std::string> modes(args.empty() ? args.end() : args.begin() + 1, args.end());
        if (!trailing.empty()) modes.push_back(trailing);
        if (!args.empty() && !modes.empty()) on_mode(args[0], modes, nick, hostmask);
    } else if (command == "TOPIC") {
        if (!args.empty()) on_topic(first, nick, hostmask, trailing.empty() && args.size() > 1 ? args[1] : trailing);
    } else if (command == "331") { // no topic is set
        if (!subject.empty()) note_topic(subject, "");
    } else if (command == "332") { // <me> <channel> :topic
        if (!subject.empty()) note_topic(subject, trailing.empty() && args.size() > 2 ? args[2] : trailing);
    } else if (command == "INVITE") {
        const std::string channel = subject.empty() ? trailing : subject;
        if (wanted_channel(channel)) send_raw(join_line(channel));
    } else if (command == "005") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (args.size() <= 1) return true;
        support_.apply(std::vector<std::string>(args.begin() + 1, args.end()));
        for (const auto& token : args) {
            if (token.rfind("BOT=", 0) == 0) bot_letter_ = token.substr(4, 1);
            if (token == "WHOX") whox_ = true;
        }
    } else if (command == "353") {
        if (args.size() >= 3) on_names(args[2], trailing);
    } else if (command == "366") {
        int op = -1;
        const int grace = g_settings.get()->limit_grace_seconds;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(irc_lower(subject));
            if (it == channels_.end()) return true;
            if (it->second.limit_from < 0) { // the member list is complete
                it->second.limit_from = seconds_now() + grace;
                it->second.lift_limit = grace > 0;
            }
            const std::string me = current_nick_.empty() ? cfg_->nick : current_nick_;
            if (const Member* m = it->second.members.find(me)) op = m->is_operator() ? 1 : 0;
        }
        if (op >= 0) note_operator(subject, op == 1, ""); // an operator asks about the password itself
        if (op != 1 && is_report_channel(subject)) send_raw("MODE " + subject); // is the password set?
    } else if (command == "352") {
        // <me> <channel> <user> <host> <server> <nick> <flags>
        if (args.size() >= 7) on_who(args[1], args[2] + "@" + args[3], args[5], args[6]);
    } else if (command == "354") {
        // our WHOX question: <me> 616 <channel> <user> <host> <nick> <flags> <account>
        if (args.size() >= 8 && args[1] == "616") {
            const std::string userhost = args[3] + "@" + args[4];
            if (cap_account_notify_) learn_account(args[5], userhost, args[7]);
            on_who(args[2], userhost, args[5], args[6]);
        }
    } else if (command == "315") {
        if (!subject.empty()) {
            sweep_akicks(subject); // the hosts of the members are known now
            review_modes(subject);
            finish_masskick(subject);
        }
    } else if (command == "324") {
        if (args.size() >= 3) {
            std::vector<std::string> modes(args.begin() + 2, args.end());
            if (!trailing.empty()) modes.push_back(trailing);
            on_channel_modes(subject, modes);
        }
    } else if (command == "482") {
        if (!subject.empty()) {
            note_operator(subject, false, "");
            cannot_act(subject, "do that");
        }
    } else if (command == "403" || command == "405" || command == "437" || command == "471" || command == "473" ||
               command == "474" || command == "475" || command == "477" || command == "489") {
        if (!subject.empty()) on_join_refused(command, subject);
    } else {
        return false;
    }
    return true;
}

// The modes a channel has (324): whether it is moderated, and the report channel's password
void IRCClient::on_channel_modes(const std::string& channel, const std::vector<std::string>& modes) {
    const bool reports = is_report_channel(channel);
    bool has_key = false;
    std::string key, present;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        bool moderated = false;
        for (const auto& change : parse_modes(support_, modes)) {
            if (change.add) present += change.mode;
            if (change.mode == 'm' && change.add) moderated = true;
            if (change.mode == 'k' && change.add) {
                has_key = true;
                key = change.param;
            }
        }
        if (it != channels_.end()) it->second.moderated = moderated;
    }
    if (reports) {
        check_report_key(has_key, key);
        return;
    }
    const ChannelGuardianConfig rules = guardian_.get_channel_config(name_, channel);
    if (rules.mode_lock.empty() || !can_act(channel)) return;
    const std::string skip = rules.lockdown ? g_settings.get()->lockdown_modes : "";
    const std::string fix = missing_modes(rules.mode_lock, present, skip);
    if (!fix.empty() && may_revert(channel)) send_raw("MODE " + channel + " " + fix);
}

void IRCClient::on_join(const std::string& channel, const std::string& nick, const std::string& hostmask, bool from_me) {
    const std::string key = irc_lower(channel);
    const bool reports = is_report_channel(channel);
    if (from_me) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ChannelState& state = channels_[key];
            state = ChannelState();
            state.name = channel;
            pending_joins_.erase(key);
            if (reports) report_problem_.clear();
        }
        log("Joined channel: " + channel);
        // hosts and ranks of those already there, and their accounts where the server can say
        send_raw("WHO " + channel + (whox_ && cap_account_notify_ ? " %tcuhnfa,616" : ""));
        return;
    }
    bool returning = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(key);
        returning = netsplit_.remaining(nick, seconds_now()) > 0;
        if (it != channels_.end()) {
            it->second.members.remove(nick); // a new stay: nothing carries over
            Member& member = it->second.members.add(nick);
            member.set_userhost(user_host(hostmask));
            member.since = (uint32_t)seconds_now();
            if (returning && it->second.split_away > 0) it->second.split_away--;
        }
    }
    note_seen(nick, hostmask, 'j', channel, "");
    for (const auto& act : guardian_.on_user_join(name_, channel, nick, hostmask, !returning && !reports)) {
        execute_action(act);
    }
    if (!returning) note_presence(channel, nick, hostmask, true);
    grant_modes(channel, nick, true);
    deliver_tells(nick, channel);
}

void IRCClient::on_kick(const std::string& channel, const std::string& victim, const std::string& by, const std::string& reason, bool me) {
    std::vector<std::string> leaving{victim};
    PsramVector<Member> kicked; // as they were, for the memory of timed voice
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it != channels_.end()) {
            if (me) {
                for (const auto& m : it->second.members.all()) leaving.push_back(m.nick);
                channels_.erase(it);
            } else {
                if (const Member* m = it->second.members.find(victim)) kicked.push_back(*m);
                it->second.members.remove(victim);
            }
        }
    }
    if (!same(by, victim)) note_voice(channel, kicked, true); // before their account is forgotten
    forget_if_gone(leaving);
    if (me) {
        const int delay = g_settings.get()->rejoin_delay;
        report(ReportKind::channel, "Kicked from " + channel + " by " + by + " (" + reason + "). Joining again in " + format_duration(delay) + ".");
        schedule_join(channel, delay);
    } else {
        note_seen(victim, "", 'k', channel, by);
    }
}

void IRCClient::on_quit_or_nick(bool quit, const std::string& nick, const std::string& hostmask, const std::string& detail, bool from_me) {
    std::vector<std::string> affected;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const bool split = quit && is_netsplit_reason(detail);
        const int64_t now = seconds_now();
        for (auto& entry : channels_) {
            ChannelState& state = entry.second;
            if (!(quit ? state.members.remove(nick) : state.members.rename(nick, detail))) continue;
            affected.push_back(state.name);
            if (!split) continue;
            if (now >= state.split_until) state.split_away = 0; // an earlier split, long over
            state.split_away++;
            state.split_until = now + NETSPLIT_MEMORY;
        }
        if (split) netsplit_.add(nick, now, now + NETSPLIT_MEMORY);
        if (!quit && from_me) current_nick_ = detail;
    }
    if (quit) security_mgr_.note_account(name_, nick, "", "");
    else security_mgr_.rename_account(name_, nick, detail);
    if (!quit && from_me) log("Now known as " + detail + ".");
    std::string shared;
    for (const auto& channel : affected) {
        if (!is_report_channel(channel)) {
            shared = channel;
            break;
        }
    }
    if (shared.empty()) return;
    note_seen(nick, hostmask, quit ? 'q' : 'n', shared, detail);
    if (quit) {
        // those lost in a netsplit did not choose to leave
        if (!is_netsplit_reason(detail)) {
            for (const auto& channel : affected) note_presence(channel, nick, hostmask, false);
        }
        return;
    }
    deliver_tells(detail, shared);
    if (from_me) return;
    const bool spare_ops = g_settings.get()->exempt_ops;
    std::vector<std::string> judged;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& channel : affected) {
            auto it = channels_.find(irc_lower(channel));
            if (it == channels_.end() || (!cfg_->report_channel.empty() && same(cfg_->report_channel, channel))) continue;
            const Member* member = it->second.members.find(detail);
            if (!(spare_ops && member && member->is_operator())) judged.push_back(channel);
        }
    }
    const std::string renamed = detail + hostmask.substr(std::min(hostmask.find('!'), hostmask.size()));
    for (const auto& act : guardian_.on_nick_change(name_, judged, detail, renamed)) execute_action(act);
}

void IRCClient::note_presence(const std::string& channel, const std::string& nick, const std::string& hostmask, bool joined) {
    if (is_report_channel(channel)) return;
    for (const auto& act : guardian_.on_presence(name_, channel, nick, hostmask, joined)) execute_action(act);
}

void IRCClient::on_names(const std::string& channel, const std::string& names) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(irc_lower(channel));
    if (it == channels_.end()) return;
    size_t at = 0;
    while (at < names.size()) {
        size_t end = names.find(' ', at);
        if (end == std::string::npos) end = names.size();
        const std::string token = names.substr(at, end - at);
        at = end + 1;

        std::string modes;
        size_t skip = 0;
        for (; skip < token.size(); skip++) {
            char mode = support_.mode_for_prefix(token[skip]);
            if (!mode) { // common prefixes, for servers that do not announce theirs
                const char* known = strchr("~&@%+", token[skip]);
                if (known) mode = "qaohv"[known - "~&@%+"];
            }
            if (!mode) break;
            modes += mode;
        }
        const size_t bang = token.find('!', skip);
        const std::string nick = token.substr(skip, bang == std::string::npos ? std::string::npos : bang - skip);
        if (nick.empty()) continue;
        Member& member = it->second.members.add(nick);
        member.modes[0] = '\0';
        for (char mode : modes) member.set_mode(mode, true);
        if (bang != std::string::npos) member.set_userhost(token.substr(bang + 1));
    }
}

void IRCClient::on_mode(const std::string& channel, const std::vector<std::string>& modes, const std::string& by, const std::string& by_hostmask) {
    const bool reports = is_report_channel(channel);
    int my_op = -1;
    bool key_changed = false, has_key = false;
    std::string key;
    PsramVector<Member> lost, voiced; // whose voice was taken away or given
    std::vector<ModeChange> changes;
    // a server changes modes when it mends a netsplit: nobody decided anything then
    const bool by_server = by.find('.') != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end()) return; // our own user modes, or a channel we are not in
        const std::string me = current_nick_.empty() ? cfg_->nick : current_nick_;
        changes = parse_modes(support_, modes);
        for (const auto& change : changes) {
            if (support_.prefix_modes.find(change.mode) != std::string::npos && !change.param.empty()) {
                Member& member = it->second.members.add(change.param);
                member.set_mode(change.mode, change.add);
                if (same(change.param, me)) {
                    my_op = member.is_operator() ? 1 : 0;
                    it->second.review = true; // what the bot can give has changed
                    it->second.grant_check = 0;
                } else if (!change.add) {
                    member.settled |= auto_bit(change.mode); // not given back during this stay
                    if (change.mode == 'v' && !by_server && !same(by, change.param)) lost.push_back(member);
                } else if (change.mode == 'v') {
                    voiced.push_back(member);
                }
            } else if (change.mode == 'm') {
                it->second.moderated = change.add;
            } else if (change.add && (change.mode == 'b' || change.mode == 'q') && !change.param.empty()) {
                const bool by_account = change.param.find_first_of("!@") == std::string::npos;
                for (Member& m : it->second.members.all()) {
                    if (!m.userhost[0] || same(m.nick, me)) continue;
                    const std::string hostmask = std::string(m.nick) + "!" + m.userhost;
                    const std::string account = by_account ? security_mgr_.identify(name_, m.nick, hostmask, channel).account : "";
                    if (!mask_restricts(change.param, hostmask, account)) continue;
                    m.settled |= AUTO_OP | AUTO_HALFOP | AUTO_VOICE;
                    if (!by_server) lost.push_back(m);
                }
            } else if (change.mode == 'k') {
                key_changed = true;
                has_key = change.add;
                key = change.param;
            }
        }
    }
    if (my_op >= 0) note_operator(channel, my_op == 1, by);
    if (reports && key_changed) check_report_key(has_key, key);
    if (!reports) keep_mode_lock(channel, by, by_hostmask, changes);
    note_voice(channel, voiced, false);
    note_voice(channel, lost, true);
}

void IRCClient::on_who(const std::string& channel, const std::string& userhost, const std::string& nick, const std::string& flags) {
    int my_op = -1;
    std::string pattern;
    bool collecting = false;
    const std::string me = current_nick();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it != channels_.end()) {
            Member& member = it->second.members.add(nick);
            member.set_userhost(userhost);
            for (char flag : flags) member.set_mode(support_.mode_for_prefix(flag), true);
            if (same(nick, me)) my_op = member.is_operator() ? 1 : 0;
        }
        collecting = masskick_.active && same(masskick_.channel, channel);
        if (collecting) pattern = masskick_.pattern;
    }
    if (my_op >= 0) note_operator(channel, my_op == 1, "");
    if (!collecting || same(nick, me) || !wildcard_match(pattern, nick)) return;
    // channel operators and the bot's own staff are never swept
    if (flags.find_first_of("~&@%") != std::string::npos) return;
    if (security_mgr_.has_permission(name_, nick, nick + "!" + userhost, "mod", channel)) return;

    const size_t limit = g_settings.get()->masskick_max;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (masskick_.victims.size() < limit) masskick_.victims.push_back(nick);
    else masskick_.skipped++;
}
