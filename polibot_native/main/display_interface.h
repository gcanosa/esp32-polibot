#pragma once

#include <string>
#include <vector>
#include <cstdint>

class DisplayInterface {
public:
    virtual ~DisplayInterface() = default;

    virtual void init_screen() = 0;
    virtual void update_header(const std::string& title, const std::string& ip_addr, int wifi_rssi) = 0;
    virtual void update_irc_status(bool connected, const std::string& server, const std::string& nick, const std::vector<std::string>& channels) = 0;
    virtual void update_system_stats(uint32_t free_ram_kb, uint32_t uptime_sec) = 0;
    virtual void log_event(const std::string& text, const std::string& level = "info") = 0;
    virtual void show_alert(const std::string& title, const std::string& message) = 0;
};

class StubDisplay : public DisplayInterface {
public:
    StubDisplay();
    void init_screen() override;
    void update_header(const std::string& title, const std::string& ip_addr, int wifi_rssi) override;
    void update_irc_status(bool connected, const std::string& server, const std::string& nick, const std::vector<std::string>& channels) override;
    void update_system_stats(uint32_t free_ram_kb, uint32_t uptime_sec) override;
    void log_event(const std::string& text, const std::string& level = "info") override;
    void show_alert(const std::string& title, const std::string& message) override;
};

class DisplayManager {
public:
    explicit DisplayManager(DisplayInterface* driver = nullptr);
    void set_driver(DisplayInterface* driver);

    void update_header(const std::string& title, const std::string& ip_addr, int wifi_rssi);
    void update_irc_status(bool connected, const std::string& server, const std::string& nick, const std::vector<std::string>& channels);
    void update_system_stats(uint32_t free_ram_kb, uint32_t uptime_sec);
    void log_event(const std::string& text, const std::string& level = "info");
    void show_alert(const std::string& title, const std::string& message);

private:
    DisplayInterface* driver_;
    StubDisplay default_stub_;
};

extern DisplayManager g_display_mgr;
