// Runs on a PC:  c++ -std=c++17 -I main test/wifi_policy_test.cpp -o /tmp/wifi_policy_test && /tmp/wifi_policy_test
#include <cassert>
#include <cstdio>
#include <vector>
#include "wifi_policy.h"

// Ticks `secs` times; returns the second (1-based) of every connect attempt
static std::vector<int> run(WifiPolicy& p, WifiPolicy::State& s, int secs, int* ap_opened = nullptr, int* ap_closed = nullptr) {
    std::vector<int> attempts;
    for (int t = 1; t <= secs; t++) {
        auto a = p.tick(s);
        if (a.connect) attempts.push_back(t);
        if (a.start_ap) { s.ap_on = true; if (ap_opened) *ap_opened = t; }
        if (a.stop_ap) { s.ap_on = false; if (ap_closed) *ap_closed = t; }
    }
    return attempts;
}

int main() {
    { // boot: tries at once, every 5 s, opens the AP after 30 s, then every 30 s, forever
        WifiPolicy p;
        WifiPolicy::State s{true, false, false, true, 0};
        int opened = 0;
        auto at = run(p, s, 3600, &opened);
        assert(at[0] == 1 && at[1] == 6 && opened == 30);
        assert(at.back() > 3600 - WifiPolicy::AP_RETRY_SECS); // still trying after an hour
        for (size_t i = 8; i < at.size(); i++) assert(at[i] - at[i - 1] == WifiPolicy::AP_RETRY_SECS);

        // someone on the setup AP is disturbed less, but the bot still keeps trying
        s.ap_clients = 1;
        at = run(p, s, 600);
        assert(at.size() >= 3);
        for (size_t i = 1; i < at.size(); i++) assert(at[i] - at[i - 1] == WifiPolicy::AP_BUSY_RETRY_SECS);

        // back online: no more attempts, the AP closes after the linger time
        s.connected = true;
        int closed = 0;
        at = run(p, s, 600, nullptr, &closed);
        assert(at.empty() && closed == WifiPolicy::AP_LINGER_SECS && !s.ap_on);

        // connection drops: retries immediately, AP only after 30 s again
        s.connected = false;
        s.ap_clients = 0;
        opened = 0;
        at = run(p, s, 40, &opened);
        assert(at[0] == 1 && at[1] == 6 && opened == 30);
    }
    { // short drop: reconnected before the AP is needed
        WifiPolicy p;
        WifiPolicy::State s{true, false, false, true, 0};
        int opened = 0;
        run(p, s, 20, &opened);
        s.connected = true;
        run(p, s, 5, &opened);
        s.connected = false;
        run(p, s, 29, &opened);
        assert(opened == 0);
    }
    { // fallback disabled: never opens the AP, keeps retrying
        WifiPolicy p;
        WifiPolicy::State s{true, false, false, false, 0};
        int opened = 0;
        auto at = run(p, s, 600, &opened);
        assert(opened == 0 && at.size() == 120);
    }
    { // nothing configured: AP at once even with fallback disabled, no attempts
        WifiPolicy p;
        WifiPolicy::State s{false, false, false, false, 0};
        int opened = 0;
        assert(run(p, s, 100, &opened).empty() && opened == 1);
    }
    { // new credentials while on the AP: tried on the next tick
        WifiPolicy p;
        WifiPolicy::State s{true, false, true, true, 1};
        run(p, s, 50);
        p.reset();
        assert(run(p, s, 1).size() == 1);
    }
    std::puts("wifi policy: all checks passed");
}
