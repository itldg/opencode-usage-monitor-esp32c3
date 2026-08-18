idf_component_register(
    SRCS
        "app_main.c"
        "lcd_init.c"
        "touch_ft6336.c"
        "wifi_app.c"
        "usage_api.c"
        "usage_ui.c"
        "audio_player.c"
        "lv_font_zh16.c"
        "lv_font_zh40.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_lcd
        esp_driver_spi
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_timer
        esp_http_client
        esp-tls
        mbedtls
        json
        efuse
        lvgl
)

# 打包 audio/*.wav → audio.bin,烧录到 audio 分区
file(GLOB AUDIO_WAVS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/audio/*.wav")
set(AUDIO_BIN "${CMAKE_BINARY_DIR}/audio.bin")
add_custom_command(
    OUTPUT "${AUDIO_BIN}"
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/tools/pack_audio.py"
            "${CMAKE_CURRENT_LIST_DIR}/audio" "${AUDIO_BIN}"
    DEPENDS ${AUDIO_WAVS}
    VERBATIM)
add_custom_target(audio_bin ALL DEPENDS "${AUDIO_BIN}")
esptool_py_flash_to_partition(flash audio "${AUDIO_BIN}")
