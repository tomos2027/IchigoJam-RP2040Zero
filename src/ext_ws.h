//LED(WS2812B)の仕様は https://akizukidenshi.com/goodsaffix/WS2812B_20200225.pdf 参照

#ifndef __EXT_WS_H__
#define __EXT_WS_H__
#include "all_includes.h"

//1クロックの時間 (NTSC:157.5MHz=約6.3ns)
//100nsあたりのNOP数: 157.5MHz→16個
#define NOP4_1575 __asm("nop");__asm("nop");__asm("nop");__asm("nop");
#define SLEEP_100NS_NTSC NOP4_1575 NOP4_1575 NOP4_1575 NOP4_1575 //16 nop=約102ns (157.5MHz)

static uint8 ws_pin;

//1ビット送信 (bit=1: High 600ns + Low 600ns、bit=0: High 300ns + Low 600ns)
void __not_in_flash_func(wsled_send_one_signal_1575) (bool bit) { //NTSCモード (157.5MHz)
    gpio_put(ws_pin, true);
    if (bit) {
        SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
    }
    SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
    gpio_put(ws_pin, false);
    SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC SLEEP_100NS_NTSC
}

//WS.LED port,n[,repeat] (4K版: ポート選択式+拡張配列変数対応)
//port: 1-7 (OUT1-6、7=LED/GP16=基板内蔵WS2812)、n: LED数、repeat: 送信回数
//色データは配列変数[0]〜[n*3-1]から読み込む (1LEDにつき3変数、[102]-[357]の拡張領域に対応し最大119LED)
void __not_in_flash_func(ws_out) (int port, int nled, int repeat) {
    if (port < 1 || port > 7 || nled < 1 || repeat < 1) {
        return;
    }
    if (nled * 3 > IJB_SIZEOF_ARRAY_MAX) { //配列変数の範囲内に収める (最大119LED)
        nled = IJB_SIZEOF_ARRAY_MAX / 3;
    }
    ws_pin = out_pins[port - 1];
    if (sd_pin_reserved(ws_pin)) {
        return;//SD動作中の共有ピン (OUT1/3/4) はWS2812出力禁止
    }
    gpio_init(ws_pin);
    gpio_set_dir(ws_pin, GPIO_OUT);
    gpio_put(ws_pin, false);
    sleep_us(300); //リセット信号
    int save = save_and_disable_interrupts();
    for (int i = 0; i < repeat; i++) {
        for (int j = 0; j < nled * 3; j++) {//1つのLEDにつき、3つの変数が必要
            uint16 v = *basic_getArrayPtr(j); //拡張配列変数対応 (4K版)
            for (int k = 7; k >= 0; k--) {
                bool bit = v & (0x01 << k);
                wsled_send_one_signal_1575(bit);
            }
        }
    }
    restore_interrupts(save);
}

//基板内蔵WS2812 (GP16) をLEDコマンド・診断表示に使う (Waveshare RP2040-Zero)
//LED 1=点灯 (50%輝度)、LED 0=消灯。WS2812はラッチ型のため、状態が変わったときだけ送信する
static bool led_builtin_last = false;
static bool led_builtin_started = false;
void led_builtin(bool on) {
    if (led_builtin_started && on == led_builtin_last) {
        return;
    }
    led_builtin_last = on;
    led_builtin_started = true;
    ws_pin = LED;
    gpio_init(ws_pin);
    gpio_set_dir(ws_pin, GPIO_OUT);
    gpio_put(ws_pin, false);
    sleep_us(300); //リセット信号
    uint32_t v = on ? 0x400000u : 0x000000u; // 赤 (GRB順: G=0x00, R=0x40, B=0x00)
    int save = save_and_disable_interrupts();
    for (int k = 23; k >= 0; k--) {
        wsled_send_one_signal_1575((v >> k) & 0x01u);
    }
    restore_interrupts(save);
}

#endif
