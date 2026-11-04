if(CONFIG_DAB_RAW_RF_TEST)
    set(component_sources "raw_rf_main.c")
    set(component_private_dependencies
        halow
        esp_timer
        usb
    )
    if(CONFIG_DAB_WIFI_EXTENDER)
        list(APPEND component_sources
            "wifi_extender.c"
            "extender_dns.c"
            "edgez_mesh_internet.c"
            "edgez_mesh_iperf.c"
            "edgez_mesh_alfred.c"
            "edgez_mesh_gateway_policy.c"
            "edgez_mesh_ndp.c")
        list(APPEND component_private_dependencies
            esp_event
            esp_netif
            esp_wifi
            nvs_flash)
        if(IDF_TARGET STREQUAL "esp32p4")
            list(APPEND component_private_dependencies espressif__esp_hosted)
        endif()
    endif()
else()
    if(CONFIG_OFFGRID_HT_HC33_HALOW_WAKE_TEST)
        set(component_sources
            "ht_hc33_halow_wake_test.c"
        )
    elseif(CONFIG_OFFGRID_HT_HC33_SENSOR)
        set(component_sources
            "ht_hc33_sensor_main.c"
            "edgez_esp_factory_data.c"
            "../nrf54/src/usb_control.pb.c"
        )
    else()
        set(component_sources
            "app_main.c"
        )
    endif()

    if(CONFIG_OFFGRID_MESH_PS_TEST)
        list(APPEND component_sources "mesh_ps_test.c")
    endif()

    if(CONFIG_HALOW_PAWR_ENABLE)
        list(APPEND component_sources "halow_pawr.c")
    endif()

    if(CONFIG_DAB_START_MQTT_RELAY)
        list(APPEND component_sources "halow_telemetry.c")
    endif()

    set(component_private_dependencies
        esp_event
        esp_netif
        halow
        esp_adc
        esp_app_format
        esp_driver_gpio
        esp_timer
    )

    if(CONFIG_OFFGRID_HT_HC33_SENSOR)
        list(APPEND component_private_dependencies
            esp_hw_support
            esp_driver_tsens
            json
            mbedtls
            nvs_flash
            spi_flash)
    endif()

    if(CONFIG_DAB_WIFI_EXTENDER)
        list(APPEND component_sources
            "wifi_extender.c"
            "extender_dns.c"
            "edgez_mesh_internet.c"
            "edgez_mesh_iperf.c"
            "edgez_mesh_alfred.c"
            "edgez_mesh_gateway_policy.c"
            "edgez_mesh_ndp.c")
    endif()

# ESP32-S3 uses its local BLE controller. ESP32-P4 uses the ESP32-C6
# controller on the Waveshare board through ESP-Hosted.
    if(NOT CONFIG_OFFGRID_HT_HC33_SENSOR AND CONFIG_HALOW_BLE_PROVISIONING AND
       (IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4"))
        list(APPEND component_sources "halow_provisioning.c")
        list(APPEND component_private_dependencies
            bt
            esp_wifi
            json
            network_provisioning
            nvs_flash
        )
    endif()

    if(NOT CONFIG_OFFGRID_HT_HC33_SENSOR AND CONFIG_HALOW_BLE_PROVISIONING AND
       (IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4"))
        list(APPEND component_sources
            "edgez_ble_control.c"
            "../nrf54/src/usb_control.pb.c")
    endif()

    # The provisioning reset button is shared by the ESP32-P4 gateway and the
    # ESP32-S3/HT-HC33 gateway build.  Keep it outside the P4-only block so a
    # provisioned HC33 can return to BLE provisioning without reflashing NVS.
    if(CONFIG_DAB_FACTORY_RESET_BUTTON)
        list(APPEND component_sources "factory_reset_button.c")
        list(APPEND component_private_dependencies nvs_flash)
    endif()

    if(IDF_TARGET STREQUAL "esp32p4")
        list(APPEND component_private_dependencies espressif__esp_hosted)
    endif()
endif()

list(APPEND component_private_dependencies nanopb)

idf_component_register(
    SRCS ${component_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../nrf54/src" "../shared"
    PRIV_REQUIRES ${component_private_dependencies}
)

if(IDF_TARGET STREQUAL "esp32s3")
    # Software reset can leave an MM6108 GPIO interrupt pending on ESP32-S3.
    # This is not specific to the power-save test: realtime BLE initialization
    # also starts HaLow after other peripherals are active. Replace the
    # upstream HAL init for every S3 image so the GPIO ISR service cannot start
    # while IRQ/BUSY still have stale status.
    get_target_property(shims_sources shims SOURCES)
    list(FILTER shims_sources EXCLUDE REGEX "mmhal_os\\.c$")
    set_property(TARGET shims PROPERTY SOURCES "${shims_sources}")
    target_sources(shims PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/mmhal_os_ps_override.c")
endif()

if(IDF_TARGET STREQUAL "esp32p4" AND NOT CONFIG_DAB_RAW_RF_TEST)
    # ESP-Hosted 1.4 starts SDIO unconditionally from a constructor. Keep the
    # component linked for realtime BLE control, but let app_main initialize
    # it before NimBLE so the C6 VHCI transport is ready first.
    idf_component_get_property(esp_hosted_lib espressif__esp_hosted COMPONENT_LIB)
    get_target_property(esp_hosted_sources ${esp_hosted_lib} SOURCES)
    list(FILTER esp_hosted_sources EXCLUDE REGEX "esp_hosted_host_init\\.c$")
    set_property(TARGET ${esp_hosted_lib} PROPERTY SOURCES "${esp_hosted_sources}")
endif()

# Factory HC33 audit uses SDK command definitions and RF-Morse diagnostics.
if(CONFIG_OFFGRID_HT_HC33_HALOW_WAKE_TEST)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../platforms/esp-halow/halow/components/mm-iot-sdk/framework/morselib/src"
        "${CMAKE_CURRENT_LIST_DIR}/../platforms/esp-halow/halow/components/mm-iot-sdk/framework/morselib/include"
        "${CMAKE_CURRENT_LIST_DIR}/../platforms/esp-halow/halow/components/edgez-rf-morsemicro/include")
endif()
