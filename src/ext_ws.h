//LED(WS2812B)の仕様は https://akizukidenshi.com/goodsaffix/WS2812B_20200225.pdf 参照

#ifndef __EXT_WS_H__
#define __EXT_WS_H__
#include "all_includes.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

//1クロックの時間 (NTSC:157.333333MHz=約6.36ns)
//100nsあたりのNOP数: 157.333333MHz→16個
#define NOP4_1575 __asm volatile("nop");__asm volatile("nop");__asm volatile("nop");__asm volatile("nop");
#define SLEEP_100NS_NTSC NOP4_1575 NOP4_1575 NOP4_1575 NOP4_1575 //16 nop=約102ns (157.333333MHz)

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
//invert=1で波形を完全反転 (逆相ドライバ/トランジスタ経由のWS2812用。アイドルもHiになる)
//パルス開始/終了レベルはbitに依存させない (正相=開始Hi/終了Lo、反転=開始Lo/終了Hi)。
//bitは各相の継続時間を選ぶだけ。レベルをbitに連動させるとbit=0のスロットがLo→Hiになり
//隣接ビットのHiパルスと連結してWS2812のデコードが破綻する (さらにラインがHiのまま
//放置されてリセット期間が取れなくなる)
//bit=1: High 約810ns + Low 約450ns、bit=0: High 約400ns + Low 約800ns (invert=0時)
static void __noinline __not_in_flash_func(wsled_send_one_signal_1575) (uint8 pin, bool bit, bool invert) { //NTSCモード (157.333333MHz)
    gpio_put(pin, !invert);
    if (bit) {
        WS_HIGH_1
    } else {
        WS_HIGH_0
    }
    gpio_put(pin, invert);
    if (bit) {
        WS_LOW_1
    } else {
        WS_LOW_0
    }
}

//WS2812B-2020以降のリセット時間は>280µsのものがあるためマージンを取る
#define WS_RESET_US 500

//WS2812 NeoMatrixのピクセルモード状態 (WS_MODE_* / ws_data_mode / ws_data_offset /
//ws_matrix_buf / ws_data_reset) はbasic.hで定義 (WS.MODE/WS.PUTコマンドとWS.LEDで共用)

//RGB332 (RRRGGGBB) → 8bit G/R/B展開 (3bit値 v<<5|v<<2|v>>1: 7→255、2bit値 v<<6|v<<4|v<<2|v: 3→255)
static void ws_rgb332_expand(uint8 v332, uint8* g, uint8* r, uint8* b) {
    uint8 r3 = (v332 >> 5) & 7, g3 = (v332 >> 2) & 7, b2 = v332 & 3;
    *r = (uint8)((r3 << 5) | (r3 << 2) | (r3 >> 1));
    *g = (uint8)((g3 << 5) | (g3 << 2) | (g3 >> 1));
    *b = (uint8)((b2 << 6) | (b2 << 4) | (b2 << 2) | b2);
}

//WS.LEDのLED番号k (0起点) に対応するピクセルデータを現在のモードで取得
//(オフセット以降から読む。PIO/ビットバング両経路で共用)
static void ws_fetch_px(int k, uint8* g, uint8* r, uint8* b) {
    int o = ws_data_offset;
    if (ws_data_mode == WS_MODE_BUFFER) {
        const uint8_t* p = ws_matrix_buf + (size_t)(o + k) * 3;
        *g = p[0]; *r = p[1]; *b = p[2];
    } else if (ws_data_mode == WS_MODE_RGB888) {
        int idx = o + k * 3;
        *g = (uint8)*basic_getArrayPtr(idx);
        *r = (uint8)*basic_getArrayPtr(idx + 1);
        *b = (uint8)*basic_getArrayPtr(idx + 2);
    } else if (ws_data_mode == WS_MODE_RGB332X2) {
        int16 v = *basic_getArrayPtr(o + k / 2);
        uint8 v332 = (k & 1) ? (uint8)(v >> 8) : (uint8)v; // 偶数px=下位バイト、奇数px=上位バイト
        ws_rgb332_expand(v332, g, r, b);
    } else { // WS_MODE_RGB332
        uint8 v332 = (uint8)*basic_getArrayPtr(o + k);
        ws_rgb332_expand(v332, g, r, b);
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
static bool ws_pio_invert_patched = false; //PIO命令メモリへ適用済みの反転状態 (WS.SETUP invert)

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

//ピンをPIOに接続し、SMを最初の命令で停止させてアイドルレベルを保持したままリセット時間を作る
//(invert=1時はアイドルHi。SM停止中もstall命令のside-setが出力され続けるため反転アイドルを維持)
static void ws_pio_configure(uint8 pin, bool invert) {
    //SIO出力の残存状態に関わらず、PIO切替前に確実にアイドルレベルへ
    //SIOラッチ操作はパッドに直接影響しないので、PIO機能中でも安全
    gpio_put(pin, invert);
    gpio_set_dir(pin, GPIO_OUT);

    pio_sm_set_enabled(ws_pio, ws_pio_sm, false);
    pio_sm_clear_fifos(ws_pio, ws_pio_sm);
    pio_sm_restart(ws_pio, ws_pio_sm);
    //信号反転: side-setビット (bit12) をXORしてPIOプログラムの波形極性を反転する
    //(本プログラムは全命令がside-set 1bitを持つため全命令への一様なXORで良い。
    // 命令メモリはプログラム共用なので適用状態を追跡し、変化した時だけ書込む)
    if (ws_pio_invert_patched != invert) {
        for (uint i = 0; i < ws2812_program.length; i++) {
            ws_pio->instr_mem[ws_pio_offset + i] ^= 0x1000;
        }
        ws_pio_invert_patched = invert;
    }
    ws2812_program_init(ws_pio, ws_pio_sm, ws_pio_offset, pin, 800000.0f, false);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);//5Vストリップ向けに信号マージンを確保
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    sleep_us(WS_RESET_US); //リセット信号 (WS2812B-2020世代のTreset>280µsにマージン)
}

static void ws_pio_release(uint8 pin, bool invert) {
    pio_sm_set_enabled(ws_pio, ws_pio_sm, false);
    pio_sm_clear_fifos(ws_pio, ws_pio_sm);
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, invert); //アイドルレベル (反転時はHi)
}

