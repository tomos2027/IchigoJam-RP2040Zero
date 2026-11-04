/*----------------------------------------------------------------------------

ntscv_render.c — 走査線レンダラ (docs/ntsc_video_spec.md §3, §5)

Copyright (c) 2026 the IchigoJam-RP2040Zero authors. MIT license.

映像割込み内で実行するため、本ファイルの関数はすべてRAM配置
(__not_in_flash_func → .time_critical) とし、XIPフラッシュの
実行待ちでライン期限 (127µs) が揺らがないようにする。

バッファ永続化契約 (1DMA転送=2走査線):
  - 有効映像ラインは内容部 (172..843) のみ書き換える。同期+ポーチ部と
    末尾はブランキング領域の全書き込み (ライン10-13) で書かれた内容が
    フィールド中ずっとバッファに残り、毎ライン再利用される。
  - 垂直同期は先頭4ライン、各ブランキングは先頭4ラインのみ全書き込みし、
    残りは書き込まない (2チャンク前の内容がそのまま再生される)。
  - ntscv_running=0 の間は一切書き込まない (ntscv_stop はエンジン自体を
    解放するため通常到達しない安全策。停止中は同期信号も出ない)。

VRAMは呼び出し側が所有・登録した配列を読むだけ (ライブラリは所有しない)。

----------------------------------------------------------------------------*/

#include "ntscv.h"
#include <string.h>
#include <assert.h>

/* RAM配置: 本memmapでは .time_critical が .data (RAM) に入る。
   ホスト側ユニットテスト (pico-sdk無し) では関数名をそのまま返す
   空の互換定義に置き換える (pico-sdkのhost定義と同じ形式)。 */
#if __has_include("pico.h")
#include "pico.h" /* __not_in_flash_func (pico/platform/sections.h) ため */
#else
#ifndef __not_in_flash_func
#define __not_in_flash_func(func_name) func_name
#endif
#endif

uint8_t *ntscv_vram_codes;
uint8_t *ntscv_vram_attrs;
const uint8_t *ntscv_font;
uint8_t *ntscv_vram_graph;
volatile uint8_t ntscv_test_mode;

static int text_cols, text_rows;

void ntscv_bind_text(uint8_t *codes, uint8_t *attrs, int cols, int rows)
{
    ntscv_vram_codes = codes;
    ntscv_vram_attrs = attrs;
    text_cols = cols;
    text_rows = rows;
}

void ntscv_bind_graph(uint8_t *vram)
{
    ntscv_vram_graph = vram;
}

void ntscv_bind_font(const uint8_t *font)
{
    ntscv_font = font;
}

void ntscv_clear_text(void)
{
    if (!ntscv_vram_codes || !ntscv_vram_attrs || text_cols <= 0) return;
    memset(ntscv_vram_codes, 0, (size_t)text_cols * text_rows);
    memset(ntscv_vram_attrs, 0, (size_t)text_cols * text_rows);
}

/*---- 基本ヘルパ ----*/

/* 偶数個のサンプルを32ビット書き込み×2サンプルで埋める (書込回数半減) */
static inline void __not_in_flash_func(fill_samples)(uint16_t *dst, int n, uint16_t v)
{
    uint32_t *p = (uint32_t *)dst;
    uint32_t v32 = (uint32_t)v * 0x10001u; /* リトルエンディアンで2サンプル分 */
    int pairs = n >> 1;
    while (pairs--) *p++ = v32;
}

/* カラーバースト1周期分 (32ビットペア×2): 黒レベル中心に -sin(90°k)。
   振幅±1段 ≈ 29IRE p.p (10段D/Aへの量子化結果)。
   サンプル値 [2,1] [2,3] をリトルエンディアンで32ビット化 */
#define NTSCV_BURST_PAIR01 ((((uint32_t)(NTSCV_LEVEL_BLACK - 1)) << 16) | NTSCV_LEVEL_BLACK)
#define NTSCV_BURST_PAIR23 ((((uint32_t)(NTSCV_LEVEL_BLACK + 1)) << 16) | NTSCV_LEVEL_BLACK)

/* ライン先頭の同期+ポーチ類 (計172サンプル) を全書き込みする。
   with_burst=true なら規格どおりブリーズウェイ8サンプル後に
   9周期のカラーバーストを置く (カラーモードとテストパターン1で使用)。
   po: このラインの副搬送波位相オフセット (0 または 2)。
   1走査線=910サンプル=227.5周期のため、ラインの先頭サンプルの位相は
   隣接ラインで180°交互 (po = (ライン番号&1)*2)。バーストもこれに追従し、
   実NTSCと同じライン構造にする (TVのY/C分離=コムフィルタと整合)。 */
