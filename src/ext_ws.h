//LED(WS2812B)の仕様は https://akizukidenshi.com/goodsaffix/WS2812B_20200225.pdf 参照

#ifndef __EXT_WS_H__
#define __EXT_WS_H__
#include "all_includes.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

//1クロックの時間 (NTSC:157.5MHz=約6.3ns)
//100nsあたりのNOP数: 157.5MHz→16個
#define NOP4_1575 __asm volatile("nop");__asm volatile("nop");__asm volatile("nop");__asm volatile("nop");
#define SLEEP_100NS_NTSC NOP4_1575 NOP4_1575 NOP4_1575 NOP4_1575 //16 nop=約102ns (157.5MHz)

//WS2812B (800kHz) の1ビット分のNOPグループ (1グループ=約102ns)
//規格: T0H 0.40us / T1H 0.80us / T0L 0.85us / T1L 0.45us (各±0.15us)
//関数呼び出し・ループのオーバーヘッドはLow側に加算されるため、Lowは短めに設定する
#define WS_HIGH_0 SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
#define WS_HIGH_1 SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC \
                  SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
#define WS_LOW_0  SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC \
                  SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
#define WS_LOW_1  SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC

//1ビット送信 (pinを引数化してハートビートとの共有グローバル競合を排除)
//bit=1: High 約810ns + Low 約450ns、bit=0: High 約400ns + Low 約800ns
static void __noinline __not_in_flash_func(wsled_send_one_signal_1575) (uint8 pin, bool bit) { //NTSCモード (157.5MHz)
    gpio_put(pin, true);
    if (bit) {
        WS_HIGH_1
    } else {
        WS_HIGH_0
    }
    gpio_put(pin, false);
    if (bit) {
        WS_LOW_1
    } else {
        WS_LOW_0
    }
}

//===== WS.LED送信 =====
//通常はPIOでWS2812波形を生成する。CPUは送信FIFOが埋まるまで待つだけで、
//NTSCのDMA割込み (DMA_IRQ_1) を止めないため画面が乱れない。
//PIOステートマシンを確保できなかった場合のみ、従来のビットバングにフォールバックする。

static PIO ws_pio;
static uint ws_pio_sm;
static uint ws_pio_offset;
static int ws_pio_state; //0=未初期化、1=使用可、-1=確保失敗

static bool ws_pio_init(void) {
    if (ws_pio_state != 0) {
        return ws_pio_state > 0;
    }
    if (!pio_claim_free_sm_and_add_program(&ws2812_program, &ws_pio, &ws_pio_sm, &ws_pio_offset)) {
        ws_pio_state = -1;
        return false;
    }
    ws_pio_state = 1;
    return true;
}

//ピンをPIOに接続し、SMを最初の命令で停止させてLowを保持したままリセット時間を作る
static void ws_pio_configure(uint8 pin) {
    pio_sm_set_enabled(ws_pio, ws_pio_sm, false);
    pio_sm_clear_fifos(ws_pio, ws_pio_sm);
    pio_sm_restart(ws_pio, ws_pio_sm);
    ws2812_program_init(ws_pio, ws_pio_sm, ws_pio_offset, pin, 800000.0f, false);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);//5Vストリップ向けに信号マージンを確保
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    sleep_us(300); //リセット信号
}

static void ws_pio_release(uint8 pin) {
    pio_sm_set_enabled(ws_pio, ws_pio_sm, false);
    pio_sm_clear_fifos(ws_pio, ws_pio_sm);
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, false);
}

//PIO確保失敗時のみ使う従来方式 (割込み禁止区間がある)
static void __noinline __not_in_flash_func(ws_bitbang_out) (uint8 pin, int nled, int repeat) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    gpio_put(pin, false);
    sleep_us(300); //リセット信号
    for (int i = 0; i < repeat; i++) {
        int save = save_and_disable_interrupts();
        for (int j = 0; j < nled * 3; j++) {//1つのLEDにつき、3つの変数が必要
            uint16 v = *basic_getArrayPtr(j); //拡張配列変数対応 (4K版)
            for (int k = 7; k >= 0; k--) {
                bool bit = v & (0x01 << k);
                wsled_send_one_signal_1575(pin, bit);
            }
        }
        restore_interrupts(save);
        sleep_us(300); //フレーム間/終端のリセット (repeat時もラッチされるようにする)
    }
}

//WS.LED port,n[,repeat] (4K版: ポート選択式+拡張配列変数対応)
//port: 1-7 (OUT1-6、7=LED/GP15=外部WS2812)、n: LED数、repeat: 送信回数
//色データは配列変数[0]〜[n*3-1]から読み込む (1LEDにつき3変数、[102]-[357]の拡張領域に対応し最大119LED)
static void __noinline ws_out(int port, int nled, int repeat) {
    if (port < 1 || port > 7 || nled < 1 || repeat < 1) {
        return;
    }
    if (nled * 3 > IJB_SIZEOF_ARRAY_MAX) { //配列変数の範囲内に収める (最大119LED)
        nled = IJB_SIZEOF_ARRAY_MAX / 3;
    }
    uint8 pin = out_pins[port - 1];
    if (sd_pin_reserved(pin)) {
        return;//SD動作中の共有ピン (OUT1/3/4) はWS2812出力禁止
    }
    if (!ws_pio_init()) { //PIOを確保できない場合のみ従来方式
        ws_bitbang_out(pin, nled, repeat);
        return;
    }

    ws_pio_configure(pin);//この中で300usのリセットを出す
    for (int i = 0; i < repeat; i++) {
        for (int j = 0; j < nled * 3; j += 3) {//1つのLEDにつき、3つの変数が必要
            uint8 g = *basic_getArrayPtr(j);
            uint8 r = *basic_getArrayPtr(j + 1);
            uint8 b = *basic_getArrayPtr(j + 2);
            uint32_t v = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
            pio_sm_put_blocking(ws_pio, ws_pio_sm, v); //GRB順、下位8bitは無視される
        }
        while (!pio_sm_is_tx_fifo_empty(ws_pio, ws_pio_sm)) {
            tight_loop_contents();
        }
        sleep_us(330); //最後の1ワード(約30us) + リセット(300us)
    }
    ws_pio_release(pin);
}

//===== 基板内蔵WS2812 (GP16) — ハートビート/診断表示専用 =====
//LEDコマンドはGP15を制御 (io.h)、WS.LED 7もGP15から出力する
//GP16は本関数 (led_builtin) のみから制御される: timer()のハートビート、エラー時の高速点滅

static uint8 led_builtin_pin = 16;// 基板内蔵WS2812 (GP16に固定)

// 基板内蔵WS2812を1色で送信 (緑)
static void __noinline __not_in_flash_func(led_builtin_send) (bool on) {
    uint8 pin = led_builtin_pin;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    gpio_put(pin, false);
    sleep_us(300); //リセット信号
    uint32_t v = on ? 0x400000u : 0x000000u; // 緑 (GRB順: G=0x40, R=0x00, B=0x00)
    int save = save_and_disable_interrupts();
    for (int k = 23; k >= 0; k--) {
        wsled_send_one_signal_1575(pin, (v >> k) & 0x01u);
    }
    restore_interrupts(save);
}

// 基板内蔵WS2812を制御 (ハートビート/診断用、LEDコマンドでは使用しない)
void led_builtin(bool on) {
    led_builtin_send(on);
}

#endif
