# 2026-09-19: Register the initial application component as the base for staged Arduino-to-IDF migration.
idf_component_register(
    # 2026-09-19: Add native audio and heap diagnostics before migrating cloud clients and ESP-SR.
    SRCS
        "app_main.cpp"
        "asr_client.cpp"
        "audio.cpp"
        "command_console.cpp"
        "conversation.cpp"
        "device_ui.cpp"
        "latency_trace.cpp"
        "llm_client.cpp"
        "memory_diagnostics.cpp"
        "storage.cpp"
        "tts_client.cpp"
        "volc_speech_protocol.cpp"
        "wake_word.cpp"
        "wifi_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_rom
        esp_system
        esp_timer
        esp_wifi
        heap
        json
        lwip
        nvs_flash
        spiffs
        esp_websocket_client
        esp-sr
        led_strip
)
