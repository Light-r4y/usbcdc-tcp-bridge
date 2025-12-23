idf_component_register(
    SRCS "cdc_acm_vcp_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        usb
        esp_netif
        esp_wifi
        nvs_flash
        driver
        freertos
        lwip
        esp_timer
        esp_event
        esp_system
    )
