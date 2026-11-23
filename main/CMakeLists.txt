idf_component_register(
    SRCS "main.c" "usb_probe.c" "fake_disk.c" "store.c" "cli.c" "report.c" "led.c" "webread.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash mdns
)

# TinyUSB is built as its own component; give it this project's tusb_config.h.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE ".")
