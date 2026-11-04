# The Seeed PlatformIO package installs its board definitions into Zephyr.
# Do not add the platform board tree as another BOARD_ROOT: Zephyr 4.2.1
# already contains xiao_nrf54l15 and rejects duplicate board definitions.

if(NOT DEFINED DTC_OVERLAY_FILE)
	set(DTC_OVERLAY_FILE
		"${CMAKE_CURRENT_LIST_DIR}/boards/xiao_nrf54l15_nrf54l15_cpuapp.overlay"
	)
endif()

if(DEFINED ENV{MORSE_ZEPHYR_MODULE})
	set(MORSE_ZEPHYR_MODULE "$ENV{MORSE_ZEPHYR_MODULE}")
	list(APPEND ZEPHYR_EXTRA_MODULES "${MORSE_ZEPHYR_MODULE}")
else()
	set(MORSE_ZEPHYR_MODULE "${CMAKE_CURRENT_LIST_DIR}/../platforms/mm-iot-zephyr")
	if(EXISTS "${MORSE_ZEPHYR_MODULE}/CMakeLists.txt")
		list(APPEND ZEPHYR_EXTRA_MODULES "${MORSE_ZEPHYR_MODULE}")
	endif()
endif()

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(offgrid-tracker-nrf54)

if(CONFIG_OFFGRID_NRF54_SENSOR_PS_PROBE)
	zephyr_include_directories("${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-sdk/framework/morselib/src")
endif()

zephyr_include_directories("${CMAKE_CURRENT_LIST_DIR}")
zephyr_include_directories(
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/include"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/src"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/src/mesh"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/src/mesh/generated"
	"${CMAKE_CURRENT_LIST_DIR}/../shared"
	"${CMAKE_CURRENT_LIST_DIR}/../shared/nanopb"
)
zephyr_include_directories(
	"$ENV{EDGEZ_HALOW_MESH_SOURCE_ROOT}/include"
	"${MORSE_ZEPHYR_MODULE}/submodules/edgez-rf-api/include"
	"${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-sdk/framework/src/mmregdb"
)
target_compile_definitions(app PRIVATE PB_ENABLE_MALLOC=1)

if(CONFIG_WIFI_MORSEMICRO)
	target_link_libraries(app PRIVATE libmorse_interface)
	zephyr_include_directories("${MORSE_ZEPHYR_MODULE}/drivers/wifi/morsemicro")
endif()

if(CONFIG_WIFI_MORSEMICRO_EDGEZ_JOIN_ONLY)
	target_compile_definitions(app PRIVATE EDGEZ_DIAGNOSTICS_ENABLED=1)
	target_link_libraries(app PRIVATE
		edgez_umac_core edgez_mesh_security
		edgez_rf_morsemicro libmorse
	)
	zephyr_include_directories(
		"${MORSE_ZEPHYR_MODULE}/components/morsemicro/libmorse_interface/include"
		"${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-edgez/include"
		"${MORSE_ZEPHYR_MODULE}/submodules/edgez-rf-api/include"
		"${MORSE_ZEPHYR_MODULE}/submodules/edgez-rf-morsemicro/include"
		"${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-sdk/framework/morselib/include"
	)
endif()

# PlatformIO compiles nrf54/src recursively. nanopb lives outside that source
# tree so ESP-IDF and Zephyr can share one runtime copy.
target_sources(app PRIVATE
	"${CMAKE_CURRENT_LIST_DIR}/../shared/nanopb/pb_common.c"
	"${CMAKE_CURRENT_LIST_DIR}/../shared/nanopb/pb_decode.c"
	"${CMAKE_CURRENT_LIST_DIR}/../shared/nanopb/pb_encode.c"
	"${MORSE_ZEPHYR_MODULE}/submodules/edgez-rf-morsemicro/src/edgez_morse_channel.c"
)
