idf_component_register(
    SRCS
        # device
        "main.cpp"
        "config_manager.cpp"
        "bot_settings.cpp"
        "security_manager.cpp"
        "wifi_manager.cpp"
        "wifi_scan.cpp"
        "ntp_manager.cpp"
        "display_interface.cpp"
        "log_buffer.cpp"
        "ota_manager.cpp"
        "status_led.cpp"
        "led_driver.cpp"
        "system_info.cpp"
        "health.cpp"
        # logic: no ESP-IDF, tested on a PC
        "logic/access_list.cpp"
        "logic/account_book.cpp"
        "logic/auto_modes.cpp"
        "logic/irc_masks.cpp"
        "logic/irc_modes.cpp"
        "logic/irc_text.cpp"
        "logic/rate_limits.cpp"
        "logic/members.cpp"
        "logic/outbox.cpp"
        # memory: seen, tell, bans
        "memory/ban_list.cpp"
        "memory/bot_memory.cpp"
        "memory/mailbox.cpp"
        "memory/record_file.cpp"
        "memory/seen_list.cpp"
        "memory/voice_memory.cpp"
        # irc
        "irc/irc_accounts.cpp"
        "irc/irc_automodes.cpp"
        "irc/irc_botmode.cpp"
        "irc/irc_channels.cpp"
        "irc/irc_client.cpp"
        "irc/irc_config.cpp"
        "irc/irc_connection.cpp"
        "irc/irc_dispatch.cpp"
        "irc/irc_manager.cpp"
        "irc/irc_operator.cpp"
        "irc/irc_protection.cpp"
        "irc/irc_recall.cpp"
        "irc/irc_registration.cpp"
        "irc/irc_reports.cpp"
        "irc/guardian.cpp"
        "irc/guardian_access.cpp"
        "irc/command_help.cpp"
        "irc/commands.cpp"
        "irc/commands_access.cpp"
        "irc/commands_akick.cpp"
        "irc/commands_channel.cpp"
        "irc/commands_info.cpp"
        "irc/commands_moderation.cpp"
        "irc/commands_owner.cpp"
        "irc/commands_recall.cpp"
        # web panel
        "web_api/web_server.cpp"
        "web_api/api_access.cpp"
        "web_api/api_action.cpp"
        "web_api/api_backup.cpp"
        "web_api/api_config.cpp"
        "web_api/api_ota.cpp"
        "web_api/api_session.cpp"
        "web_api/api_status.cpp"
        "web_api/api_users.cpp"
        "web_api/config_checks.cpp"
    INCLUDE_DIRS "." "logic" "memory" "irc" "web_api"
    EMBED_TXTFILES "web/index.html" "web/home.html" "web/panel.css" "web/panel_core.js" "web/panel_irc.js"
                   "web/panel_rules.js" "web/panel_users.js" "web/panel_system.js" "web/panel_logs.js" "web/panel_wifi.js" "web/panel_access.js" "web/ui.css" "web/theme.js"
    EMBED_FILES "web/logo.webp" "web/board.webp"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        nvs_flash
        mbedtls
        json
        spiffs
        driver
        esp_timer
        lwip
        esp-tls
        app_update
        esp_app_format
        esp_https_ota
        esp_http_client
        spi_flash
)