static void __not_in_flash_func(emit_head)(uint16_t *dst, bool with_burst, int po)
{
    fill_samples(dst, NTSCV_H_SYNC_SAMPLES, NTSCV_LEVEL_SYNC);
    if (!with_burst) {
        fill_samples(dst + NTSCV_H_SYNC_SAMPLES,
                     NTSCV_LINE_HEAD_SAMPLES - NTSCV_H_SYNC_SAMPLES,
                     NTSCV_LEVEL_BLACK);
        return;
    }
    fill_samples(dst + NTSCV_H_SYNC_SAMPLES, NTSCV_BREEZEWAY_SAMPLES, NTSCV_LEVEL_BLACK);
    uint32_t *p = (uint32_t *)(dst + NTSCV_H_SYNC_SAMPLES + NTSCV_BREEZEWAY_SAMPLES);
    if (po == 0) {
        for (int c = 0; c < NTSCV_BURST_CYCLES; c++) {
            *p++ = NTSCV_BURST_PAIR01; /* 位相0-1 */
            *p++ = NTSCV_BURST_PAIR23; /* 位相2-3 */
        }
    } else {
        for (int c = 0; c < NTSCV_BURST_CYCLES; c++) {
            *p++ = NTSCV_BURST_PAIR23; /* 位相2-3 (po=2で180°交互) */
            *p++ = NTSCV_BURST_PAIR01; /* 位相0-1 */
        }
    }
    fill_samples(dst + NTSCV_H_SYNC_SAMPLES + NTSCV_BREEZEWAY_SAMPLES
                     + NTSCV_BURST_CYCLES * 4,
                 NTSCV_LINE_HEAD_SAMPLES - NTSCV_H_SYNC_SAMPLES
                     - NTSCV_BREEZEWAY_SAMPLES - NTSCV_BURST_CYCLES * 4,
                 NTSCV_LEVEL_BLACK);
}

/*---- IJTEXT: 32×24 白黒、1文字21サンプル×9走査線 (仕様書§3.1) ----*/

/* 8ドットを21サンプルへ均等配分するドット幅。
   幅[j] = floor(21(j+1)/8) - floor(21j/8) → 2,3,2,3,3,2,3,3
   各ドットは基本2サンプル、幅3のドット (j=1,3,4,6,7) だけ
   1サンプル追加する。起動時に floor式から計算して1バイトのマスクに
   留める (割込み内のテーブル参照と分岐を最小化するため)。
   実行時計算のためRAM配置が保証され、フラッシュ参照も起きない */
static uint8_t ij_extra_mask; /* bit7=j0 … bit0=j7、幅3のドットに1 */

/* ライン (0..215) → 文字行 (0..23) の変換表。
   割込み内で libgccの __aeabi_idiv (フラッシュ配置) を呼ばないため、
   起動時に一度だけ作るRAM表 (9は2の冪でないため除算をシフトに置換できない)。
   行内走査線 tl は l - row*9 (定数倍はシフト加算で済む) */
static uint8_t ij_row_of_line[NTSCV_FIELD_ACTIVE_LINES];

void ntscv_render_boot_init(void)
{
    for (int l = 0; l < NTSCV_FIELD_ACTIVE_LINES; l++) {
        ij_row_of_line[l] = (uint8_t)(l / NTSCV_IJ_LINES_PER_ROW);
    }
    ij_extra_mask = 0;
    for (int j = 0; j < 8; j++) {
        uint8_t width = (uint8_t)((21 * (j + 1) >> 3) - (21 * j >> 3));
        if (width == 3) ij_extra_mask |= (uint8_t)(0x80 >> j);
    }
    ntscv_render_palette_changed(); /* IJカラー描画表 (パレット変更時も再構築される) */
}

/* w: 有効画素域の先頭 (サンプル172)。l: 有効映像内の走査線 0..215 */
static void __not_in_flash_func(render_ij)(uint16_t *w, int l)
{
    int row = ij_row_of_line[l];
    int tl = l - row * NTSCV_IJ_LINES_PER_ROW;
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_IJ_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_IJ_COLS;

    /* 拡大時はVRAM側で1文字がzoom×zoomセルに複製されている。
       各セルはグリフの一部 (横 8>>zsh ドット、縦: グループ内行位置) を担当。
       セルの走査線tl → フォント行: ((row%zoom)*9 + tl) >> zsh */
    int zoom = ntscv_ij_zoom;
    int zsh = (zoom == 8) ? 3 : (zoom == 4) ? 2 : (zoom == 2) ? 1 : 0;
    int z = ((row & (zoom - 1)) * NTSCV_IJ_LINES_PER_ROW + tl) >> zsh;
    int fontline = (z < 8) ? z : 7;

    if (zoom == 1) {
        for (int i = 0; i < NTSCV_IJ_COLS; i++) {
            uint8_t bits = ntscv_font[code[i] * 8 + fontline];
            uint8_t a = attr[i];
            if (a & NTSCV_IJ_ATTR_INVERT) bits = ~bits;
            if (a & NTSCV_IJ_ATTR_CURSOR) bits ^= ntscv_ij_cursor_xor;
            uint8_t mask = ij_extra_mask;
            for (int j = 0; j < 8; j++) {
                uint16_t s = (bits & 0x80) ? NTSCV_LEVEL_WHITE : NTSCV_LEVEL_BLACK;
                *w++ = s;
                *w++ = s;
                if (mask & 0x80) *w++ = s; /* 幅3のドットは1サンプル追加 */
                mask <<= 1;
                bits <<= 1;
            }
        }
        return;
    }
    int ndots = 8 >> zsh;        /* このセルが担当するグリフ列数 */
    int dotw = 20 >> (3 - zsh);  /* 1ドットのサンプル幅: zoom 2→5, 4→10, 8→20 */
    for (int i = 0; i < NTSCV_IJ_COLS; i++) {
        uint8_t bits = ntscv_font[code[i] * 8 + fontline];
        uint8_t a = attr[i];
        if (a & NTSCV_IJ_ATTR_INVERT) bits = ~bits;
        if (a & NTSCV_IJ_ATTR_CURSOR) bits ^= ntscv_ij_cursor_xor;
        bits <<= (i & (zoom - 1)) * ndots; /* セル列位置が担当するグリフ断片 */
        uint16_t s_last = NTSCV_LEVEL_BLACK;
        for (int j = 0; j < ndots; j++) {
            uint16_t s = (bits & 0x80) ? NTSCV_LEVEL_WHITE : NTSCV_LEVEL_BLACK;
            for (int k = 0; k < dotw; k++) *w++ = s;
            s_last = s;
            bits <<= 1;
        }
        *w++ = s_last; /* 21サンプル目を最終ドット色で延長しグリフ間に縦線が出ないようにする */
    }
}

