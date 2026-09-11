idf_component_register(
    SRCS "main.c"
         "ble_hid.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
    INCLUDE_DIRS "."
    REQUIRES bsp driver esp_driver_usb_serial_jtag
    PRIV_REQUIRES bt nvs_flash esp_hid
)

