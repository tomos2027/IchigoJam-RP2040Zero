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

# 60FPSタイマー割り込み内からtuh_task()を呼ぶため、TinyUSBのosal_task_delayは
# busy loop化パッチ (patches/tinyusb_osal_task_delay.patch) の適用が必須。
# 未適用のままビルドするとISR内sleep_msで異常動作するため、ここで検知して停止する。
set(TINYUSB_OSAL "${PICO_SDK_PATH}/lib/tinyusb/src/osal/osal_pico.h")
if(EXISTS "${TINYUSB_OSAL}")
    file(READ "${TINYUSB_OSAL}" TINYUSB_OSAL_CONTENT)
    string(FIND "${TINYUSB_OSAL_CONTENT}" "msec * 1000" TINYUSB_PATCH_POS)
    if(TINYUSB_PATCH_POS EQUAL -1)
        message(FATAL_ERROR
            "TinyUSBのosal_pico.hに patches/tinyusb_osal_task_delay.patch が適用されていません\n"
            "(60FPSタイマー割り込み内からtuh_task()を呼ぶためパッチ適用は必須です)\n"
            "適用方法: cd ${PICO_SDK_PATH}/lib/tinyusb && git apply ${CMAKE_CURRENT_LIST_DIR}/patches/tinyusb_osal_task_delay.patch")
    endif()
endif()

# 本機はNTSCコンポジット出力専用 (HDMI/DVI非対応のためPicoDVI等のDVIライブラリは不要)
# NTSCドライバは独自実装 (src/video/ntscv、MIT) — 旧ドライバは互換shim経由で利用
add_executable(IchigoJam_RP2040Zero
	src/main.c
	src/video/ntscv_core.c
	src/video/ntscv_palette.c
	src/video/ntscv_render.c
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
	pico_multicore
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

# 信号プリセット: 0=S_D(既定), 1=S_A(元ドライバ再現), 2=S_B(262p+EQ/SERR), 3=S_C(525i@157.5MHz)
# 切り分けビルド時に -DNTSCV_SIG_PRESET=N を指定する (未指定なら0)
if (NTSCV_SIG_PRESET)
    target_compile_definitions(IchigoJam_RP2040Zero PRIVATE NTSCV_SIG_PRESET=${NTSCV_SIG_PRESET})
endif()

# 映像調整用: -DNTSCV_BOOT_TESTPATTERN=1 (SMPTEカラーバー) / =2 (グリッド) で
# 起動時にNTSCテストパターンを表示する (通常ビルドでは無効)
if (NTSCV_BOOT_TESTPATTERN)
    target_compile_definitions(IchigoJam_RP2040Zero PRIVATE NTSCV_BOOT_TESTPATTERN=${NTSCV_BOOT_TESTPATTERN})
endif()

# 診断用: -DNTSCV_MERGE_DEBUG=1 でDMA合成の割込み合流回数 (ntscv_debug_events) を
# 画面右上に16進4桁で表示する (増え続ける=合成が間に合っていない。通常ビルドでは無効)
if (NTSCV_MERGE_DEBUG)
    target_compile_definitions(IchigoJam_RP2040Zero PRIVATE NTSCV_MERGE_DEBUG)
endif()

# 診断用: -DNTSCV_COLOR_GRAYTEST=1 でテキスト用パレット (0-255) の色差変調を除去し
# 各色を平均レベル一色にする。ドット欠けが「色差変調」由来かの切り分け用
# (バーのbar_pairはntscv_palette_calcを直接呼ぶため影響しない。通常ビルドでは無効)
if (NTSCV_COLOR_GRAYTEST)
    target_compile_definitions(IchigoJam_RP2040Zero PRIVATE NTSCV_COLOR_GRAYTEST)
endif()
