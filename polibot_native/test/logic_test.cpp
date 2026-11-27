// Runs on a PC, from the project folder:
// c++ -std=c++17 -I main/logic test/logic_test.cpp main/logic/*.cpp -o /tmp/logic_test && /tmp/logic_test
#include <cassert>
#include <cstdio>
#include "access_list.h"
#include "releases.h"
#include "account_book.h"
#include "auto_modes.h"
#include "floating_limit.h"
#include "irc_masks.h"
#include "irc_modes.h"
#include "irc_text.h"
#include "members.h"
#include "outbox.h"
#include "rate_limits.h"

static void masks() {
    assert(wildcard_match("*!*@*.example", "a!b@Host.EXAMPLE") && !wildcard_match("*!*@host", "a!b@other"));
    assert(mask_is_specific("*!*@bad.example") && mask_is_specific("bot*!*@*"));
    assert(!mask_is_specific("*!*@*") && !mask_is_specific("*!~*@*.*") && !mask_is_specific(""));
    assert(user_host("n!u@h") == "u@h" && user_host("server.example") == "");
    for (const char* trusted : {"*!*@home.example", "*!alice@home.example", "n!*@*.isp.example", "*@10.0.0.7", "*!*@2001:db8::1", "*!*@user/alice"}) assert(mask_names_host(trusted));
    // nicknames and idents are the sender's to choose
    for (const char* open : {"", "*!*@*", "*!alice@*", "alice!*@*", "alice!alice@*.*", "*!~alice@*", "*!*@?*", "*!*@*:*", "alice", "*alice*", "*!alice"}) assert(!mask_names_host(open));
    assert(ban_mask("troll", "~t@bad.example", "host") == "*!*@bad.example");
    assert(ban_mask("troll", "~t@bad.example", "userhost") == "*!~t@bad.example");
    assert(ban_mask("troll", "~t@bad.example", "nick") == "troll!*@*");
    assert(ban_mask("troll", "", "host") == "troll!*@*");           // nothing known about them
    assert(ban_mask("troll", "t@*", "host") == "troll!*@*");        // never a host that matches everybody
    assert(ban_mask("*@bad.example", "", "host") == "*!*@bad.example");
    assert(ban_mask("a!b@c", "x@y", "host") == "a!b@c" && ban_mask("$a:acc", "x@y", "host") == "$a:acc");
    assert(is_channel_name("#chan") && !is_channel_name("chan") && !is_channel_name("#a b") && !is_channel_name("#a,b"));
    assert(is_netsplit_reason("irc.a.net irc.b.net") && is_netsplit_reason("*.net *.split"));
    assert(!is_netsplit_reason("Quit: bye.") && !is_netsplit_reason("see http://a.example/x now.here") && !is_netsplit_reason("a.b c.d e.f"));
}

static void text() {
    int seconds = 0;
    assert(parse_duration("30m", seconds) && seconds == 1800);
    assert(parse_duration("2H", seconds) && seconds == 7200 && parse_duration("7d", seconds) && seconds == 604800);
    for (const char* bad : {"30", "m", "-5m", "0m", "5x", "999999999d", "400d", "spam"}) assert(!parse_duration(bad, seconds));
    assert(format_duration(0) == "0s" && format_duration(65) == "1m 5s" && format_duration(90000) == "1d 1h" && format_duration(3600) == "1h");
    assert(fill_template("+b m:{mask}", "#c", "*!*@h") == "+b m:*!*@h" && fill_template("OP {channel}", "#c") == "OP #c");
    assert(redact_command("!login hunter2") == "!login <hidden>");
    assert(redact_command("MODE #r +sk secret") == "MODE #r +sk <hidden>");
    assert(redact_command("PRIVMSG #c :hello") == "PRIVMSG #c :hello");
    assert(redact_command("!join #secret hunter2") == "!join #secret <hidden>" && redact_command("JOIN #secret hunter2") == "JOIN #secret <hidden>");
    assert(redact_command("!join #open") == "!join #open" && redact_command("please join us now") == "please join us now");
    assert(tag_value("time=2026;account=alice;msgid=1", "account") == "alice" && tag_value("account=bob", "account") == "bob");
    assert(tag_value("draft/account=x;accounts=y", "account") == "" && tag_value("account;x=1", "account") == "" && tag_value("", "account") == "");
    assert((parse_steps("warn, Quiet,nonsense,ban") == std::vector<std::string>{"warn", "quiet", "ban"}));
}

