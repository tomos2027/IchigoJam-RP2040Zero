/*----------------------------------------------------------------------------

ntsc_compat.h — 旧NTSCドライバAPI互換shim

Copyright (c) 2026 the IchigoJam-RP2040Zero authors. MIT license.

IchigoJam移植部 (init.h/display.h/keyboard.h/storage.h) が旧ドライバ
(rp2040_pwm_ntsc_textgraph) 向けに使っていた名前を、新ライブラリ ntscv
(src/video/) のAPIへつなぐ薄い互換層。互換名はインターフェースとして
機能的なものであり、実装はすべて ntscv 側の独自コードによる。

----------------------------------------------------------------------------*/

#ifndef NTSC_COMPAT_H
#define NTSC_COMPAT_H

#include "../video/ntscv.h"
#include <string.h>

/*==== 旧定数の互換 ===*/

#define X_RES  NTSCV_GRAPH_COLS   /* 336 */
#define Y_RES  NTSCV_GRAPH_ROWS   /* 216 */
#define WIDTH_X NTSCV_COLOR_COLS  /* 42 */
#define WIDTH_Y NTSCV_COLOR_ROWS  /* 27 */
#define WIDTH_XBW NTSCV_MONO80_COLS /* 80 */
#define WIDTH_XMAX WIDTH_XBW
#define ATTROFFSET (WIDTH_X * WIDTH_Y)      /* カラー属性エリア位置 1134 */
#define ATTROFFSETBW (WIDTH_XBW * WIDTH_Y)  /* モノクロ属性エリア位置 2160 */

#define NUM_LINE_SAMPLES NTSCV_SAMPLES_PER_LINE
#define NUM_LINES NTSCV_LINES_PER_FIELD
#define V_SYNC NTSCV_FIELD_VSYNC_LINES
#define V_PREEQ NTSCV_FIELD_BLANK1_LINES
#define H_SYNC NTSCV_H_SYNC_SAMPLES
#define H_PICTURE NTSCV_LINE_HEAD_SAMPLES

#define IJ_WIDTH NTSCV_IJ_COLS
#define IJ_ROWS NTSCV_IJ_ROWS
#define IJ_LINES_PER_CHAR NTSCV_IJ_LINES_PER_ROW
#define IJ_HEIGHT (IJ_ROWS * IJ_LINES_PER_CHAR)
#define IJ_CHAR_SAMPLES NTSCV_IJ_CHAR_SAMPLES
#define IJ_ATTROFFSET (IJ_WIDTH * IJ_ROWS)  /* 属性エリア位置 768 */
#define IJ_PICTURE H_PICTURE
#define IJ_ATTR_INVERT NTSCV_IJ_ATTR_INVERT
#define IJ_ATTR_CURSOR NTSCV_IJ_ATTR_CURSOR

/* ビデオモード値 (ntscvと同一の数値) */
#define VMODE_WIDETEXT NTSCV_MODE_COLOR_TEXT
#define VMODE_MONOTEXT NTSCV_MODE_MONO_TEXT
#define VMODE_IJTEXT NTSCV_MODE_IJ_TEXT
#define VMODE_WIDEGRPH NTSCV_MODE_COLOR_GRAPH

/* テキスト/グラフィックモード値 (互換の vestigial 値) */
#define TMODE_WIDETEXT 3
#define TMODE_MONOTEXT 5
#define GMODE_NOGRPH 0
#define GMODE_WIDEGRPH 3

/*==== 旧変数の互換 ===*/

/* ntscvの公開変数への別名 */
#define videomode ntscv_mode
#define drawcount ntscv_frame_seq
#define ntsc_mag ntscv_ij_zoom
#define ntsc_cursor_mask ntscv_ij_cursor_xor

/* 旧TVRAM互換レイアウト: [0..W*H)=文字コード、[W*H..2*W*H)=属性。
   モードごとに属性エリア位置が変わる (IJ 768 / カラー 1134 / 80桁 2160)。 */
static uint8_t TVRAM[WIDTH_XMAX * WIDTH_Y * 2 + 1];
static uint8_t *GVRAM = 0;   /* グラフィックVRAM (set_videomodeで登録) */
static uint8_t *Fontp = 0;   /* 現在のフォント先頭 (呼び出し側が設定) */

/* モード切替の管理用 (旧ドライバ相当。ntscvは直接は使わない) */
static unsigned char textmode = TMODE_WIDETEXT;
static unsigned char graphmode = GMODE_NOGRPH;
static int twidth = WIDTH_X;
static int twidthy = WIDTH_Y;
static int attroffset = ATTROFFSET;

/*==== 旧関数の互換 ===*/

/* テキスト画面クリア: TVRAM全体 (コード+属性エリア) をゼロにする */
static void clearscreen(void)
{
    memset(TVRAM, 0, sizeof TVRAM);
}

/* グラフィック画面クリア (GVRAM未登録なら何もしない) */
static void g_clearscreen(void)
{
    if (GVRAM) memset(GVRAM, 0, X_RES * Y_RES);
}