/*---- IJTEXTカラー: 32×24、セル属性 (下位4bit=前景, 上位4bit=背景) ----
  DMA合成IRQは1チャンク (2走査線) を127µs以内に処理する必要がある (持続レート)。
  毎サンプルのパレット参照/位相演算はこの予算を超過し、割込み合流で画面が
  破綻するため、出力値を起動時/パレット変更時に事前計算した表引きのみにする。
  幾何は単色IJTEXTと完全同一 (1文字21サンプル、ドット幅2,3,2,3,3,2,3,3、
  文字位置 i の開始位相 = (21i + po) %4 = (i + po) %4)。
  色は「白レベル輝度+セル単位ティント」:
   - 輝度: 前景ドット=白(9) / 背景ドット=黒(2)。VIDEO1と同一の輝度構造なので
     現代TVのノイズリダクションによる中間輝度のドット欠けが起きない
     (実機確認: 色本来の輝度3〜8ではドットが欠ける)。
   - 色差: セル全体 (前景+背景ドット) に前景色の色差を一定に載せる。
     隣接走査線で色差が相関するためTVの2ラインコムフィルタが抽出できる
     (ドット単位の色差は走査線間で非相関のため抽出されない — 実機確認:
     走査線一定のカラーバーは見えるがドット単位の色は見えない)。*/

/* [前景/背景][色][開始位相][サンプル0..20] (+1パディングで先頭4バイト整列)。
   前景 = 白レベル9+色差±1 (文字はほぼ白)、背景 = 黒レベル2+フル色差 (セルの色)。
   セル全体 (前景ドット+背景ドット) に同一の色差パターンを載せることで、隣接
   走査線で色差が相関し、TVの2ラインコムフィルタが色を抽出できる。
   ドット単位の色差 (フォントビットで色差の位置が走査線ごとに変わる) は
   走査線間で非相関のためコムフィルタで抽出されず色が見えない (実機確認済み:
   走査線一定のカラーバーは見えるがドット単位の色差は見えない)。 */
static uint16_t ij_color_run[2][16][4][22] __attribute__((aligned(4)));

/* 有彩色か (生レベルに色差があるか) とセルのティント色選択表 */
static uint8_t ij_color_chromatic[16];
static uint8_t ij_color_tint_sel[256];

