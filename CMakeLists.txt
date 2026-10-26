cmake_minimum_required(VERSION 3.13)

# Waveshare RP2040-Zero (RP2040 / 2MB flash / 内蔵WS2812=GP16)
# pico-sdk 2.3.1以降に同梱のボード定義を使用する
# (UART0=GP0/1、2MBフラッシュ、内蔵WS2812=PICO_DEFAULT_WS2812_PIN=GP16)
# 注意: pico_sdk_import.cmakeはincludeした時点でボード/プラットフォームを確定させるため、
# このsetはincludeより前に置く必要がある (コマンドラインの-DPICO_BOARD=...での上書きも可能)
set(PICO_BOARD waveshare_rp2040_zero CACHE STRING "Board type")

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(IchigoJam_RP2040Zero)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# 本機はNTSCコンポジット出力専用 (HDMI/DVI非対応のためPicoDVI等のDVIライブラリは不要)
add_executable(IchigoJam_RP2040Zero
	src/main.c
	src/ntsc/rp2040_pwm_ntsc_textgraph.c
)

# WS.LED (WS2812B) はPIOで波形を生成する (CPUの長時間割込み禁止を避ける)
pico_generate_pio_header(IchigoJam_RP2040Zero ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

# SCRATCH_Y (core0スタック領域) を16KBに拡大 (複雑な式のスタック溢れ対策、IchigoJam_Pのmemmapと同じ)
pico_set_linker_script(IchigoJam_RP2040Zero ${CMAKE_CURRENT_LIST_DIR}/memmap_ichigojam_p.ld)

target_include_directories(IchigoJam_RP2040Zero PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
	# Make sure TinyUSB can find tusb_config.h
	${CMAKE_CURRENT_LIST_DIR}/src
	${CMAKE_CURRENT_LIST_DIR}/src/ntsc
)

# pico-sdk / pico-extras はリポジトリ直下にcloneして使う (README参照)
# pico-sdkのTinyUSBには patches/tinyusb_osal_task_delay.patch を適用すること (USBキーボード利用に必須)

target_link_libraries(IchigoJam_RP2040Zero PUBLIC
	pico_stdlib
	pico_util
	tinyusb_host
	tinyusb_board
	hardware_flash
	hardware_watchdog
	hardware_adc
	hardware_pwm
	hardware_dma
	hardware_i2c
	hardware_spi
	hardware_sleep
	hardware_pio
)

# pico_enable_stdio_usb(IchigoJam_RP2040Zero 0)
pico_enable_stdio_uart(IchigoJam_RP2040Zero 1)

# create map/bin/hex file etc.
pico_add_extra_outputs(IchigoJam_RP2040Zero)
