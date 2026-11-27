#include "mdns.h"
#include "bot_settings.h"
#include "log_buffer.h"
#include "mdns_reply.h"
#include "wifi_manager.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <atomic>

namespace {

constexpr uint16_t PORT = 5353;
constexpr const char* GROUP = "224.0.0.251";
constexpr int64_t JOIN_AGAIN = 60; // seconds: a WiFi that came back may have forgotten the group

std::atomic<bool> s_running{false};

// Joins the group on the interface with this address, leaving it first if need be
bool join(int fd, const in_addr& own) {
    ip_mreq group = {};
    group.imr_multiaddr.s_addr = inet_addr(GROUP);
    group.imr_interface = own;
    setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &group, sizeof(group));
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) != 0) return false;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &own, sizeof(own));
    return true;
}

void serve(int fd) {
    std::string joined;
    int64_t joined_at = 0;
    for (;;) {
        const std::string name = g_settings.get()->mdns_name;
        if (!valid_host_name(name)) return;
        const std::string ip = g_wifi_mgr.get_ip();
        const int64_t now = esp_timer_get_time() / 1000000;
        in_addr own = {};
        const bool addressed = inet_aton(ip.c_str(), &own) && own.s_addr;
        if (addressed && (ip != joined || now - joined_at >= JOIN_AGAIN) && join(fd, own)) {
            if (ip != joined) g_log_buffer.log("Also reachable as http://" + name + ".local/");
            joined = ip;
            joined_at = now;
        }
        uint8_t query[512], answer[192];
        sockaddr_in from = {};
        socklen_t from_length = sizeof(from);
        const int n = recvfrom(fd, query, sizeof(query), 0, reinterpret_cast<sockaddr*>(&from), &from_length); // 2 s at most
        if (n <= 0 || !addressed) continue;
        const MdnsReply reply = mdns_reply(query, n, name, reinterpret_cast<const uint8_t*>(&own.s_addr), ntohs(from.sin_port) != PORT,
                                           answer, sizeof(answer));
        if (!reply.length) continue;
        sockaddr_in to = from;
        if (!reply.unicast) {
            to.sin_addr.s_addr = inet_addr(GROUP);
            to.sin_port = htons(PORT);
        }
        sendto(fd, answer, reply.length, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
}

void task(void*) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        const uint8_t hops = 255;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &hops, sizeof(hops));
        const timeval wait = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
        sockaddr_in here = {};
        here.sin_family = AF_INET;
        here.sin_port = htons(PORT);
        here.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, reinterpret_cast<sockaddr*>(&here), sizeof(here)) == 0) serve(fd);
        close(fd);
    }
    s_running = false;
    vTaskDelete(nullptr);
}

} // namespace

void mdns_tick() {
    if (!valid_host_name(g_settings.get()->mdns_name) || s_running.exchange(true)) return;
    if (xTaskCreatePinnedToCore(task, "mdns", 4096, nullptr, 2, nullptr, 1) != pdPASS) s_running = false;
}