//PIO確保失敗時のみ使う従来方式 (割込み禁止区間がある)
static void __noinline __not_in_flash_func(ws_bitbang_out) (uint8 pin, int nled, int repeat, bool invert) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    gpio_put(pin, invert); //アイドルレベル (反転時はHi)
    sleep_us(WS_RESET_US); //リセット信号
    //repeat分を1つの割込み禁止区間で連続送信 (フレーム間にリセットが入らない)
    //最悪でも2730px×3×8×1.26µs≈82ms/1repeat (mode3) で、NTSC表示はDMAで維持される
    int save = save_and_disable_interrupts();
    for (int i = 0; i < repeat; i++) {
        for (int k = 0; k < nled; k++) {//1つのLEDにつきG/R/B 3バイト (WS.MODEに応じたソース)
            uint8 g, r, b;
            ws_fetch_px(k, &g, &r, &b);
            uint8 rgb[3] = { g, r, b }; //GRB順 (MSBファースト)
            for (int j = 0; j < 3; j++) {
                for (int bi = 7; bi >= 0; bi--) {
                    wsled_send_one_signal_1575(pin, (rgb[j] >> bi) & 0x01, invert);
                }
            }
        }
    }
    restore_interrupts(save);
    gpio_put(pin, invert); //アイドルレベルへ戻す (反転時はHi)
    //長い割込み禁止中はDMAリングの再構築 (compose) が止まり、復帰後の
    //合流処理では1スロットしか進められずarm_nextが実再生位置からずれる。
    //ずれは自己修復しないため (VSYNCが内容に対してずれた位置に合成され、
    //映像がロールしたまま次のntscv_initまで復旧しない)、ここでDMAリングを
    //フィールド先頭から再構成する (ntscv_resync: PWM/DMAを止めないソフトリセット)
    if (ntsc_active()) {
        ntscv_resync();
    }
    sleep_us(WS_RESET_US); //終端のリセット (ラッチ)
}

//基板内蔵WS2812 (GP16) のピン番号 (WS.LED 0 / 起動・診断表示で使用)
static uint8 led_builtin_pin = 16;

