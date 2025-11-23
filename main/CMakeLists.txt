idf_component_register(SRCS "main.c"
                            "led_mng.cpp"
                            "wifi_app.cpp"
                            "http_server.cpp"
                            "app_nvs.cpp"
                            "sntp_time_sync.cpp"
                            "${device_srcs}"
                            "${component_srcs}"
                            EMBED_FILES webpage/app.css webpage/app.js webpage/favicon.ico webpage/index.html webpage/jquery-3.3.1.min.js
                        INCLUDE_DIRS ".")