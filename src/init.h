#ifndef __INIT_H__
#define __INIT_H__
#include "all_includes.h"

//Waveshare RP2040-Zero (RP2040)
// NTSC駆動 (既定: 525ライン/157.5MHz/60.0716Hz、VREG 1.20Vオーバークロック)
// プリセット切替 (-DNTSCV_SIG_PRESET) により元ドライバ系信号にもできる
//IchigoJam
#define FONT_SIZE 8
//UARTは基板の側面パッドGP0 (TX) / GP1 (RX) ※keyboard.hのUART_ID (uart0) と対
#define UART_TX_PIN 0
#define UART_RX_PIN 1

#define VREG_VSEL VREG_VOLTAGE_1_20

//NTSC用フォントRAM (IchigoJamのフォント+PCG+カーソル表示用の0xFF)
//診断用カナリアで前後を挟む (structで物理レイアウトを保証)
struct {
    volatile uint32_t canary1;
    uint8_t fontram[256 * 8];
    volatile uint32_t canary2;
} ntsc_fg;
#define ntsc_fontram ntsc_fg.fontram
static volatile uint32_t ntsc_diag_error = 0;//診断用: 検知したメモリ破壊 (1:Fontp 2:vram 4:videomode 8:フォントRAM境界 16:DMAバッファ境界 32:走査線カウンタ 64:拡大倍率)
static uint32_t ntsc_diag_frames = 0;//診断用: 破壊を検知した時のフレーム数

//NTSCの1走査線ごとに呼ばれる (_g.linecnt++: TICK()、WAIT -n用)
// フラッシュ書込中もDMA割込みから呼ばれるためRAM配置する
static void __not_in_flash_func(ntsc_line_callback)(void) {
    _g.linecnt++;
}

//表示シャドウバッファ: 60Hzタイマーがここへ描画し、フィールド境界フック (DMA IRQ) が
//TVRAMへ一括memcpyで反映する。vram_to_tvram_all全体をIRQで実行すると処理が重く
//DMA再arm期限 (508µs) を超えて合流 (合成遅延) が多発するため、IRQ内の処理を
//memcpy 1回 (約30µs) に抑える
//構築中フラグ: フックのmemcpyがタイマーの描画途中に割り込むとシャドウの中途状態が
//コピーされティアになるため (60Hz対60.19Hzのうなりで約5.4秒周期で発生)、
//構築完了 (shadow_ready) のときだけコピーし、構築中は1フィールドスキップする
static uint8_t tvram_shadow[WIDTH_XMAX * WIDTH_Y * 2 + 1] __attribute__((aligned(4)));
static volatile uint8_t tvram_shadow_building = 0;
static volatile uint8_t tvram_shadow_ready = 0;