static void modes() {
    ServerSupport s;
    auto c = parse_modes(s, {"+o-v+l", "alice", "bob", "10"});
    assert(c.size() == 3 && c[0].add && c[0].param == "alice" && !c[1].add && c[1].param == "bob" && c[2].param == "10");
    c = parse_modes(s, {"-lk+s", "*"});
    assert(c.size() == 3 && c[0].param.empty() && c[1].mode == 'k' && c[1].param == "*" && c[2].param.empty());
    s.apply({"PREFIX=(qaohv)~&@%+", "CHANMODES=beI,k,fl,imnst"});
    assert(s.mode_for_prefix('~') == 'q' && s.mode_for_prefix('+') == 'v' && s.mode_for_prefix('x') == 0);
    assert(s.max_modes == 3);
    s.apply({"MODES=12"});
    assert(s.max_modes == 12);
    s.apply({"MODES"});
    s.apply({"MODES=500"});
    assert(s.max_modes == 20);
    c = parse_modes(s, {"+qo", "boss", "alice"});
    assert(c[0].param == "boss" && c[1].param == "alice");
}

static void members() {
    MemberList list;
    Member& a = list.add("Alice");
    a.set_mode('o', true);
    a.set_mode('v', true);
    a.set_mode(0, true);
    a.set_userhost("a@h");
    a.set_userhost("");
    assert(list.find("alice") && list.find("ALICE")->is_operator() && std::string(list.find("alice")->userhost) == "a@h");
    list.find("alice")->set_mode('o', false);
    assert(!list.find("alice")->is_operator() && list.find("alice")->has_mode('v'));
    list.add("bob");
    assert(list.add("ALICE").has_mode('v') && list.size() == 2);
    assert(list.rename("alice", "al") && !list.find("alice") && list.find("al"));
    assert(list.remove("al") && !list.remove("al") && list.size() == 1);
    const std::string longer(60, 'n');
    list.add(longer);
    assert(list.find(longer) && list.size() == 2);
    Member& fresh = list.add("new");
    assert(!fresh.settled && !fresh.since);
    for (char mode : std::string("qaohvY")) fresh.set_mode(mode, true); // every rank a server may have
    assert(fresh.has_mode('v') && fresh.has_mode('Y'));
}

static void limits() {
    TokenBucket bucket; // three at once, then one a second
    for (int i = 0; i < 3; i++) assert(bucket.take(1000, 3, 1000));
    assert(!bucket.take(1000, 3, 1000) && !bucket.take(1500, 3, 1000) && bucket.take(2000, 3, 1000) && !bucket.take(2000, 3, 1000));
    assert(bucket.take(2000, 3, 0)); // no pacing

    RateWindow window(8);
    for (int i = 1; i <= 3; i++) assert(window.hit("Host", 100, 10) == i);
    assert(window.hit("host", 109, 10) == 4 && window.hit("host", 115, 10) == 2);
    window.clear("HOST");
    assert(window.hit("host", 115, 10) == 1 && window.hit("host", 115, 0) == 1);
    for (int i = 0; i < 100; i++) window.hit("flood" + std::to_string(i), 200, 10); // far past its size
    assert(window.hit("flood99", 200, 10) == 2);

    ExpiringSet set(2);
    set.add("a", 0, 10);
    set.add("b", 0, 20);
    set.add("c", 0, 30); // "a" ends soonest and makes room
    assert(set.remaining("a", 0) == 0 && set.remaining("b", 5) == 15 && set.remaining("c", 30) == 0);
    set.clear();
    assert(set.remaining("b", 5) == 0);

    LoginGuard guard;
    assert(!guard.failed("web 1.2.3.4", 0, 3, 600, 60) && !guard.failed("web 1.2.3.4", 1, 3, 600, 60));
    assert(!guard.blocked_for("web 1.2.3.4", 1) && guard.failed("web 1.2.3.4", 2, 3, 600, 60));
    assert(guard.blocked_for("web 1.2.3.4", 2) == 60 && !guard.blocked_for("web 5.6.7.8", 2) && !guard.blocked_for("web 1.2.3.4", 62));

    CloneDetector clones;
    assert(clones.add("n/#c", "a@1", "a", "Buy cheap things now", 0, 3, 15).empty());
    assert(clones.add("n/#c", "a@1", "a", "buy  CHEAP things now", 1, 3, 15).empty()); // the same user again
    assert(clones.add("n/#other", "b@2", "b", "buy cheap things now", 1, 3, 15).empty());
    assert(clones.add("n/#c", "b@2", "b", "\x03" "04buy cheap things now", 2, 3, 15).empty());
    assert((clones.add("n/#c", "c@3", "c", "buy cheap things now", 3, 3, 15) == std::vector<std::string>{"a", "b", "c"}));
    assert((clones.add("n/#c", "d@4", "d", "buy cheap things now", 4, 3, 15) == std::vector<std::string>{"d"}));
    assert(clones.add("n/#c", "e@5", "e", "buy cheap things now", 40, 3, 15).empty()); // long after

    Strikes strikes;
    assert(strikes.add("x", 0, 60) == 1 && strikes.add("x", 10, 60) == 2 && strikes.add("y", 10, 60) == 1);
    assert(strikes.add("x", 100, 60) == 1);
    for (int i = 0; i < 200; i++) strikes.add("many" + std::to_string(i), 200, 60);
    assert(strikes.add("many199", 201, 60) == 2);
}

