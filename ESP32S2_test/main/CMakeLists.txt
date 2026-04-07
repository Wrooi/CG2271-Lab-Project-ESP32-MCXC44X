idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES
                            esp_wifi
                            esp_event
                            nvs_flash
                            esp_netif
                            esp_http_server
                            esp_http_client
                            esp-tls
                            esp_driver_uart
                            esp_driver_gpio
                            driver)