//1フレーム分vramの内容を表示シャドウバッファへ反映する (TVRAMへの反映はフィールドフック)
//VIDEO 3, 4 (screen_big) のときは1文字をmagxmag文字に複製して拡大表示する
void vram_to_tvram_all(bool visible_cursor) {
    //PCG (0xE0-0xFF) をフォントRAMへ反映 (POKE #700-#7FF対応)
    memcpy(ntsc_fontram + 0xE0 * 8, screen_pcg, SIZE_PCG * 8);

    //カーソルは属性ビット (IJ_ATTR_CURSOR) で描画側に伝える
    //フォントスロット0xFFをカーソルに使うとPCG文字 (0xE0-0xFF) と衝突しゴースト文字が出るため、
    //TVRAMには本来の文字コードを書き、描画時にマスクXORする (DVIと同じ方式)
    ntsc_cursor_mask = key_flg.insert ? 0xff : 0xf0;
    int cursor_pos = -1;
    if (((frames >> 4) & 1) && visible_cursor && _g.cursory >= 0 && _g.cursorx < SCREEN_W && _g.cursory < SCREEN_H) {
        cursor_pos = _g.cursory * SCREEN_W + _g.cursorx;
    }

    // ===== カラーモード (VIDEO 9/10: IJ_COLOR / COLOR_TEXT) =====
    // セル属性 = 下位4bit前景 / 上位4bit背景。反転・カーソルは前景と背景を入れ替える。
    if (ntscv_mode == NTSCV_MODE_IJ_COLOR) {
        // 32x24 IchigoJam互換レイアウト + カラー属性
        ntsc_mag = 1;
        for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                int idx = y * SCREEN_W + x;
                uint8 a = screen_attr[idx];
                if (_g.screen_invert || idx == cursor_pos)
                    a = (uint8)(((a & 0x0F) << 4) | ((a >> 4) & 0x0F));
                tvram_shadow[idx] = vram[idx];
                tvram_shadow[IJ_ATTROFFSET + idx] = a;
            }
        }
        return;
    }
    if (ntscv_mode == NTSCV_MODE_COLOR_TEXT) {
        // 42x27 カラーテキスト (VIDEO 10)。SCREEN_W/SCREEN_H は video_on() で 42/27 に設定済み。
        ntsc_mag = 1;
        for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                int idx = y * SCREEN_W + x;
                uint8 a = screen_attr[idx];
                if (_g.screen_invert || idx == cursor_pos)
                    a = (uint8)(((a & 0x0F) << 4) | ((a >> 4) & 0x0F));
                tvram_shadow[y * WIDTH_X + x] = vram[idx];
                tvram_shadow[ATTROFFSET + y * WIDTH_X + x] = a;
            }
        }
        return;
    }

    // ===== 単色 IJTEXT (従来。VIDEO 1-4 など) =====
    uint8 attr = _g.screen_invert ? IJ_ATTR_INVERT : 0;
    int mag = 1 << _g.screen_big;
    ntsc_mag = mag;//NTSC描画側に拡大倍率を伝える (複製セルを拡大グリフの断片として描画する)
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint8 c = vram[y * SCREEN_W + x];
            uint8 cellattr = attr | (y * SCREEN_W + x == cursor_pos ? IJ_ATTR_CURSOR : 0);
            uint8* tvram_dst = tvram_shadow + y * mag * IJ_WIDTH + x * mag;
            uint8* attr_dst = tvram_shadow + IJ_ATTROFFSET + y * mag * IJ_WIDTH + x * mag;
            for (int j = 0; j < mag; j++) {
                for (int k = 0; k < mag; k++) {
                    tvram_dst[j * IJ_WIDTH + k] = c;
                    attr_dst[j * IJ_WIDTH + k] = cellattr;
                }
            }
        }
    }
}

//表示更新 (DMA IRQのフィールド境界フックから1回/フィールドで呼ばれる)。
//60Hzタイマーからコピーするとフィールド60.19Hzとのうなり (51µs/フレーム) で
//ティア線が約5.4秒周期で画面を一周し、画面ジャンプ/ロールに見えていたため、
//マークライン直後のVSYNC+ブランキング窓 (非表示) でシャドウをTVRAMへ一括反映する。
//IRQ内の処理はワードコピーのみ (vram_to_tvram_all本体は60Hzタイマーがシャドウへ描画)。
//タイマーが構築中のときはこのフィールドはスキップし、次の境界で反映する
//SAVE/KBD等のフラッシュ書込み (core1委譲) 中はXIPが不能になるため、
//DMA割込みから到達可能なこの関数はRAM配置必須。memcpy自体がフラッシュ配置の
//ため呼べず、ループでコピーする (sound.h:60のnoinline教訓に従いRAM配置を保証)
static void __attribute__((noinline)) __not_in_flash_func(field_display_update)(void) {
    if (tvram_shadow_ready) {
        //TVRAM/tvram_shadowはaligned(4)。サイズ=WIDTH_XMAX*WIDTH_Y*2+1 (4321B)
        //のためワードコピー+末尾バイト
        uint32_t *d = (uint32_t *)TVRAM;
        const uint32_t *s = (const uint32_t *)tvram_shadow;
        for (uint32_t n = sizeof(TVRAM) / 4; n; n--) {
            *d++ = *s++;
        }
        uint8_t *db = (uint8_t *)d;
        const uint8_t *sb = (const uint8_t *)s;
        for (uint32_t n = sizeof(TVRAM) & 3; n; n--) {
            *db++ = *sb++;
        }
        tvram_shadow_ready = 0;
    }
}

