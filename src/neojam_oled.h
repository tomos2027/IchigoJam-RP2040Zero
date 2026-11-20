//NEOJAM: EJ OLED表示 (SSD1306 128x64、I2C 0x3C)
//Milkjam (src/iotpad_oled.h) のOLED部をNEOJAMへ移植
//U8g2フォントを事前デコードしたビットマップで描画 (oled_fonts.h、U8g2 library by olikraus, BSD-3-Clause)
//行モデル: baseline = (i+1)*フォント高さ + y_offset
//グリフモデル (u8g2_font.c準拠): top = y - (h + gy), left = x + gx
//3画面レイアウト (システム情報 / ENV.IN / 入力状態)
//※Milkjamと異なりWiFi・定期更新タスクは持たない (OLEDPAGE/OLEDOFF/OLEDFONT実行時に都度再描画)
#ifndef __NEOJAM_OLED_H__
#define __NEOJAM_OLED_H__
#include "all_includes.h"
#include <stdarg.h>
//フォントデータはall_includes.hでoled_fonts.hとして既にinclude済み

#define OLED_ADDR     0x3C
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_PAGES    (OLED_HEIGHT / 8)
#define OLED_BUFSIZ   (OLED_WIDTH * OLED_PAGES)

//===== SSD1306 コマンド =====
#define OLED_CMD          0x00
#define OLED_DATA         0x40

//===== 状態 =====
#define OLED_FONT_COUNT 5
#define OLED_DEFAULT_FONT 2 // 8x13標準 (Milkjam/padのDEFAULT_FONT_INDEX準拠)
#define OLED_SCREEN_COUNT 3 // NEOJAMステータスページ数
static bool oled_ok = false;
static bool oled_probed = false; // 起動時に1回だけプローブ (未接続なら以後 "OLED Not Found")
static bool oled_direct = false;
static int  oled_font_index = OLED_DEFAULT_FONT;
static int  oled_cursor_x = 0, oled_cursor_y = 0;
static int  oled_current_screen = 1;

//フレームバッファ (フォント描画より前の位置に定義)
static uint8_t oled_buf[OLED_BUFSIZ];
static void oled_buf_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    if (on) oled_buf[y / 8 * OLED_WIDTH + x] |= (1 << (y & 7));
    else    oled_buf[y / 8 * OLED_WIDTH + x] &= ~(1 << (y & 7));
}

//===== フォントテーブル =====
static const OLED_Char* oled_font_tables[OLED_FONT_COUNT] = {
    oled_font_5_7,
    oled_font_6_12,
    oled_font_8_13,
    oled_font_9_15,
    oled_font_10_20,
};
static const char* oled_font_names[OLED_FONT_COUNT] = { "5x7", "6x12", "8x13", "9x15", "10x20" };
static const uint8_t oled_font_h[OLED_FONT_COUNT] = { 7, 12, 13, 15, 20 };       // フォント高さ
static const int8_t oled_font_yoff[OLED_FONT_COUNT] = { 0, 0, -1, 0, 0 };        // 8x13は上に1px補正

