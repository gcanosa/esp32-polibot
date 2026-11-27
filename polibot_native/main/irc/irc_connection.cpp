// IRCClient: sockets, TLS, sending and receiving
#include "irc_client.h"
#include "irc_internal.h"
#include "health.h"
#include <esp_timer.h>
#include <esp_crt_bundle.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>

// Gives up after 10 s, or at once when the network is removed or changed (lwIP alone can wait for minutes)
bool IRCClient::connect_socket(int fd, const struct sockaddr* addr, socklen_t len) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int ready = 0;
    if (connect(fd, addr, len) == 0) {
        ready = 1;
    } else if (errno == EINPROGRESS) {
        for (int i = 0; i < 20 && ready == 0 && !stop_ && !reconnect_; i++) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
            ready = select(fd + 1, nullptr, &writable, nullptr, &tv);
        }
    }
    int error = 0;
    socklen_t error_len = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len);
    fcntl(fd, F_SETFL, flags);
    return ready > 0 && error == 0;
}

bool IRCClient::connect_once() {
    const auto shared = settings();
    const IrcSettings& cfg = *shared;
    log("Connecting to IRC " + cfg.host + ":" + std::to_string(cfg.port) + (cfg.ssl ? " (TLS)" : "") + "...");

    if (cfg.ssl) {
        esp_tls_cfg_t tls_cfg = {};
        tls_cfg.timeout_ms = 10000;
        tls_cfg.non_block = false;
        // Verified against the bundled root CAs. With "ssl_verify": false no option is set and
        // esp-tls skips verification (see sdkconfig.defaults) - only meant for self-signed servers.
        if (cfg.ssl_verify) tls_cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_tls_t* tls = esp_tls_init();
        if (!tls) {
            log("Failed to allocate esp_tls handle.");
            return false;
        }
        int ret = esp_tls_conn_new_sync(cfg.host.c_str(), cfg.host.length(), cfg.port, &tls_cfg, tls);
        if (ret != 1 || esp_tls_get_conn_sockfd(tls, &sock_fd_) != ESP_OK) {
            log("TLS handshake failed (code " + std::to_string(ret) + ")." +
                (cfg.ssl_verify ? " Self-signed certificate? Set Verify TLS Certificate to No." : ""));
            esp_tls_conn_destroy(tls);
            sock_fd_ = -1;
            return false;
        }
        tls_handle_ = tls;
    } else {
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        std::string port_str = std::to_string(cfg.port);
        if (getaddrinfo(cfg.host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            log("DNS lookup failed for " + cfg.host);
            return false;
        }

        int fd = socket(res->ai_family, res->ai_socktype, 0);
        if (fd < 0) {
            freeaddrinfo(res);
            return false;
        }

        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (!connect_socket(fd, res->ai_addr, res->ai_addrlen)) {
            if (!stop_ && !reconnect_) log("TCP connect failed to " + cfg.host);
            close(fd);
            freeaddrinfo(res);
            return false;
        }
        freeaddrinfo(res);
        sock_fd_ = fd;
    }

    // Removed or changed while connecting: never register with outdated settings
    if (stop_ || reconnect_) return false; // run_loop closes the socket

    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        outbox_.clear();
    }

    connected_ = true;
    last_activity_ = seconds_now();
    log("Socket connected! Handshaking as " + cfg.nick + "...");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_nick_ = cfg.nick;
    }

    // Send IRC registration handshake, asking first what the server offers: accounts, and SASL to sign
    // in before the welcome (NickServ is the fallback afterwards). A server without capabilities
    // simply welcomes.
    sasl_ok_ = false;
    nick_attempts_ = 0;
    server_caps_.clear();
    cap_requests_ = 0;
    cap_extended_join_ = whox_ = false;
    cap_account_tag_ = cap_account_notify_ = false;
    cap_deadline_ = last_activity_ + SASL_TIMEOUT_SECS;
    send_raw("CAP LS 302");
    send_raw("NICK " + cfg.nick);
    send_raw("USER " + cfg.ident + " 0 * :" + cfg.realname);

    return true;
}

void IRCClient::close_connection() {
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        connected_ = false;
        registered_ = false;
        if (tls_handle_) {
            esp_tls_conn_destroy(tls_handle_); // also closes the socket
            tls_handle_ = nullptr;
        } else if (sock_fd_ >= 0) {
            close(sock_fd_);
        }
        sock_fd_ = -1;
        outbox_.clear();
    }
    security_mgr_.forget_accounts(name_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    channels_.clear();
    pending_joins_.clear();
    netsplit_.clear();
    support_ = ServerSupport();
    bot_letter_.clear();
    bot_listed_ = bot_mode_set_ = bot_mode_refused_ = false;
    commands_.clear();
    cap_account_tag_ = cap_account_notify_ = false;
    report_keyed_ = false;
    report_greeted_ = false;
    report_key_set_at_ = -60;
    report_problem_.clear();
    masskick_ = MassKick();
    current_nick_.clear();
}