//NTSCビデオ出力を初期化し開始する (クロックはNTSC用の157.5MHz設定済みのこと)
void ntsc_init() {
    ntsc_fg.canary1 = 0xC0DE6001;//診断用カナリア
    ntsc_fg.canary2 = 0xC0DE6002;
    //IchigoJamのフォントをフォントRAMへ先にコピーしておく (0xE0-0xFFは毎フレームscreen_pcgから反映される)
    memcpy(ntsc_fontram, CHAR_PATTERN, 256 * 8);
    Fontp = ntsc_fontram;
    rp2040_pwm_ntsc_init(NTSC_PIN);
    screen_reset_palette();//COLOR用16色パレットをNTSCドライバへ反映
    ntsc_set_line_callback(ntsc_line_callback);
    ntscv_set_field_hook(field_display_update);//表示更新をフィールド境界へ同期 (ティアリング修正)
    //IJTEXTモードへ直接設定する (set_videomodeだとstop/start_compositeが走り同期が乱れるため)
    videomode = VMODE_IJTEXT;
    twidth = IJ_WIDTH;
    twidthy = IJ_ROWS;
    attroffset = IJ_ATTROFFSET;
    ntsc_mag = 1 << _g.screen_big;//VIDEOコマンドの拡大設定を引き継ぐ
    clearscreen();
}

//SWITCHコマンドの実体 (basic.hのcommand_lcdから呼ばれる)
//本機はNTSCコンポジット出力専用のため何もしない (HDMI/DVI非対応、SWITCHは予約語)
void IJB_lcd(uint mode) {
    (void)mode;
}

//hid_app.cが複雑になってるので、もっと簡潔に処理できるなら直したい
void putc_long_press_key() {
    int save = save_and_disable_interrupts();//割り込みを止めないとなぜかtime-us-64() - lp.last_key_report_timeがオーバーフローする時がある
    int interval = lp.is_first_putc ? 500000 : 100000;//キーを長押しした時、最初の1回だけ入力の間隔を長くする
    if (time_us_64() - lp.last_key_report_time > interval) {
        lp.last_key_report_time = time_us_64();
        if (lp.last_char != 0) {
            key_pushc(lp.last_char);
            lp.is_first_putc = false;
        }
    }
    restore_interrupts(save);
}