/* パレット変更 (PALコマンド/初期化) を描画表へ反映する */
void __not_in_flash_func(ntscv_render_palette_changed)(void)
{
    for (int c = 0; c < 16; c++) {
        uint16_t s[4];
        /* ティントの色差は視認性ルール無しの生パレットから取る
           (ルール2のドット平均保護は白レベル輝度方式では不要で、
            適用すると色差が弱くなるため) */
        s[0] = (uint16_t)ntscv_palette_plain[c][0];
        s[1] = (uint16_t)(ntscv_palette_plain[c][0] >> 16);
        s[2] = (uint16_t)ntscv_palette_plain[c][1];
        s[3] = (uint16_t)(ntscv_palette_plain[c][1] >> 16);
        int32_t sum = s[0] + s[1] + s[2] + s[3];
        int32_t mx = s[0], mn = s[0];
        for (int k = 1; k < 4; k++) {
            if (s[k] > mx) mx = s[k];
            if (s[k] < mn) mn = s[k];
        }
        int32_t cvec[4];
        if (mx != mn) { /* 有彩色: 色差ベクトル (生レベル - 平均) */
            ij_color_chromatic[c] = 1;
            int32_t mean = (sum + 2) >> 2;
            for (int k = 0; k < 4; k++) cvec[k] = (int32_t)s[k] - mean;
        } else { /* 無彩色 (黒/白/灰): 色差なし */
            ij_color_chromatic[c] = 0;
            for (int k = 0; k < 4; k++) cvec[k] = 0;
        }
        for (int po = 0; po < 4; po++) {
            for (int k = 0; k < NTSCV_IJ_COLOR_CHAR_SAMPLES; k++) {
                int32_t ck = cvec[(po + k) & 3];
                /* 前景: 白レベル+弱い色差 (±1にクランプ)。フル振幅の色差を前景
                   ドットに載せるとサンプル値が9±3で揺れ、Y/C分離の漏れで文字の
                   中に明暗の縞 (色のむら) が出て読みづらい (実機確認)。
                   除算を使わず分岐クランプで±1に丸める */
                int32_t cs = (ck > 1) ? 1 : ((ck < -1) ? -1 : ck);
                int32_t vf = NTSCV_LEVEL_WHITE + cs;
                int32_t vb = NTSCV_LEVEL_BLACK + ck; /* 背景: 黒レベル+フル色差 */
                if (vf < 0) vf = 0;
                if (vf > 10) vf = 10;
                if (vb < 1) vb = 1; /* レベル0 (同期レベル) は避ける */
                if (vb > 10) vb = 10;
                ij_color_run[0][c][po][k] = (uint16_t)vf;
                ij_color_run[1][c][po][k] = (uint16_t)vb;
            }
        }
    }
    /* ティント色の選択表: 前景が有彩色なら前景色、無彩色なら背景色
       (COLOR 15,4 のような背景色指定でも色が出るように) */
    for (int a = 0; a < 256; a++) {
        uint8_t fg = (uint8_t)(a & 15), bg = (uint8_t)((a >> 4) & 15);
        ij_color_tint_sel[a] = ij_color_chromatic[fg] ? fg : bg;
    }
}

static void __not_in_flash_func(render_ij_color)(uint16_t *w, int l, int po)
{
    int row = ij_row_of_line[l];
    int tl = l - row * NTSCV_IJ_LINES_PER_ROW;
    int fontline = (tl < 8) ? tl : 7; /* 9走査線 → 8ラインフォント (zoom=1固定) */
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_IJ_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_IJ_COLS;
    for (int i = 0; i < NTSCV_IJ_COLS; i++) {
        uint8_t a = attr[i];
        int pi = (po + i) & 3; /* 文字位置+ライン位相 → 開始位相 (21≡1 mod 4) */
        /* セルのティント色 (前景が有彩色なら前景色、無彩色なら背景色) の
           前景バリアント (白レベル+色差) と背景バリアント (黒レベル+同じ色差) */
        uint8_t tc = ij_color_tint_sel[a];
        const uint16_t *fg = ij_color_run[0][tc][pi];
        const uint16_t *bg = ij_color_run[1][tc][pi];
        if (((a >> 4) & 15) == (a & 15)) bg = fg; /* 前景=背景: 一様なセル */
        uint8_t bits = ntscv_font[code[i] * 8 + fontline];
        if (bits == 0 || fg == bg) {
            /* 全ドット同色 (空白セルが大半): 表から一括複製する。
               出力位置が32bit整列のときは32bit転送を使う */
            if (((uintptr_t)w & 2) == 0) {
                const uint32_t *src = (const uint32_t *)bg; /* 表先頭は4バイト整列 */
                uint32_t *d = (uint32_t *)w;
                d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
                d[4] = src[4]; d[5] = src[5]; d[6] = src[6]; d[7] = src[7];
                d[8] = src[8]; d[9] = src[9];
                w[20] = bg[20];
            } else {
                for (int k = 0; k < NTSCV_IJ_COLOR_CHAR_SAMPLES; k++) w[k] = bg[k];
            }
            w += NTSCV_IJ_COLOR_CHAR_SAMPLES;
            continue;
        }
        /* ドット幅 2,3,2,3,3,2,3,3 → サンプル位置は定数 (表引き+ストアのみ) */
        const uint16_t *p;
        p = (bits & 0x80) ? fg : bg; w[0] = p[0]; w[1] = p[1];
        p = (bits & 0x40) ? fg : bg; w[2] = p[2]; w[3] = p[3]; w[4] = p[4];
        p = (bits & 0x20) ? fg : bg; w[5] = p[5]; w[6] = p[6];
        p = (bits & 0x10) ? fg : bg; w[7] = p[7]; w[8] = p[8]; w[9] = p[9];
        p = (bits & 0x08) ? fg : bg; w[10] = p[10]; w[11] = p[11]; w[12] = p[12];
        p = (bits & 0x04) ? fg : bg; w[13] = p[13]; w[14] = p[14];
        p = (bits & 0x02) ? fg : bg; w[15] = p[15]; w[16] = p[16]; w[17] = p[17];
        p = (bits & 0x01) ? fg : bg; w[18] = p[18]; w[19] = p[19]; w[20] = p[20];
        w += NTSCV_IJ_COLOR_CHAR_SAMPLES;
    }
}

