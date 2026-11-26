#include "display_interface.h"
#include <esp_log.h>

static const char* TAG = "DISPLAY";
DisplayManager g_display_mgr;

StubDisplay::StubDisplay() {
    ESP_LOGI(TAG, "StubDisplay initialized (Hardware TFT screen not attached).");
}

void StubDisplay::init_screen() {
    ESP_LOGI(TAG, "Display hook ready for ST7789/ILI9341/GC9A01 SPI DMA driver.");
}

void StubDisplay::update_header(const std::string& title, const std::string& ip_addr, int wifi_rssi) {
    // Stub
}

void StubDisplay::update_irc_status(bool connected, const std::string& server, const std::string& nick, const std::vector<std::string>& channels) {
    // Stub
}

void StubDisplay::update_system_stats(uint32_t free_ram_kb, uint32_t uptime_sec) {
    // Stub
}

void StubDisplay::log_event(const std::string& text, const std::string& level) {
    // Stub
}

void StubDisplay::show_alert(const std::string& title, const std::string& message) {
    ESP_LOGW(TAG, ">>> DISPLAY ALERT: %s - %s <<<", title.c_str(), message.c_str());
}

DisplayManager::DisplayManager(DisplayInterface* driver) : driver_(driver ? driver : &default_stub_) {
    driver_->init_screen();
}

void DisplayManager::set_driver(DisplayInterface* driver) {
    driver_ = driver ? driver : &default_stub_;
    driver_->init_screen();
}

void DisplayManager::update_header(const std::string& title, const std::string& ip_addr, int wifi_rssi) {
    driver_->update_header(title, ip_addr, wifi_rssi);
}

void DisplayManager::update_irc_status(bool connected, const std::string& server, const std::string& nick, const std::vector<std::string>& channels) {
    driver_->update_irc_status(connected, server, nick, channels);
}

void DisplayManager::update_system_stats(uint32_t free_ram_kb, uint32_t uptime_sec) {
    driver_->update_system_stats(free_ram_kb, uptime_sec);
}

void DisplayManager::log_event(const std::string& text, const std::string& level) {
    driver_->log_event(text, level);
}

void DisplayManager::show_alert(const std::string& title, const std::string& message) {
    driver_->show_alert(title, message);
}