bool timer(repeating_timer_t* rt) {
    frames++;
    watchdog_update();//ウォッチドッグをfeedする (core0が固まったら自動再起動)
    psg_tick();
    set_tone();
    //NTSC整合性チェック (診断用): メモリ破壊を検知したらエラー番号を画面に表示し、以降の通常画面更新を止める
    uint32_t err = 0;
    if (Fontp != ntsc_fontram) err |= 1;//フォントポインタ破壊
    if (videomode == VMODE_WIDETEXT || videomode == VMODE_WIDEGRPH) {
        if (vram != vram_wide) err |= 2;//VIDEO 10/拡大VRAMポインタ破壊
    } else {
        if (vram != (uint8*)RAM_VRAM) err |= 2;//VRAMポインタ破壊
    }
    if (videomode != NTSCV_MODE_IJ_TEXT &&
        videomode != NTSCV_MODE_COLOR_TEXT &&
        videomode != NTSCV_MODE_COLOR_GRAPH &&
        videomode != NTSCV_MODE_MONO_TEXT &&
        videomode != NTSCV_MODE_IJ_COLOR) err |= 4;//ビデオモード破壊
    if (ntsc_fg.canary1 != 0xC0DE6001 || ntsc_fg.canary2 != 0xC0DE6002) err |= 8;//フォントRAM境界破壊
    if (ntscv_guard_a() != NTSCV_GUARD_A_VALUE || ntscv_guard_b() != NTSCV_GUARD_B_VALUE) err |= 16;//DMAバッファ境界破壊
    if (ntsc_scanline() >= NUM_LINES) err |= 32;//走査線カウンタ破壊
    if (ntsc_mag != 1 && ntsc_mag != 2 && ntsc_mag != 4 && ntsc_mag != 8) err |= 64;//拡大倍率破壊
    //瞬間的な遷移 (フラッシュ書込後のNTSC再初期化中にrp2040_pwm_ntsc_initが設定する
    //videomode=VMODE_WIDETEXT等、約1ms以内に復帰する一時状態) の誤検知を防ぐため、
    //同一エラーが3フレーム連続 (50ms) 継続した場合のみラッチする
    // (SAVE/KBDのフラッシュ書込後の位相再合わせ ntsc_release+ntsc_init が該当、初回コミットからの構造)
    static uint32_t diag_prev_err = 0;
    static uint8_t diag_persist = 0;
    if (err) {
        if (!ntsc_diag_error) {
            if (err == diag_prev_err) {
                if (++diag_persist >= 3) {
                    ntsc_diag_error = err;
                    ntsc_diag_frames = frames;
                }
            } else {
                diag_prev_err = err;
                diag_persist = 1;
            }
        }
    } else {
        diag_persist = 0;//エラーが消えたら連続カウントをリセット
    }
    if (ntsc_diag_error) {
        //エラー表示をシャドウへ構築する (TVRAMへの反映はフィールドフックのmemcpyで行う)
        tvram_shadow_ready = 0;
        tvram_shadow_building = 1;
        memset(tvram_shadow, 0, sizeof tvram_shadow);
        uint8* p = tvram_shadow;
        *p++ = 'E';
        *p++ = '0' + ntsc_diag_error / 10;
        *p++ = '0' + ntsc_diag_error % 10;
        *p++ = ' ';
        *p++ = 'F';
        uint32 f = ntsc_diag_frames % 1000;
        *p++ = '0' + f / 100;
        *p++ = '0' + (f / 10) % 10;
        *p++ = '0' + f % 10;
        //E番号が見えるよう、先頭8セルの属性を白(前景7/背景0)にする
        for (int i = 0; i < 8; i++) {
            tvram_shadow[IJ_ATTROFFSET + i] = 0x07;
            tvram_shadow[ATTROFFSET + i] = 0x07;
        }
        tvram_shadow_building = 0;
        tvram_shadow_ready = 1;
        //エラー時は内蔵WS2812を赤で高速点滅させて知らせる
        static bool diag_led;
        if ((frames & 7) == 0) {
            diag_led = !diag_led;
            led_builtin_red(diag_led);
        }
    } else {
        tvram_shadow_ready = 0;
        tvram_shadow_building = 1;
        vram_to_tvram_all(_g.cursorflg);
        tvram_shadow_building = 0;
        tvram_shadow_ready = 1;
    }
    //TVRAMへの反映はフィールド境界フック (field_display_update) のmemcpyで行う。
    //60Hzタイマーからの非同期コピーはフィールドとのうなりで約5.4秒周期の
    //ティア線/画面ジャンプを生じていたため
    //中でsleep_ms()が呼ばれたときに止まるので、本来タイマーの中でtuh_task()を呼び出してはいけないが、なぜか動いている
    //pico-sdk/lib/tinyusb/src/osal/osal_pico.h　のosal_task_delay()のsleepをwhileループに置き換えると解決する
    tuh_task();
    putc_long_press_key();
    return true;
}

//ハードフォルト時に内蔵WS2812を赤で高速点滅させる (画面が使えなくても状態を通知する、診断用)
//ウォッチドッグを止めないと8秒後に再起動して点滅が消えてしまう
void isr_hardfault(void) {
    watchdog_disable();
    bool on = true;
    led_builtin_red(true);
    for (;;) {
        busy_wait_us(60000);//約8Hz点滅
        on = !on;
        led_builtin_red(on);
    }
}