/*---- カラーテキスト: 42×27、1ドット2サンプル (仕様書§3.2) ----
  1文字16サンプル = 搬送波4周期。ドットごとに位相が180°進行するため
  偶数ドット=パレット位相0-1、奇数ドット=位相2-3 を使う。
  po (ライン位相オフセット 0/2) が2のラインでは偶数ドット=位相2-3となり
  ペアの選択を入れ替える (実NTSCのライン毎180°交互に追従)。
*/
static void __not_in_flash_func(render_color_text)(uint16_t *w, int l, int po)
{
    int row = l >> 3;
    int fline = l & 7;
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_COLOR_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_COLOR_COLS;
    int wsel = po >> 1; /* 偶数ドットが使う32bitペアの番号 (0/1) */
    uint32_t *w32 = (uint32_t *)w;
    for (int i = 0; i < NTSCV_COLOR_COLS; i++) {
        uint8_t bits = ntscv_font[code[i] * 8 + fline];
        uint8_t a = attr[i];
        const uint32_t *fg = ntscv_palette[a & 15];
        const uint32_t *bg = ntscv_palette[(a >> 4) & 15];
        for (int j = 0; j < 4; j++) {
            *w32++ = (bits & 0x80) ? fg[wsel] : bg[wsel];
            *w32++ = (bits & 0x40) ? fg[wsel ^ 1] : bg[wsel ^ 1];
            bits <<= 2;
        }
    }
}

/*---- カラーグラフィック+テキスト: 336×216に42×27文字を重ねる (仕様書§3.3) ----
  GVRAMは1画素1バイトのパレット番号。フォントビットが立っている画素は文字色優先。
*/
static void __not_in_flash_func(render_color_graph)(uint16_t *w, int l, int po)
{
    if (!ntscv_vram_graph) {
        render_color_text(w, l, po); /* グラフィックVRAM未登録ならテキストのみ */
        return;
    }
    int row = l >> 3;
    int fline = l & 7;
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_COLOR_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_COLOR_COLS;
    const uint8_t *gline = ntscv_vram_graph + l * NTSCV_GRAPH_COLS;
    int wsel = po >> 1;
    uint32_t *w32 = (uint32_t *)w;
    for (int i = 0; i < NTSCV_COLOR_COLS; i++) {
        uint8_t bits = ntscv_font[code[i] * 8 + fline];
        const uint32_t *fg = ntscv_palette[attr[i]];
        const uint8_t *gp = gline + i * 8;
        for (int j = 0; j < 4; j++) {
            *w32++ = (bits & 0x80) ? fg[wsel] : ntscv_palette[gp[j * 2]][wsel];
            *w32++ = (bits & 0x40) ? fg[wsel ^ 1] : ntscv_palette[gp[j * 2 + 1]][wsel ^ 1];
            bits <<= 2;
        }
    }
}

/*---- モノクロテキスト: 80×27、1ドット1サンプル (仕様書§3.4) ----*/

static void __not_in_flash_func(render_mono80)(uint16_t *w, int l)
{
    int row = l >> 3;
    int fline = l & 7;
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_MONO80_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_MONO80_COLS;
    for (int i = 0; i < NTSCV_MONO80_COLS; i++) {
        uint8_t bits = ntscv_font[code[i] * 8 + fline];
        if (attr[i] & 0x80) bits = ~bits;
        for (int j = 0; j < 8; j++) {
            *w++ = (bits & 0x80) ? NTSCV_LEVEL_WHITE : NTSCV_LEVEL_BLACK;
            bits <<= 1;
        }
    }
}

/*---- テストパターン (仕様書§3.5) ----*/

/* SMPTEバー7色の4位相ペア (白黄シアン緑マゼンタ赤青) */
static uint32_t bar_pair[7][2];

static void diag_prepare(void); /* パターン3用の合成データ初期化 (後方定義) */

void ntscv_test_pattern(int p)
{
    if (p == 1) {
        static const uint8_t rgb[7][3] = { /* {青,赤,緑} */
            { 255, 255, 255 }, /* 白     */
            { 0,   255, 255 }, /* 黄     */
            { 255, 0,   255 }, /* シアン */
            { 0,   0,   255 }, /* 緑     */
            { 255, 255, 0   }, /* マゼンタ */
            { 0,   255, 0   }, /* 赤     */
            { 255, 0,   0   }, /* 青     */
        };
        for (int i = 0; i < 7; i++) {
            uint16_t s[4];
            ntscv_palette_calc(s, rgb[i][0], rgb[i][1], rgb[i][2]);
            bar_pair[i][0] = (uint32_t)s[1] << 16 | s[0];
            bar_pair[i][1] = (uint32_t)s[3] << 16 | s[2];
        }
    }
    if (p == 3) {
        diag_prepare(); /* IJ経路診断用の合成データ (先に初期化してから表示に切り替える) */
    }
    ntscv_test_mode = (uint8_t)p;
}