//===== フォント番号のflash保存 (config領域byte1。KBD byte0と同じセクタ) =====
//core1にフラッシュ消去/書込みを委譲し、core0は映像DMA割込みを継続させる (IJB_kbdと同一経路)
static uint8_t oled_tflash[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
static void oled_save_font_flash(int idx) {
    uint32_t offset = get_config_offset();
    memcpy(oled_tflash, get_flash(offset), FLASH_SECTOR_SIZE);
    oled_tflash[1] = (uint8_t)idx;
    psg_core1_flash_op_and_wait(offset, oled_tflash);
}
//起動時にconfig byte1からフォント番号を復元 (範囲外=未保存なら既定値のまま)
static void oled_load_font_flash() {
    int idx = get_flash(get_config_offset())[1];
    if (idx >= 0 && idx < OLED_FONT_COUNT) oled_font_index = idx;
}

//===== SSD1306 I2Cドライバ (i2c0は起動時にi2c0_init()済み、GP4=SDA/GP5=SCL) =====
static int oled_i2c_write(const uint8_t* data, int len) {
    return i2c_write_timeout_us(i2c0, OLED_ADDR, (uint8_t*)data, len, false, TIMEOUT_US) == len ? 0 : -1;
}
static int oled_i2c_cmd(uint8_t cmd) {
    uint8_t buf[2] = { OLED_CMD, cmd };
    return oled_i2c_write(buf, 2);
}
static int oled_i2c_data(const uint8_t* data, int len) {
    for (int i = 0; i < len; i += 32) {
        int chunk = (i + 32 < len) ? 32 : len - i;
        uint8_t buf[33];
        buf[0] = OLED_DATA;
        memcpy(buf + 1, data + i, chunk);
        if (oled_i2c_write(buf, chunk + 1) != 0) return -1;
    }
    return 0;
}
static bool oled_init_hw() {
    int r = 0;
    r |= oled_i2c_cmd(0xAE); // display off
    r |= oled_i2c_cmd(0xD5); r |= oled_i2c_cmd(0x80); // clk div
    r |= oled_i2c_cmd(0xA8); r |= oled_i2c_cmd(0x3F); // multiplex 63
    r |= oled_i2c_cmd(0xD3); r |= oled_i2c_cmd(0x00); // display offset
    r |= oled_i2c_cmd(0x40); // start line 0
    r |= oled_i2c_cmd(0x8D); r |= oled_i2c_cmd(0x14); // charge pump on
    r |= oled_i2c_cmd(0x20); r |= oled_i2c_cmd(0x00); // horizontal addressing
    r |= oled_i2c_cmd(0xA1); // segment remap
    r |= oled_i2c_cmd(0xC8); // COM scan direction
    r |= oled_i2c_cmd(0xDA); r |= oled_i2c_cmd(0x12); // COM pins
    r |= oled_i2c_cmd(0x81); r |= oled_i2c_cmd(0xCF); // contrast
    r |= oled_i2c_cmd(0xD9); r |= oled_i2c_cmd(0xF1); // precharge
    r |= oled_i2c_cmd(0xDB); r |= oled_i2c_cmd(0x40); // VCOMH
    r |= oled_i2c_cmd(0xA4); // display from RAM
    r |= oled_i2c_cmd(0xA6); // normal display
    r |= oled_i2c_cmd(0xAF); // display on
    return r == 0;
}

//===== フレームバッファ操作 =====
static void oled_buf_clear() { memset(oled_buf, 0, sizeof oled_buf); }
static void oled_buf_send() {
    oled_i2c_cmd(0x20); oled_i2c_cmd(0x02); // page addressing mode
    for (int page = 0; page < OLED_PAGES; page++) {
        oled_i2c_cmd(0xB0 | page);
        oled_i2c_cmd(0x00); // col low
        oled_i2c_cmd(0x10); // col high
        oled_i2c_data(oled_buf + page * OLED_WIDTH, OLED_WIDTH);
    }
}

//===== 文字描画 (事前デコード済みフォント、u8g2位置モデル) =====
//y_baseline=ベースライン位置 (u8g2のsetFontPosBaseline相当)
//戻り値=描画後のx位置 (OLEDPRINTのカーソル進行に使用)
static int oled_draw_str(int x, int y_baseline, const char* s) {
    const OLED_Char* tbl = oled_font_tables[oled_font_index];
    while (*s) {
        uint8_t c = (uint8_t)*s++;
        if (c < 0x20 || c > 0x7E) c = 0x3F; // '?'
        const OLED_Char* ch = &tbl[c - 0x20];
        int top = y_baseline - (ch->h + ch->gy); // u8g2: target_y -= (h + gy)
        if (top < 0) top = 0; // ベースラインがフォント高さ未満でも上端に表示 (y=0等で完全に消えないようクランプ)
        int left = x + ch->gx;                    // u8g2: target_x += gx
        for (int col = 0; col < ch->w; col++) {
            uint16_t bits = ch->data[col];
            for (int row = 0; row < ch->h; row++) {
                if (bits & (1 << row)) {
                    oled_buf_pixel(left + col, top + row, true);
                }
            }
        }
        x += ch->adv;
    }
    return x;
}
//行位置: baseline = (行+1)×フォント高さ + y補正
static int oled_line_y(int i) {
    return (i + 1) * oled_font_h[oled_font_index] + oled_font_yoff[oled_font_index];
}
static void oled_line(int i, const char* s) {
    oled_draw_str(0, oled_line_y(i), s);
}
//軽量書式 (%d/%u/%s のみ対応 — stdioのvsnprintfはフラッシュが約+50KB増えるため自前実装)
static char* oled_put_str(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}
static char* oled_put_num(char* p, unsigned v, bool neg) {
    char tmp[10];
    int n = 0;
    do { tmp[n++] = '0' + v % 10; v /= 10; } while (v);
    if (neg) *p++ = '-';
    while (n) *p++ = tmp[--n];
    return p;
}
static void oled_vfmt(char* dst, int dstlen, const char* fmt, va_list ap) {
    char* p = dst;
    char* end = dst + dstlen - 1;
    while (*fmt && p < end) {
        if (*fmt == '%' && fmt[1]) {
            fmt++;
            if (*fmt == 'd') {
                int v = va_arg(ap, int);
                unsigned u = v < 0 ? (unsigned)-(v + 1) + 1 : (unsigned)v; // INT_MIN安全
                p = oled_put_num(p, u, v < 0);
            } else if (*fmt == 'u') {
                p = oled_put_num(p, va_arg(ap, unsigned), false);
            } else if (*fmt == 's') {
                p = oled_put_str(p, va_arg(ap, const char*));
            } else {
                *p++ = *fmt;
            }
            fmt++;
        } else {
            *p++ = *fmt++;
        }
    }
    *p = 0;
}
static void oled_linef(int i, const char* fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    oled_vfmt(buf, sizeof buf, fmt, ap);
    va_end(ap);
    oled_line(i, buf);
}

//===== 画面レイアウト (NEOJAM向け3画面) =====
static void oled_update_display() {
    if (!oled_ok) return;
    oled_buf_clear();
    switch (oled_current_screen) {
    case 1: // システム情報
        oled_line(0, "NEOJAM BASIC");
        oled_line(1, "VER 16130 (1.6.1 4K)");
        oled_linef(2, "FREE %d", (IJB_SIZEOF_LIST - 2) - _g.listsize);
        oled_linef(3, "FONT %s", oled_font_names[oled_font_index]);
        break;
    case 2: { // 環境センサー (ENV.INと同一の取得経路)
        int16_t t10 = 0, h10 = 0, t10b = 0;
        uint32_t pa = 0;
        int aht = env_read_aht20(&t10, &h10);
        int bmp = env_read_bmp280(&t10b, &pa);
        if (aht == 0) oled_linef(0, "T:%d.%d H:%d.%d", t10 / 10, t10 % 10, h10 / 10, h10 % 10);
        else          oled_line(0, "T:-- H:--");
        if (bmp == 0) oled_linef(1, "P:%u.%02u hPa", (unsigned)(pa / 100), (unsigned)(pa % 100));
        else          oled_line(1, "P:--");
        oled_line(2, aht == 0 && bmp == 0 ? "AHT20+BMP280 OK" : (aht == 0 ? "AHT20 OK" : (bmp == 0 ? "BMP280 OK" : "ENV N/A")));
        break;
    }
    case 3: // 入力状態
        oled_linef(0, "IN   %03X", IJB_in() & 0x7FF); // 11bit (IN4は常に0)
        oled_linef(1, "BTN  %d", IJB_btn(0));
        oled_linef(2, "A1   %4d", IJB_ana(1)); // IN1 (GP27)
        oled_linef(3, "A2   %4d", IJB_ana(2)); // IN2 (GP26)
        break;
    }
    oled_buf_send();
}

//===== OLED全体初期化 (起動時に1回。未接続時はNACKで即失敗し以後再試行しない) =====
static void oled_probe_init() {
    if (oled_probed) return;
    oled_probed = true;
    oled_load_font_flash();
    oled_ok = oled_init_hw();
    if (oled_ok) {
        oled_buf_clear();
        oled_update_display();
    }
}

#endif
