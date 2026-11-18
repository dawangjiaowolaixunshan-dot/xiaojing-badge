idf_component_register(
    SRCS "app_main.c" "settings.c" "net.c" "console.c" "poems.c" "ui.c" "ui_poem.c" "ui_chat.c"
         "mic.c" "asr.c" "chat.c" "history.c" "tts.c" "whale_img.c" "pmu.c" "prefs.c" "ui_home.c" "pet.c" "weather.c" "voices.c" "power.c" "care.c" "recite.c" "pinyin_table.c" "ui_recite.c" "wifi_prov.c" "words.c" "ui_words.c" "ui_math.c" "mathq.c" "vp.c" "mfcc.c" "ui_settings.c" "fonts/font_clock_60.c" "fonts/font_poem_32.c" "fonts/font_poem_44.c" "fonts/font_text_22.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "data/poems.txt" "data/words.txt"
    REQUIRES spiffs esp_http_server nvs_flash esp_wifi esp_netif esp_event esp_http_client esp-tls esp_timer
             esp_driver_usb_serial_jtag esp_driver_gpio)