/* パターン1: 上位7バー (各96サンプル=24搬送波周期) + 下位グレースケール8段 */
static void __not_in_flash_func(render_bars)(uint16_t *w, int l, int po)
{
    int wsel = po >> 1; /* ライン位相オフセットでペア選択を入れ替える */
    uint32_t *w32 = (uint32_t *)w;
    if (l < NTSCV_FIELD_ACTIVE_LINES - 27) {
        for (int bar = 0; bar < 7; bar++) {
            for (int c = 0; c < 24; c++) {
                *w32++ = bar_pair[bar][wsel];
                *w32++ = bar_pair[bar][wsel ^ 1];
            }
        }
        return;
    }
    for (int step = NTSCV_LEVEL_BLACK; step <= NTSCV_LEVEL_WHITE; step++) {
        uint32_t v = (uint32_t)step << 16 | step;
        for (int k = 0; k < 42; k++) *w32++ = v; /* 84サンプル = 42ペア */
    }
}

/* パターン2: 枠+格子 (84サンプル=1文字半分間隔) */
static void __not_in_flash_func(render_grid)(uint16_t *w, int l)
{
    if (l == 0 || l == 107 || l == NTSCV_FIELD_ACTIVE_LINES - 1) {
        fill_samples(w, NTSCV_ACTIVE_SAMPLES, NTSCV_LEVEL_WHITE);
        return;
    }
    for (int col = 0; col < 8; col++) {
        *w++ = NTSCV_LEVEL_WHITE;
        for (int i = 1; i < 84; i++) *w++ = NTSCV_LEVEL_BLACK;
    }
    w[-1] = NTSCV_LEVEL_WHITE; /* 右端の枠線 */
}

/*---- テストパターン3: IJ経路診断画面 (実行時データパスの切り分け) ----

  24行を3バンド+黒セパレータに分け、各バンドでrender_ijへの入力ソースを
  差し替えて描く。どこまで表示されるかで故障箇所を特定できる:

    行0-6   : ライブラリ内の合成データ (フォント全0xFF/コード'A') → 白一色になるはず
    行7     : 黒セパレータ
    行8-14  : 実フォント (ntscv_font) + 合成コード → 'A'のグリフが見えるはず
    行15    : 黒セパレータ
    行16-23 : 実TVRAM (コード+属性そのまま) → タイトル文字 or "E nn" が見えるはず
*/

static uint8_t diag_font[256 * 8];                 /* すべて0xFF (どのコードも白一色) */
static uint8_t diag_codes[NTSCV_IJ_COLS * NTSCV_IJ_ROWS]; /* すべて'A' */
static uint8_t diag_attrs[NTSCV_IJ_COLS * NTSCV_IJ_ROWS]; /* すべて0 */

static void diag_prepare(void)
{
    memset(diag_font, 0xFF, sizeof diag_font);
    memset(diag_codes, 'A', sizeof diag_codes);
    memset(diag_attrs, 0, sizeof diag_attrs);
}

static void __not_in_flash_func(render_diag)(uint16_t *w, int l)
{
    int row = ij_row_of_line[l]; /* 割り算不使用: 変換表 */
    if (row == 7 || row == 15) {
        /* セパレータは白: ここが見えなければrender_ij以前の問題 */
        fill_samples(w, NTSCV_ACTIVE_SAMPLES, NTSCV_LEVEL_WHITE);
        return;
    }
    /* render_ijが読むグローバルを一時的に差し替え、描画後に復元する
       (割込み内で完結するため、外部への副作用はない) */
    const uint8_t *save_font = ntscv_font;
    uint8_t *save_codes = ntscv_vram_codes;
    uint8_t *save_attrs = ntscv_vram_attrs;
    if (row < 7) {
        ntscv_font = diag_font;            /* バンド1: すべて合成 */
        ntscv_vram_codes = diag_codes;
        ntscv_vram_attrs = diag_attrs;
        render_ij(w, l);
    } else if (row < 15) {
        ntscv_vram_codes = diag_codes;     /* バンド2: 実フォント+合成コード */
        ntscv_vram_attrs = diag_attrs;
        render_ij(w, l - 8 * NTSCV_IJ_LINES_PER_ROW);
    } else {
        render_ij(w, l - 16 * NTSCV_IJ_LINES_PER_ROW); /* バンド3: 実TVRAM */
    }
    ntscv_font = save_font;
    ntscv_vram_codes = save_codes;
    ntscv_vram_attrs = save_attrs;
}

/*---- チャンク組み立て本体 (2走査線分) ----*/

/* 安全チャンク: DMAがバッファ終端からはみ出して再生した場合に
   流れる「同期+黒」の有効な2走査線分 (core.cが起動時に1度だけ作る)。
   白いゴミ (ガード値・隣接RAM) が出るのを防ぐ。 */
