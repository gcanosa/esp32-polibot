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
        "update_checker.cpp"
        "alerts.cpp"
        "mdns.cpp"
        # logic: no ESP-IDF, tested on a PC
        "logic/access_list.cpp"
        "logic/account_book.cpp"
        "logic/auto_modes.cpp"
        "logic/crash_log.cpp"
        "logic/irc_masks.cpp"
        "logic/irc_modes.cpp"
        "logic/irc_text.cpp"
        "logic/mdns_reply.cpp"
        "logic/mode_lock.cpp"
        "logic/rate_limits.cpp"
        "logic/members.cpp"
        "logic/outbox.cpp"
        "logic/proxy.cpp"
        "logic/releases.cpp"
        "logic/spam.cpp"
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
        "irc/irc_locks.cpp"
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
        "irc/commands_locks.cpp"
        "irc/commands_moderation.cpp"
        "irc/commands_owner.cpp"
        "irc/commands_recall.cpp"
        # web panel
        "web_api/web_server.cpp"
        "web_api/api_access.cpp"
        "web_api/api_action.cpp"
        "web_api/api_backup.cpp"
        "web_api/api_channel.cpp"
        "web_api/api_config.cpp"
        "web_api/api_ota.cpp"
        "web_api/api_session.cpp"
        "web_api/api_sessions.cpp"
        "web_api/api_status.cpp"
        "web_api/api_update.cpp"
        "web_api/api_users.cpp"
        "web_api/config_checks.cpp"
    INCLUDE_DIRS "." "logic" "memory" "irc" "web_api"
    EMBED_TXTFILES "update_key.pem"
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

# The panel's text files are stored, and sent, packed with gzip
set(web_text index.html home.html ui.css panel.css theme.js panel_core.js panel_irc.js panel_rules.js panel_users.js
    panel_system.js panel_logs.js panel_wifi.js panel_access.js panel_update.js panel_channel.js panel_sessions.js)
idf_build_get_property(python PYTHON)
foreach(name ${web_text})
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/web/${name}")
    set(packed "${CMAKE_CURRENT_BINARY_DIR}/${name}.gz")
    add_custom_command(OUTPUT "${packed}"
        COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/pack_web.py" "${source}" "${packed}"
        DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/pack_web.py"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${packed}" BINARY)
endforeach()