static void outbox() {
    assert(line_priority("PONG :x", true) == LinePriority::urgent && line_priority("NICK a", false) == LinePriority::urgent);
    assert(line_priority("NICK a", true) == LinePriority::action && line_priority("kick #c a", true) == LinePriority::action);
    assert(line_priority("PRIVMSG ChanServ :OP #c", true) == LinePriority::action && line_priority("PRIVMSG #c :hi", true) == LinePriority::talk);

    Outbox box; // one at once, then one a second
    assert(box.offer("PRIVMSG #c :1", LinePriority::talk, 0, 1, 1000) == Outbox::Offer::send_now);
    assert(box.offer("PRIVMSG #c :2", LinePriority::talk, 0, 1, 1000) == Outbox::Offer::queued);
    assert(box.offer("KICK #c a", LinePriority::action, 0, 1, 1000) == Outbox::Offer::queued);
    std::string line;
    assert(box.pending() && !box.next(line, 500, 1, 1000));
    assert(box.next(line, 1000, 1, 1000) && line == "KICK #c a");   // actions go first
    assert(box.next(line, 2000, 1, 1000) && line == "PRIVMSG #c :2" && !box.pending());
    box.offer("PRIVMSG #c :x", LinePriority::talk, 2000, 1, 1000);
    int queued = 0;
    while (box.offer(std::string(400, 'x'), LinePriority::talk, 2000, 1, 1000) == Outbox::Offer::queued) queued++;
    assert(queued > 5 && queued < 20); // a hard limit on what may wait
    box.clear();
    assert(!box.pending());

    LineQueue reports(32);
    reports.push_dropping_oldest("first report");
    reports.push_dropping_oldest("second report");
    reports.push_dropping_oldest("third report");
    assert(reports.pop(line) && line == "second report" && reports.pop(line) && line == "third report" && !reports.pop(line));
}

static void accounts() {
    AccountBook book;
    assert(book.note("net", "Alice", "a@h", "alice"));
    assert(!book.note("net", "alice", "A@H", "Alice")); // nothing new
    assert(book.find("net", "alice", "A@H") == "alice" && book.find("net", "alice", "x@h") == "" && book.find("other", "alice", "a@h") == "");
    assert(book.find("net", "alice", "") == "");
    book.note("net", "alice", "a@h", "alice2"); // replaced, not added
    assert(book.size() == 1 && book.find("net", "ALICE", "a@h") == "alice2");
    book.note("net", "bob", "b@h", "bob");
    book.rename("net", "alice", "bob"); // the previous holder of the nickname is gone
    assert(book.size() == 1 && book.find("net", "bob", "a@h") == "alice2" && book.find("net", "alice", "a@h") == "");
    book.rename("net", "bob", "BOB");
    assert(book.find("net", "bob", "a@h") == "alice2");
    book.note("net", "bob", "a@h", ""); // signed out
    assert(book.size() == 0);
    book.note("net", "long", "l@h", std::string(40, 'x')); // too long to keep: never matched by its beginning
    assert(book.size() == 0);
    for (size_t i = 0; i < AccountBook::CAPACITY + 10; i++) book.note("net", "n" + std::to_string(i), "u@h", "acc");
    assert(book.size() == AccountBook::CAPACITY && book.find("net", "n" + std::to_string(AccountBook::CAPACITY + 9), "u@h") == "acc");
    book.note("two", "z", "u@h", "z");
    book.forget_network("net");
    assert(book.size() == 1);
    book.forget("two", "z");
    assert(book.size() == 0);
}