void ntscv_compose_safety_chunk(uint16_t *dst)
{
    for (int i = 0; i < NTSCV_CHUNK_LINES; i++) {
        uint16_t *lp = dst + (size_t)i * NTSCV_SAMPLES_PER_LINE;
        emit_head(lp, false, 0);
        fill_samples(lp + NTSCV_LINE_HEAD_SAMPLES,
                     NTSCV_SAMPLES_PER_LINE - NTSCV_LINE_HEAD_SAMPLES,
                     NTSCV_LEVEL_BLACK);
    }
}

void __not_in_flash_func(ntscv_compose_chunk)(uint16_t *dst, uint16_t first_line)
{
    /* 有効映像の直後のラインでフレームシーケンスを進める (旧drawcount互換)。
       停止中も進めるので映像待ちループが固まらない。 */
    if (first_line == NTSCV_FRAME_MARK_LINE) ntscv_frame_seq++;

    /* 停止中は書き込まない (ntscv_stopはエンジンを解放するため通常は不到達の安全策) */
    if (!ntscv_running) return;

    /* カラーバーストを載せるか: カラーモード、またはカラーバー表示中 */
    bool with_burst = (ntscv_mode == NTSCV_MODE_COLOR_TEXT) ||
                      (ntscv_mode == NTSCV_MODE_COLOR_GRAPH) ||
                      (ntscv_mode == NTSCV_MODE_IJ_COLOR) ||
                      (ntscv_test_mode == 1);

    /* ライン位相オフセット: 1走査線=227.5周期のためライン毎に180°交互
       (実NTSC規格。TVのY/C分離=コムフィルタと整合する)。
       フィールドは262ライン (偶数) なのでフィールド相対ライン番号の
       偶奇=グローバル偶奇。バッファの再生間隔も8ライン (偶数) なので
       チャネルリングでヘッドの位相が崩れない。 */

    for (int i = 0; i < NTSCV_CHUNK_LINES; i++) {
        uint16_t line = first_line + (uint16_t)i;
        uint16_t *lp = dst + (size_t)i * NTSCV_SAMPLES_PER_LINE;
        int lpo = (int)(line & 1) << 1; /* このラインの位相オフセット */

        if (line < NTSCV_FIELD_VSYNC_LINES) {
            /* 垂直同期: 先頭8ライン (4チャネル分) だけ全書き込み。
               残り2ラインは各チャネルが4スロット前の自バッファ内容を再生 */
            if (line < 8) {
                fill_samples(lp, NTSCV_SAMPLES_PER_LINE - NTSCV_H_SYNC_SAMPLES,
                             NTSCV_LEVEL_SYNC);
                fill_samples(lp + NTSCV_SAMPLES_PER_LINE - NTSCV_H_SYNC_SAMPLES,
                             NTSCV_H_SYNC_SAMPLES, NTSCV_LEVEL_BLACK);
            }
            continue;
        }

        if (line < NTSCV_ACTIVE_START) {
            /* ブランキング (前): 先頭8ライン (10-17) だけ全書き込みし
               各チャネルのヘッドを確立。残りは再生 */
            if (line < NTSCV_ACTIVE_START - 18) { /* ライン10-17 */
                emit_head(lp, with_burst, lpo);
                fill_samples(lp + NTSCV_LINE_HEAD_SAMPLES,
                             NTSCV_SAMPLES_PER_LINE - NTSCV_LINE_HEAD_SAMPLES,
                             NTSCV_LEVEL_BLACK);
            }
            continue;
        }

        if (line < NTSCV_FRAME_MARK_LINE) {
            /* 有効映像: 内容部のみ書き換え。ヘッド/末尾はライン10-17の
               全書き込みが永続する。有効画素の位置/幅はモード別 (ntscv.h)。
               32桁カラー (IJ_COLOR) は単色IJTEXTと同一幾何。テストパターンも
               単色IJTEXT幾何 (172/672) で描く */
            int astart = NTSCV_IJ_TEXT_START, awidth = NTSCV_ACTIVE_SAMPLES;
            if (ntscv_test_mode == 0) {
                if (ntscv_mode == NTSCV_MODE_COLOR_TEXT ||
                    ntscv_mode == NTSCV_MODE_COLOR_GRAPH) {
                    astart = NTSCV_COLOR_START;
                    awidth = NTSCV_COLOR_ACTIVE_SAMPLES;
                } else if (ntscv_mode == NTSCV_MODE_MONO_TEXT) {
                    astart = NTSCV_MONO80_START;
                    awidth = NTSCV_MONO80_ACTIVE_SAMPLES;
                }
            }
            uint16_t *act = lp + astart;
            int l = (int)line - NTSCV_ACTIVE_START;
            if (!ntscv_font || !ntscv_vram_codes || !ntscv_vram_attrs) {
                fill_samples(act, awidth, NTSCV_LEVEL_BLACK);
            } else {
                int tp = ntscv_test_mode;
                if (tp == 1) {
                    render_bars(act, l, lpo);
                } else if (tp == 2) {
                    render_grid(act, l);
                } else if (tp == 3) {
                    render_diag(act, l); /* IJ経路診断 (実フォント/TVRAMの切り分け) */
                } else if (ntscv_mode == NTSCV_MODE_IJ_TEXT) {
                    render_ij(act, l);
                } else if (ntscv_mode == NTSCV_MODE_IJ_COLOR) {
                    render_ij_color(act, l, lpo);
                } else if (ntscv_mode == NTSCV_MODE_MONO_TEXT) {
                    render_mono80(act, l);
                } else if (ntscv_mode == NTSCV_MODE_COLOR_GRAPH) {
                    render_color_graph(act, l, lpo);
                } else {
                    render_color_text(act, l, lpo);
                }
            }
            continue;
        }

        /* 有効映像の直後のブランキング (後): ライン252-259は内容部を黒で
           消去 (ヘッドは永続)。残り (260-261) は再生 */
        if (line < NTSCV_LINES_PER_FIELD - 2) { /* ライン252-259 */
            fill_samples(lp + NTSCV_LINE_HEAD_SAMPLES,
                         NTSCV_SAMPLES_PER_LINE - NTSCV_LINE_HEAD_SAMPLES,
                         NTSCV_LEVEL_BLACK);
        }
    }
}