//WS.LED port,n[,repeat] (4K版: ポート選択式+拡張配列変数対応+WS.MODEピクセルモード)
//port: 0=基板内蔵WS2812(GP16)、1-6=OUT1-6、7=外部WS2812(GP15)、8=LED(GP2)、9-11=OUT9-11 (IN2/IN3/IN4)
//(OUT8=IN1のみWS.LEDからは指定不可: 8がLED(OUT7)に割り当てられているため)
//n: LED数、repeat: 送信回数。色データはWS.MODEのモード/オフセットに従って読み込む
static void __noinline ws_out(int port, int nled, int repeat) {
    if (port < 0 || port > 11 || nled < 1 || repeat < 1) {
        return;
    }
    //WS.MODEのモード/オフセットに応じた残りピクセル数へ自動切詰め
    if (ws_data_mode == WS_MODE_BUFFER) {
        int avail = WS_BUFFER_PIXELS - ws_data_offset;
        if (nled > avail) nled = avail;
    } else if (ws_data_mode == WS_MODE_RGB888) {
        int avail = (IJB_SIZEOF_ARRAY_MAX - ws_data_offset) / 3;
        if (nled > avail) nled = avail;
    } else if (ws_data_mode == WS_MODE_RGB332X2) {
        int avail = (IJB_SIZEOF_ARRAY_MAX - ws_data_offset) * 2;
        if (nled > avail) nled = avail;
    } else { // WS_MODE_RGB332
        int avail = IJB_SIZEOF_ARRAY_MAX - ws_data_offset;
        if (nled > avail) nled = avail;
    }
    if (nled < 1) return; //オフセット以降にデータがない
    if (ws_setup_leds > 0 && nled > ws_setup_leds) {
        nled = ws_setup_leds; //WS.SETUPで設定したLED数上限へ切詰め (上限超過分のLEDは色を保持)
    }
    // 0=基板内蔵WS2812(GP16)、7=外部WS2812(GP15)、8=LED(GP2)、1-6=OUT1-6
    uint8 pin = (port == 0) ? led_builtin_pin : ((port == 7) ? WS2812_PIN : ((port == 8) ? LED : out_pins[port - 1]));
    bool inv = (port != 0) && (ws_setup_invert != 0); //内蔵WS2812 (WS.LED 0) は常に正相
    if (sd_pin_reserved(pin)) {
        return;//SD動作中の共有ピン (OUT1/3/4) はWS2812出力禁止
    }
    if (!ws_pio_init()) { //PIOを確保できない場合のみ従来方式
        ws_bitbang_out(pin, nled, repeat, inv);
        return;
    }

    //PIO送信中は映像DMA以外の割込みをマスクする (SAVE/DISTANCEと同じNVIC方式)。
    //60Hzタイマ (表示更新+tuh_task) 等の長い割込みがFIFO補給を止めると、
    //TX FIFO (8語=240µs) が枯渇してSMがstall命令で停止し、データ線が
    //テープのリセット閾値以上LOWになり疑似リセットが発生する。
    //すると残りのデータがLED 0から再ラッチされ、パターンがランダムな
    //オフセットで重複点灯する (LED数が多い=送信が長いほど発生しやすい)。
    //映像DMA割込み (DMA_IRQ_1) は動作継続 (ハンドラ最大~125µs < FIFO 240µsのため
    //枯渇しない)。60Hzタイマが送信中 (~2ms) 一時停止するため表示が一瞬静止するが
    //DMA信号は継続し、復帰後にmerge経路で自動同期する
    uint32_t nvic_state = nvic_hw->iser;
    nvic_hw->icer = ~(1u << DMA_IRQ_1);
    ws_pio_configure(pin, inv);//この中でリセットを出す
    //repeat回数分、リセットを挟まず連続送信する (公式IchigoJam仕様:
    //n個分のパターンが後続LEDへ流れて、テープ長分タイル状に繰り返される。
    //repeatでも毎回オフセットから同じデータを再読み込みする)
    for (int i = 0; i < repeat; i++) {
        for (int k = 0; k < nled; k++) {
            uint8 g, r, b;
            ws_fetch_px(k, &g, &r, &b);
            uint32_t v = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
            pio_sm_put_blocking(ws_pio, ws_pio_sm, v); //GRB順、MSBファースト (下位8bitは無視される)
        }
    }
    while (!pio_sm_is_tx_fifo_empty(ws_pio, ws_pio_sm)) {
        tight_loop_contents();
    }
    nvic_hw->iser = nvic_state;
    sleep_us(WS_RESET_US + 30); //最後の1ワード(約30us) + リセット(WS_RESET_US) でラッチ
    ws_pio_release(pin, inv);
}

//===== 基板内蔵WS2812 (GP16) — 起動表示・診断表示用 =====
//LEDコマンド/OUT7はGP2を制御 (io.h)。WS.LED 7はGP15(外部WS2812)、WS.LED 8はGP2、WS.LED 0はGP16
//GP16は本関数群から制御される: 起動時の1回緑点滅、エラー時の高速赤点滅

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
        wsled_send_one_signal_1575(pin, (grb >> k) & 0x01u, false); //起動・診断表示は常に正相
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

// 起動時: 緑を1回点滅 (約500ms ON / 500ms OFF) して消灯
void led_builtin_boot_blink(void) {
    for (int i = 0; i < 1; i++) {
        led_builtin_green(true);
        sleep_ms(500);
        led_builtin_green(false);
        sleep_ms(500);
    }
}

#endif