void on_uart_rx() {
    while (uart_is_readable(UART_ID)) {
        uint8_t ch = uart_getc(UART_ID);
        if (ch == ESC) {
            _g.key_flg_esc = (_g.uartmode_rxd & 2) == 0; // 1.2b41
        }
        if (_g.uartmode_rxd & 1) {
            key_pushc(ch);
        }
    }
}

//0番のプログラムの先頭行の先頭が @ARUN かどうか
bool is_arun() {
    //0番のプログラムデータ
    //行番号0、行番号1、行の文字数、行の内容...とデータが入っているので、index3からチェックする(行番号1*256+行番号0=行番号)
    const uint8_t* flash = get_flash(calc_flash_offset(0));
    uint8_t arun[] = "@ARUN";
    for (int i = 0; arun[i] != '\0'; i++) {
        if (arun[i] != flash[i + 3]) {
            return false;
        }
    }
    return true;
}

void pico_init() {
    static repeating_timer_t out;
    board_init();

    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);//電圧が安定してからクロックを上げる
    // Run system at NTSC clock (157.5MHz = 4×カラーサブキャリア3.579545MHz。
    // 全プリセット共通: 整数kHzのためset_sys_clock_khzを使用)
    if (clock_get_hz(clk_sys) != NTSCV_SYS_CLOCK_HZ) {
        set_sys_clock_khz(157500, true);
    }

    stdio_uart_init();//クロックを変えてから初期化する
    // uart_set_hw_flow(UART_ID, false, false);//いる？
    // uart_set_fifo_enabled(UART_ID, false);
    irq_set_exclusive_handler(UART_IRQ, on_uart_rx);
    irq_set_enabled(UART_IRQ, true);
    uart_set_irq_enables(UART_ID, true, false);
    gpio_pull_up(UART_TX_PIN);
    gpio_pull_up(UART_RX_PIN);

    //内蔵WS2812 (GP16) は起動・エラー表示専用のため、ここでは点灯しない
    //LEDコマンドはGP2 (io.h) を制御

    // init host stack on configured roothub port
    tuh_init(BOARD_TUH_RHPORT);
    tuh_task();//消すと起動時にキーボード接続していた時に、認識しない時がある？
    add_repeating_timer_us(-16666, timer, NULL, &out);//60FPS
    //ウォッチドッグ: core0が固まったら自動再起動する (timer()でfeedする)
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
}

//起動時にNTSCビデオ出力を初期化する (main()から呼ばれる)
void video_output_init() {
    ntsc_init();
}

void ichigojam_init() {
    //SCREEN_W,SCREEN_Hはグローバル変数に置換されるので、基本的にSCREEN_W等を介してアクセスする
    SCREEN_W = CHAR_MAX_COLS;
    SCREEN_H = CHAR_MAX_ROWS;
    screen_clear();

    key_clearKey();
    key_flg.caps = true;
    set_keymap(key_getKeyboardID());
    random_init();
    io_init();
    screen_clp();
    sound_init();
    uart_init_IJ();
    i2c0_init();
    oled_probe_init(); // SSD1306 OLED初期化 (EJ OLED、未接続時はNACKで即スキップ)

    int sleepflg = getSleepFlag();//起動時ボタンを押していたらtrue
    sleepflg |= is_arun();//プログラムの先頭が@ARUNならtrue
    sleepflg |= watchdog_enable_caused_reboot();//SLEEPコマンドで再起動していたらtrue
    if (!sleepflg) {
        psg_beep(10, 3);
    }
    // video_waitSync(BOOT_WAIT);
#ifdef IJB_TITLE
    for (int i = 0;; i++) { // 自動起動時は表示だけしない
        char c = IJB_TITLE[i];
        if (c == '\0')
            break;
        if (!sleepflg)
            put_chr(c);
        video_waitSync(1);
    }
#endif
    if (!sleepflg) {
        put_str("OK\n");
    }
    _g.sleepflg = sleepflg;
}

#endif