/*---- 仕様の整合性をコンパイル時に確認 ----*/

static_assert(NTSCV_FIELD_VSYNC_LINES + NTSCV_FIELD_BLANK1_LINES +
              NTSCV_FIELD_ACTIVE_LINES + NTSCV_FIELD_BLANK2_LINES ==
              NTSCV_LINES_PER_FIELD, "フィールドのライン数構成");

static_assert(NTSCV_IJ_TEXT_START + NTSCV_ACTIVE_SAMPLES <= NTSCV_SAMPLES_PER_LINE,
              "単色IJTEXTのライン構成");
static_assert(NTSCV_IJ_COLOR_START + NTSCV_IJ_COLOR_SAMPLES <= NTSCV_SAMPLES_PER_LINE,
              "32桁カラーのライン構成");
static_assert(NTSCV_COLOR_START + NTSCV_COLOR_ACTIVE_SAMPLES <= NTSCV_SAMPLES_PER_LINE,
              "42桁カラーのライン構成");
static_assert(NTSCV_MONO80_START + NTSCV_MONO80_ACTIVE_SAMPLES <= NTSCV_SAMPLES_PER_LINE,
              "80桁モノクロのライン構成");
static_assert(NTSCV_IJ_TEXT_START % 4 == 0 && NTSCV_IJ_COLOR_START % 4 == 0 &&
              NTSCV_COLOR_START % 4 == 0 && NTSCV_MONO80_START % 4 == 0,
              "有効画素の開始位置は4サンプル境界 (位相整合)");

static_assert(NTSCV_IJ_COLS * NTSCV_IJ_CHAR_SAMPLES == NTSCV_ACTIVE_SAMPLES, "IJ 1行のサンプル数");
static_assert(NTSCV_IJ_COLS * NTSCV_IJ_COLOR_CHAR_SAMPLES == NTSCV_IJ_COLOR_SAMPLES,
              "32桁カラー 1行のサンプル数");
static_assert(NTSCV_COLOR_COLS * 16 == NTSCV_COLOR_ACTIVE_SAMPLES, "カラー1行のサンプル数");
static_assert(NTSCV_MONO80_COLS * 8 == NTSCV_MONO80_ACTIVE_SAMPLES, "モノクロ1行のサンプル数");

static_assert(NTSCV_IJ_ROWS * NTSCV_IJ_LINES_PER_ROW == NTSCV_FIELD_ACTIVE_LINES, "IJ 縦構成");
static_assert(NTSCV_COLOR_ROWS * 8 == NTSCV_FIELD_ACTIVE_LINES, "カラー縦構成");
static_assert(NTSCV_MONO80_ROWS * 8 == NTSCV_FIELD_ACTIVE_LINES, "モノクロ縦構成");
static_assert(NTSCV_GRAPH_ROWS == NTSCV_FIELD_ACTIVE_LINES, "グラフィック縦構成");
static_assert(NTSCV_GRAPH_COLS == NTSCV_COLOR_COLS * 8, "グラフィック横構成");

static_assert(NTSCV_FRAME_MARK_LINE == NTSCV_ACTIVE_START + NTSCV_FIELD_ACTIVE_LINES, "フレーム境界ライン");

static_assert(sizeof(uint16_t) * NTSCV_SAMPLES_PER_LINE % 4 == 0, "バッファは32ビット境界に一致");
static_assert(NTSCV_LINES_PER_FIELD % NTSCV_CHUNK_LINES == 0, "チャンクとフィールドはライン境界で一致");

/* 全書き込みアンカー範囲 (4チャネルリング: 各領域の先頭8ライン) の閾値確認 */
static_assert(NTSCV_ACTIVE_START - 18 == 18, "ブランキング全書き込みはライン10-17");
static_assert(NTSCV_LINES_PER_FIELD - 2 == 260, "消去書き込みはライン252-259");
