idf_component_register(
    SRCS "cdc_acm_vcp_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        usb
        esp_wifi
        esp_event
        esp_netif
        lwip 
        nvs_flash
    )
