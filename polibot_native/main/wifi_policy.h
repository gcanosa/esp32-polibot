#pragma once

// What the WiFi radio should do, decided once a second. No ESP-IDF here, so it can be tested on a PC
// (see test/wifi_policy_test.cpp).
class WifiPolicy {
public:
    static constexpr int RETRY_SECS = 5;           // station only
    static constexpr int AP_RETRY_SECS = 30;       // joining moves the radio between channels: be gentle on an open setup AP
    static constexpr int AP_BUSY_RETRY_SECS = 180; // someone is using the setup AP
    static constexpr int AP_AFTER_SECS = 30;       // offline this long: open the setup AP
    static constexpr int AP_LINGER_SECS = 120;     // back online: keep it open this long, so the new address can be read

    struct State {
        bool has_ssid;
        bool connected;
        bool ap_on;
        bool ap_allowed; // the "ap_fallback" setting
        int ap_clients;
    };
    struct Action {
        bool connect = false;
        bool start_ap = false;
        bool stop_ap = false;
    };

    // After a change of network: try it on the next tick
    void reset() {
        offline_ = online_ = 0;
        since_attempt_ = LONG_AGO;
    }

    Action tick(const State& s) {
        Action a;
        if (s.connected) {
            offline_ = 0;
            since_attempt_ = LONG_AGO;
            if (s.ap_on && ++online_ >= AP_LINGER_SECS) a.stop_ap = true;
            return a;
        }
        online_ = 0;
        if (offline_ < AP_AFTER_SECS) offline_++;
        // with no network to join, the access point is the only way in, whatever ap_fallback says
        if (!s.ap_on && (!s.has_ssid || (s.ap_allowed && offline_ >= AP_AFTER_SECS))) a.start_ap = true;
        const int interval = !s.ap_on ? RETRY_SECS : s.ap_clients > 0 ? AP_BUSY_RETRY_SECS : AP_RETRY_SECS;
        if (s.has_ssid && ++since_attempt_ >= interval) {
            a.connect = true;
            since_attempt_ = 0;
        }
        return a;
    }

private:
    static constexpr int LONG_AGO = 1 << 20;
    int offline_ = 0;
    int online_ = 0;
    int since_attempt_ = LONG_AGO;
};