static void access_lists() {
    // a nickname alone identifies nobody: a bare word is a services account, a mask must name a host
    assert(access_mask("*@h.example") == "*!*@h.example" && access_mask("alice") == "alice" && access_mask("a!b@c") == "a!b@c");
    for (const char* good : {"*!*@h.example", "al!*@*.isp.example", "*!*@2001:db8::1", "alice", "user:bob"}) assert(access_problem(good).empty());
    for (const char* bad : {"", "*!*@*", "alice!*@*", "*!alice@*", "*!*@*.*", "alice!ident", "a b", "a,b", "al*", "user:", "nick:x"}) assert(!access_problem(bad).empty());
    assert(!access_problem(std::string(32, 'a')).empty() && !access_problem("*!*@" + std::string(95, 'h')).empty());

    AccessList list;
    assert(list.add("net", "#a", 'o', "*!*@staff.example") && list.add("net", "#a", 'v', "carol") && list.add("net", "#a", 'h', "user:Bob"));
    assert(list.add("NET", "#A", 'v', "Carol") && list.all().size() == 3); // there already
    assert(!list.add("net", "#a", 'q', "x") && !list.add("net", "#a", 'o', "*!*@*") && !list.add("", "#a", 'o', "x") && !list.add("net", "", 'o', "x"));
    assert(!list.add("net", "#" + std::string(80, 'c'), 'o', "x")); // never cut short
    assert(list.any("net", "#A") && !list.any("net", "#b") && !list.any("other", "#a")); // one channel of one network
    assert(list.modes_for("net", "#a", "x!y@staff.example", "", "") == AUTO_OP);
    assert(list.modes_for("net", "#a", "carol!c@evil.example", "", "") == 0); // the nickname proves nothing
    assert(list.modes_for("net", "#a", "z!c@h", "CAROL", "") == AUTO_VOICE);
    assert(list.modes_for("net", "#a", "bob!b@h", "bob", "") == 0 && list.modes_for("net", "#a", "z!b@h", "", "bob") == AUTO_HALFOP);
    assert(list.modes_for("net", "#b", "x!y@staff.example", "carol", "bob") == 0 && list.modes_for("two", "#a", "x!y@staff.example", "carol", "bob") == 0);
    assert(list.modes_for("net", "#a", "x!y@staff.example", "carol", "bob") == (AUTO_OP | AUTO_HALFOP | AUTO_VOICE));
    assert((list.list("net", "#a", 'v') == std::vector<std::string>{"carol"}));
    assert(list.remove("net", "#a", 'v', "CAROL") && !list.remove("net", "#a", 'v', "carol") && !list.remove("net", "#a", 'o', "user:Bob"));
    for (size_t i = 0; i < AccessList::MAX + 5; i++) list.add("net", "#full", 'v', "n" + std::to_string(i));
    assert(list.all().size() == AccessList::MAX && !list.add("net", "#full", 'v', "more") && list.add("net", "#full", 'v', "n1"));
}

static void auto_modes() {
    assert(auto_bit('o') == AUTO_OP && auto_bit('q') == AUTO_OP && auto_bit('h') == AUTO_HALFOP && auto_bit('v') == AUTO_VOICE && !auto_bit('b'));
    assert((grant_lines({{'o', "a"}, {'v', "b"}, {'v', "c"}, {'h', "d"}}, 3) == std::vector<std::string>{"+ovv a b c", "+h d"}));
    assert(grant_lines({}, 3).empty() && grant_lines({{'v', "a"}}, 0) == std::vector<std::string>{"+v a"});
    const std::string who = "bob!~b@bad.example";
    assert(mask_restricts("*!*@bad.example", who, "") && mask_restricts("m:*!*@bad.example", who, ""));
    assert(mask_restricts("~q:bob!*@*", who, "") && mask_restricts("~t:5:*!*@bad.example", who, ""));
    assert(mask_restricts("$a:bobby", who, "Bobby") && mask_restricts("R:bobby", who, "bobby") && !mask_restricts("$a:bobby", who, ""));
    assert(!mask_restricts("*!*@good.example", who, "") && !mask_restricts("*!*@*", who, "") && !mask_restricts("bob", who, "bob") && !mask_restricts("$a", who, "a"));
    assert(mask_restricts("*!*@2001:db8::1", "a!b@2001:db8::1", ""));
    // the waiting time stands still: 40 s had passed when the pause began at 140, and still have after it
    assert(after_pause(100, 140, 500) == 460 && after_pause(140, 140, 500) == 500 && after_pause(300, 140, 500) == 500);
}