static void start_composite(void) { ntscv_resume(); }
static void stop_composite(void) { ntscv_stop(); }
static int ntsc_active(void) { return ntscv_active(); }
static void ntsc_release(void) { ntscv_release(); }
static void ntsc_set_line_callback(void (*cb)(void)) { ntscv_set_line_hook(cb); }
static uint16_t ntsc_scanline(void) { return ntscv_debug_scanline(); }
static void set_palette(uint16_t c, unsigned char b, unsigned char r, unsigned char g)
{
    ntscv_palette_set(c, b, r, g);
}

/* RGB332 (MSX風16色) → ntscvパレット (b,r,g順) へ変換して設定 */
static inline void ntsc_set_color_palette(uint16_t n, uint8_t rgb332)
{
    uint8_t r = (rgb332 >> 5) & 7;
    uint8_t g = (rgb332 >> 2) & 7;
    uint8_t b = rgb332 & 3;
    ntscv_palette_set(n, (uint8_t)(b * 255 / 3), (uint8_t)(r * 255 / 7), (uint8_t)(g * 255 / 7));
}

/* screen.h のパレット再設定 (COLOR用)。screen.hは本ファイルより後にincludeされるため前方宣言 */
void screen_reset_palette(void);

/* 初期化に使った音声/映像ピン (再初期化で再利用) */
static uint8_t ntsc_pin_num = 0;

/* COLORコマンド用のモード切替: 起動時と同じ経路でNTSCエンジンをフル再初期化する。
   softにモードだけ変えると、走査線ヘッド(アンカー)のカラーバーストが前フィールドの
   ままになりTVが色/同期をロックできず「横線が流れる/フラッシュ」になるため、
   release→mode設定→ntscv_init でヘッドから作り直す。 */
static void ntsc_restart_mode(unsigned char m)
{
    ntscv_mode = m;
    if (m == NTSCV_MODE_COLOR_TEXT) {
        ntscv_bind_text(TVRAM, TVRAM + ATTROFFSET, WIDTH_X, WIDTH_Y);
    } else {
        ntscv_bind_text(TVRAM, TVRAM + IJ_ATTROFFSET, IJ_WIDTH, IJ_ROWS);
    }
    clearscreen();
    ntscv_init(ntsc_pin_num); /* 内部でreleaseして再初期化 (mode設定済→初期ヘッドにバースト) */
    screen_reset_palette();
}

/* ビデオ出力システムを初期化し開始する (旧rp2040_pwm_ntsc_init相当)。
   呼び出し前に Fontp が設定済みであること。
   起動時はIchigoJam互換IJTEXTモードで直接始める (同期を乱さない)。 */
static void rp2040_pwm_ntsc_init(uint8_t n)
{
    ntsc_pin_num = n;
    ntscv_bind_font(Fontp);
    ntscv_bind_text(TVRAM, TVRAM + IJ_ATTROFFSET, IJ_WIDTH, IJ_ROWS);
    ntscv_mode = VMODE_IJTEXT;
#ifdef NTSCV_BOOT_TESTPATTERN
    /* ビルド時に -DNTSCV_BOOT_TESTPATTERN=1 (カラーバー) / =2 (グリッド)
       を指定すると起動時にテストパターンを表示する (映像調整用) */
    ntscv_test_pattern(NTSCV_BOOT_TESTPATTERN);
#endif
    ntscv_init(n);
}

/* ビデオモードの切り替え (旧set_videomode相当)。
   m: モード、gvram: グラフィック用メモリ先頭 (グラフィック使用時のみ) */
static void set_videomode(unsigned char m, unsigned char *gvram)
{
    if (videomode == m) return;
    switch (m) {
    case VMODE_WIDETEXT:
        ntscv_bind_text(TVRAM, TVRAM + ATTROFFSET, WIDTH_X, WIDTH_Y);
        clearscreen();
        textmode = TMODE_WIDETEXT;
        twidth = WIDTH_X;
        attroffset = ATTROFFSET;
        graphmode = GMODE_NOGRPH;
        break;
    case VMODE_MONOTEXT:
        ntscv_bind_text(TVRAM, TVRAM + ATTROFFSETBW, WIDTH_XBW, WIDTH_Y);
        clearscreen();
        textmode = TMODE_MONOTEXT;
        twidth = WIDTH_XBW;
        attroffset = ATTROFFSETBW;
        graphmode = GMODE_NOGRPH;
        break;
    case VMODE_IJTEXT:
        ntscv_bind_text(TVRAM, TVRAM + IJ_ATTROFFSET, IJ_WIDTH, IJ_ROWS);
        clearscreen();
        twidth = IJ_WIDTH;
        twidthy = IJ_ROWS;
        attroffset = IJ_ATTROFFSET;
        graphmode = GMODE_NOGRPH;
        break;
    case VMODE_WIDEGRPH:
        ntscv_bind_text(TVRAM, TVRAM + ATTROFFSET, WIDTH_X, WIDTH_Y);
        clearscreen();
        GVRAM = gvram;
        ntscv_bind_graph(gvram);
        g_clearscreen();
        textmode = TMODE_WIDETEXT;
        twidth = WIDTH_X;
        attroffset = ATTROFFSET;
        graphmode = GMODE_WIDEGRPH;
        break;
    default:
        return;
    }
    videomode = m;
}

#endif /* NTSC_COMPAT_H */
