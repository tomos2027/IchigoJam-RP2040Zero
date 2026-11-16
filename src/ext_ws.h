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

//WS2812B-2020以降のリセット時間は>280µsのものがあるためマージンを取る
#define WS_RESET_US 500

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
    //SIO出力がHIGHになっている場合 (OUT n,1/LED 1後等) も、PIO切替前に確実にLOWへ
    //SIOラッチ操作はパッドに直接影響しないので、PIO機能中でも安全
    gpio_put(pin, false);
    gpio_set_dir(pin, GPIO_OUT);

    pio_sm_set_enabled(ws_pio, ws_pio_sm, false);
    pio_sm_clear_fifos(ws_pio, ws_pio_sm);
    pio_sm_restart(ws_pio, ws_pio_sm);
    ws2812_program_init(ws_pio, ws_pio_sm, ws_pio_offset, pin, 800000.0f, false);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);//5Vストリップ向けに信号マージンを確保
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    sleep_us(WS_RESET_US); //リセット信号 (WS2812B-2020世代のTreset>280µsにマージン)
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
    sleep_us(WS_RESET_US); //リセット信号
    //repeat分を1つの割込み禁止区間で連続送信 (フレーム間にリセットが入らない)
    //最悪でも119LED×3×24×1.26µs≈11ms/1repeatで、NTSC表示はDMAで維持される
    int save = save_and_disable_interrupts();
    for (int i = 0; i < repeat; i++) {
        for (int j = 0; j < nled * 3; j++) {//1つのLEDにつき、3つの変数が必要
            uint16 v = *basic_getArrayPtr(j); //拡張配列変数対応 (4K版)
            for (int k = 7; k >= 0; k--) {
                bool bit = v & (0x01 << k);
                wsled_send_one_signal_1575(pin, bit);
            }
        }
    }
    restore_interrupts(save);
    sleep_us(WS_RESET_US); //終端のリセット (ラッチ)
}

//基板内蔵WS2812 (GP16) のピン番号 (WS.LED 0 / 起動・診断表示で使用)
static uint8 led_builtin_pin = 16;

//WS.LED port,n[,repeat] (4K版: ポート選択式+拡張配列変数対応)
//port: 0=基板内蔵WS2812(GP16)、1-6=OUT1-6、7=外部WS2812(GP15)、8=LED(GP2)
//n: LED数、repeat: 送信回数。色データは配列変数[0]〜[n*3-1]から読み込む (1LEDにつき3変数)
static void __noinline ws_out(int port, int nled, int repeat) {
    if (port < 0 || port > 8 || nled < 1 || repeat < 1) {
        return;
    }
    if (nled * 3 > IJB_SIZEOF_ARRAY_MAX) { //配列変数の範囲内に収める (最大119LED)
        nled = IJB_SIZEOF_ARRAY_MAX / 3;
    }
    // 0=基板内蔵WS2812(GP16)、7=外部WS2812(GP15)、8=LED(GP2)、1-6=OUT1-6
    uint8 pin = (port == 0) ? led_builtin_pin : ((port == 7) ? WS2812_PIN : ((port == 8) ? LED : out_pins[port - 1]));
    if (sd_pin_reserved(pin)) {
        return;//SD動作中の共有ピン (OUT1/3/4) はWS2812出力禁止
    }
    if (!ws_pio_init()) { //PIOを確保できない場合のみ従来方式
        ws_bitbang_out(pin, nled, repeat);
        return;
    }

    ws_pio_configure(pin);//この中でリセットを出す
    //repeat回数分、リセットを挟まず連続送信する (公式IchigoJam仕様:
    //n個分のパターンが後続LEDへ流れて、テープ長分タイル状に繰り返される)
    for (int i = 0; i < repeat; i++) {
        for (int j = 0; j < nled * 3; j += 3) {//1つのLEDにつき、3つの変数が必要
            uint8 g = *basic_getArrayPtr(j);
            uint8 r = *basic_getArrayPtr(j + 1);
            uint8 b = *basic_getArrayPtr(j + 2);
            uint32_t v = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
            pio_sm_put_blocking(ws_pio, ws_pio_sm, v); //GRB順、下位8bitは無視される
        }
    }
    while (!pio_sm_is_tx_fifo_empty(ws_pio, ws_pio_sm)) {
        tight_loop_contents();
    }
    sleep_us(WS_RESET_US + 30); //最後の1ワード(約30us) + リセット(WS_RESET_US) でラッチ
    ws_pio_release(pin);
}

//===== 基板内蔵WS2812 (GP16) — 起動表示・診断表示用 =====
//LEDコマンド/OUT7はGP2を制御 (io.h)。WS.LED 7はGP15(外部WS2812)、WS.LED 8はGP2、WS.LED 0はGP16
//GP16は本関数群から制御される: 起動時の5回緑点滅、エラー時の高速赤点滅

// 指定GRB色で1LED送信 (bitbang、割込み禁止区間あり)
static void __noinline __not_in_flash_func(led_builtin_color) (uint32_t grb) {
    uint8 pin = led_builtin_pin;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    gpio_put(pin, false);
    sleep_us(300); //リセット信号
    int save = save_and_disable_interrupts();
    for (int k = 23; k >= 0; k--) {
        wsled_send_one_signal_1575(pin, (grb >> k) & 0x01u);
    }
    restore_interrupts(save);
}

// 緑で点灯/消灯
static void led_builtin_green(bool on) {
    led_builtin_color(on ? 0x400000u : 0x000000u); // GRB順: G=0x40
}

// 赤で点灯/消灯
void led_builtin_red(bool on) {
    led_builtin_color(on ? 0x004000u : 0x000000u); // GRB順: R=0x40
}

// 基板内蔵WS2812を制御 (旧互換、緑)
void led_builtin(bool on) {
    led_builtin_green(on);
}

// 起動時: 緑を5回点滅 (約500ms ON / 500ms OFF、計約5秒) して消灯
void led_builtin_boot_blink(void) {
    for (int i = 0; i < 5; i++) {
        led_builtin_green(true);
        sleep_ms(500);
        led_builtin_green(false);
        sleep_ms(500);
    }
}

#endif