static void connect_commands() {
    assert(client_command("/mode -x", "Bot") == "MODE Bot -x" && client_command("  MODE +i-w ", "Bot") == "MODE Bot +i-w");
    assert(client_command("/umode -x", "Bot") == "MODE Bot -x" && client_command("/mode {nick} -x", "Bot") == "mode Bot -x");
    assert(client_command("/mode #c +m", "Bot") == "mode #c +m");
    assert(client_command("/msg NickServ IDENTIFY a pw", "Bot") == "PRIVMSG NickServ :IDENTIFY a pw");
    assert(client_command("notice #c :hi there", "Bot") == "NOTICE #c :hi there" && client_command("/msg x", "Bot").empty());
    assert(client_command("PRIVMSG #c :hi", "Bot") == "PRIVMSG #c :hi" && client_command("/quote OPER a b", "Bot") == "OPER a b");
    assert(client_command("/join #x key", "Bot") == "join #x key");
    assert(client_command("", "Bot").empty() && client_command(" / ", "Bot").empty() && client_command("/umode", "Bot").empty());
}

// A server restart seen from one channel: the numbers are the ones a guarded channel goes through
static void channel_limit() {
    const LimitRules rules{5, 30, 2, 300}; // margin, floor, slack, seconds before lowering
    FloatingLimit limit;
    assert(limit.next(0, rules, 0) == 0 && !limit.current);   // nobody counted yet: leave it alone
    assert(limit.next(22, rules, 0) == 30);                   // the floor
    assert(limit.next(25, rules, 30) == 0);                   // 25 + 5 still fits
    assert(limit.next(30, rules, 60) == 37);                  // a flood: raised one margin at a time
    assert(limit.next(22, rules, 90) == 0);                   // gone again: not lowered at once
    assert(limit.next(22, rules, 389) == 0);
    assert(limit.next(22, rules, 390) == 33);                 // then in steps
    assert(limit.next(22, rules, 420) == 30);
    assert(limit.next(22, rules, 450) == 0 && limit.current == 30);
    assert(limit.next(100, rules, 480) == 107);
    assert(limit.next(22, rules, 510) == 0);
    assert(limit.next(101, rules, 540) == 0);                 // busy again: the wait starts over
    assert(limit.next(22, rules, 570) == 0 && limit.next(22, rules, 840) == 0);
    int last = limit.current;
    for (int64_t at = 870; at < 1300; at += 30) {
        const int next = limit.next(22, rules, at);
        if (!next) continue;
        assert(last - next > rules.slack);                    // no MODE for a trifle
        last = next;
    }
    assert(limit.current == 31);                              // it rests within the slack of what is needed
}

static void releases() {
    assert(version_newer("0.2.0", "0.1.5") && version_newer("0.10.0", "0.9.9") && version_newer("1.0", "0.99.99"));
    assert(version_newer("0.2.0.1", "0.2.0"));
    assert(!version_newer("0.1.5", "0.1.5") && !version_newer("0.1.4", "0.1.5") && !version_newer("0.2", "0.2.0"));
    for (const char* bad : {"", "v0.3.0", "0.3.0-rc1", "0..3", ".3", "3.", "1.2.3.4.5", "9999999.0", " 9.0", "9.0 "}) {
        assert(!version_newer(bad, "0.1.5") && !version_newer("9.9.9", bad));
    }
    assert(release_file_name("polibot-0.2.0-update.bin") && release_file_name("a_b.bin"));
    for (const char* bad : {"", ".bin", "x.txt", "../x.bin", "a/b.bin", "a b.bin", "x.bin?y=1", ".hidden.bin", "http:x.bin", "a\\b.bin"}) {
        assert(!release_file_name(bad));
    }
    assert(!release_file_name(std::string(61, 'a') + ".bin") && release_file_name(std::string(60, 'a') + ".bin"));
    assert(sha256_text(std::string(64, 'a')) && sha256_text(std::string(64, '0')));
    assert(!sha256_text(std::string(63, 'a')) && !sha256_text(std::string(65, 'a')) && !sha256_text(std::string(64, 'g')) && !sha256_text(std::string(64, 'A')));
}

int main() {
    releases();
    channel_limit();
    access_lists();
    auto_modes();
    connect_commands();
    accounts();
    masks();
    text();
    modes();
    members();
    limits();
    outbox();
    std::puts("logic: all checks passed");
}