bool IRCClient::write_locked(const std::string& out) {
    size_t done = 0;
    while (done < out.size()) {
        ssize_t n = tls_handle_ ? esp_tls_conn_write(tls_handle_, out.data() + done, out.size() - done)
                                : send(sock_fd_, out.data() + done, out.size() - done, 0);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

bool IRCClient::send_raw(const std::string& line, bool in_order) {
    std::string out = line;
    // one call sends exactly one IRC command, whatever the input contains
    out.erase(std::remove_if(out.begin(), out.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; }), out.end());
    out = trim(out);
    if (out.empty()) return false;
    const std::string bare = out; // what waits in the queue, without the line break
    out += "\r\n";

    const auto limits = g_settings.get();
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!connected_) return false;
    LinePriority priority = line_priority(out, registered_);
    if (in_order && priority != LinePriority::urgent) priority = LinePriority::action;
    if (priority == LinePriority::urgent) return write_locked(out);
    switch (outbox_.offer(bare, priority, esp_timer_get_time() / 1000, limits->send_burst, limits->send_interval_ms)) {
        case Outbox::Offer::send_now: return write_locked(out);
        case Outbox::Offer::queued: return true;
        default: return false; // far behind: dropped rather than run out of memory
    }
}

void IRCClient::flush_queue() {
    const auto limits = g_settings.get();
    std::lock_guard<std::mutex> lock(io_mutex_);
    std::string line;
    while (connected_ && outbox_.next(line, esp_timer_get_time() / 1000, limits->send_burst, limits->send_interval_ms)) {
        if (!write_locked(line + "\r\n")) return;
    }
}

bool IRCClient::send_privmsg(const std::string& target, const std::string& message) {
    if (target.empty()) { // web console reply
        log("Reply: " + message);
        return true;
    }
    return send_raw("PRIVMSG " + target + " :" + message);
}

bool IRCClient::send_notice(const std::string& target, const std::string& message) {
    return send_raw("NOTICE " + target + " :" + message);
}

void IRCClient::read_loop() {
    std::string line_accum = "";
    char chunk[512];
    bool ping_sent = false;

    // Wakes once a second, so that stop and reconnect are honoured promptly; sooner while lines wait
    while (connected_ && !stop_ && !reconnect_) {
        watch_beat(watch_);
        tick();
        flush_queue();
        const int ping_timeout = settings()->ping_timeout;
        if (cap_deadline_ && seconds_now() > cap_deadline_) {
            const bool signing_in = !settings()->nickserv_pw.empty();
            if (signing_in) send_raw("AUTHENTICATE *");
            end_negotiation(signing_in ? "SASL did not answer. Will identify with NickServ." : "");
        }
        if (!(tls_handle_ && esp_tls_get_bytes_avail(tls_handle_) > 0)) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(sock_fd_, &readable);
            bool waiting;
            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                waiting = outbox_.pending();
            }
            struct timeval tv = { .tv_sec = waiting ? 0 : 1, .tv_usec = waiting ? 200000 : 0 };
            int ready = select(sock_fd_ + 1, &readable, nullptr, nullptr, &tv);
            if (ready < 0) break;
            if (ready == 0) {
                int64_t elapsed = seconds_now() - last_activity_;
                if (elapsed > ping_timeout) {
                    log("Ping timeout reached (" + std::to_string(elapsed) + "s). Dropping connection.");
                    break;
                }
                if (elapsed > ping_timeout / 2 && !ping_sent) {
                    ping_sent = send_raw("PING :keepalive");
                }
                continue;
            }
        }

        ssize_t n;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            n = tls_handle_ ? esp_tls_conn_read(tls_handle_, chunk, sizeof(chunk))
                            : recv(sock_fd_, chunk, sizeof(chunk), 0);
        }
        if (tls_handle_ && (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE)) continue;
        if (n <= 0) {
            log("Connection closed by the server.");
            break;
        }

        line_accum.append(chunk, n);
        last_activity_ = seconds_now();
        ping_sent = false;

        size_t pos;
        while ((pos = line_accum.find('\n')) != std::string::npos) {
            std::string line = line_accum.substr(0, pos);
            line_accum.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                process_raw_line(line);
            }
        }
        if (line_accum.size() > 8192) line_accum.clear(); // no IRC line is this long
    }
}
