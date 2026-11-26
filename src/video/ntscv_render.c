/*----------------------------------------------------------------------------

ntscv_render.c — 走査線レンダラ (docs/ntsc_video_spec.md §3, §5)

Copyright (c) 2026 the IchigoJam-RP2040Zero authors. MIT license.

映像割込み内で実行するため、本ファイルの関数はすべてRAM配置
(__not_in_flash_func → .time_critical) とし、XIPフラッシュの
実行待ちでライン期限 (127µs) が揺らがないようにする。

バッファ永続化契約 (1DMA転送=2走査線、フレーム525ライン=262.5チャンク):
  - 有効映像ラインは内容部 (172..843) のみ書き換える。同期+ポーチ部と
    末尾はアンカーの全書き込み (f1: ライン10-17、f2: 273-280) で書かれた
    内容がフィールド中ずっとバッファに残り、毎ライン再利用される
    (チャンクは8ライン刻みで再構築されるため、後退8ライン連鎖は8ライン幅の
    アンカー窓を必ず通る — f2有効映像はf2アンカー、f1有効映像はf1アンカーに到達)。
  - 垂直同期 (f1: 0-9、f2: 262-272) とアンカー、消去域 (f1: 252-260、
    f2: 514-522) は毎フレーム全書き込みし、261/523/524は書き込まない
    (消去済みラインの内容がそのまま再生される)。
  - ntscv_running=0 の間は一切書き込まない (ntscv_stop はエンジン自体を
    解放するため通常到達しない安全策。停止中は同期信号も出ない)。

VRAMは呼び出し側が所有・登録した配列を読むだけ (ライブラリは所有しない)。

----------------------------------------------------------------------------*/

#include "ntscv.h"
#include "hardware/sync.h"
#include "pico/time.h"
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
   バースト [2,1,2,3] は旧ドライバ (実機カラー実績あり) と同一。
   908サンプル/ライン (227周期) なので全ラインで同じ位相になる */
static void __not_in_flash_func(emit_head)(uint16_t *dst, bool with_burst)
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
    for (int c = 0; c < NTSCV_BURST_CYCLES; c++) {
        *p++ = NTSCV_BURST_PAIR01; /* 位相0-1 */
        *p++ = NTSCV_BURST_PAIR23; /* 位相2-3 */
    }
    fill_samples(dst + NTSCV_H_SYNC_SAMPLES + NTSCV_BREEZEWAY_SAMPLES
                     + NTSCV_BURST_CYCLES * 4,
                 NTSCV_LINE_HEAD_SAMPLES - NTSCV_H_SYNC_SAMPLES
                     - NTSCV_BREEZEWAY_SAMPLES - NTSCV_BURST_CYCLES * 4,
                 NTSCV_LEVEL_BLACK);
}

/* ブランキング1ライン分 (ヘッド+黒) を全書き込みする */
static void __not_in_flash_func(emit_blank_line)(uint16_t *lp, bool with_burst)
{
    emit_head(lp, with_burst);
    fill_samples(lp + NTSCV_LINE_HEAD_SAMPLES,
                 NTSCV_SAMPLES_PER_LINE - NTSCV_LINE_HEAD_SAMPLES,
                 NTSCV_LEVEL_BLACK);
}

/* 均等化パルス行: 行頭と半ライン中央に規格幅 (H同期の半分) の同期パルス
   (NTSC標準形式。パルス列が半ライン間隔で並び、TVの同期分離器が
   フィールドパリティを判定できる。幅がH同期と同じだとTVがEQをH同期として
   数えてラインカウンタがずれ、垂直がフリーランするため半幅が必須) */
static void __not_in_flash_func(emit_eq_line)(uint16_t *lp)
{
    uint16_t half = NTSCV_SAMPLES_PER_LINE / 2; /* 半ライン 454サンプル */
    uint16_t eq = NTSCV_EQ_PULSE_SAMPLES;       /* EQ幅 34サンプル ≈ 2.38us */
    fill_samples(lp, eq, NTSCV_LEVEL_SYNC);
    fill_samples(lp + eq, half - eq, NTSCV_LEVEL_BLACK);
    fill_samples(lp + half, eq, NTSCV_LEVEL_SYNC);
    fill_samples(lp + half + eq,
                 NTSCV_SAMPLES_PER_LINE - half - eq, NTSCV_LEVEL_BLACK);
}

/* セレーション行: ほぼ同期レベルで半ライン境界手前にH同期幅の黒の切れ目 */
static void __not_in_flash_func(emit_serr_line)(uint16_t *lp)
{
    uint16_t half = NTSCV_SAMPLES_PER_LINE / 2;
    uint16_t serr = half - NTSCV_H_SYNC_SAMPLES;
    fill_samples(lp, serr, NTSCV_LEVEL_SYNC);
    fill_samples(lp + serr, NTSCV_H_SYNC_SAMPLES, NTSCV_LEVEL_BLACK);
    fill_samples(lp + half, serr, NTSCV_LEVEL_SYNC);
    fill_samples(lp + half + serr,
                 NTSCV_SAMPLES_PER_LINE - half - serr, NTSCV_LEVEL_BLACK);
}

#if NTSCV_LONG_SYNC_VSYNC
/* ロング同期行: 元ドライバの垂直同期 (同期[0,840) + 黒[840,908))。
   S_Aプリセットでのみ使用。半ライン間隔のパルス列を持たない。 */
static void __not_in_flash_func(emit_long_sync_line)(uint16_t *lp)
{
    fill_samples(lp, NTSCV_LONG_SYNC_SAMPLES, NTSCV_LEVEL_SYNC);
    fill_samples(lp + NTSCV_LONG_SYNC_SAMPLES,
                 NTSCV_SAMPLES_PER_LINE - NTSCV_LONG_SYNC_SAMPLES,
                 NTSCV_LEVEL_BLACK);
}
#endif

/* フィールド2の垂直同期は半ライン遅れ (真インターレース)。そのため
   パルス列の位相が半ラインずつずれ、以下の4種の「混成ライン」が必要になる:
   - EQ_LATE  (ライン262): 前半ブランキング + 後半に最初の均等化パルス (262.5)
   - EQ_SERR  (ライン265): 前半に最後の均等化パルス (265.0) + 後半セレーション
   - SERR_EQ  (ライン268): 前半セレーション + 後半に最初の均等化パルス (268.5)
   - EQ_HALF  (ライン271): 前半に最後の均等化パルス (271.0) + 後半ブランキング */

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
  毎サンプルの位相演算はこの予算を圧迫するため、出力値を起動時/パレット変更時に
  事前計算した表引きのみにする。
  幾何は単色IJTEXTと同一 (1文字21サンプル、ドット幅2,3,2,3,3,2,3,3、
  文字位置 i の開始位相 = (21i)%4 = i%4)。
  文字ドット = 属性の前景色、背景ドット = 属性の背景色 (生パレット値)。*/

/* [前景/背景][色][開始位相][サンプル0..20] (+1パディングで先頭4バイト整列)。
   [0]=文字用 (生レベル)、[1]=背景用 (生レベル)。 */
static uint16_t ij_color_run[2][16][4][22] __attribute__((aligned(4)));

/* 1色分の描画表を更新する (割込み禁止区間を短く保つため色ごとに呼ぶ) */
static void __not_in_flash_func(build_ij_color_run)(int c)
{
    uint16_t s[4];
    s[0] = (uint16_t)ntscv_palette[c][0];
    s[1] = (uint16_t)(ntscv_palette[c][0] >> 16);
    s[2] = (uint16_t)ntscv_palette[c][1];
    s[3] = (uint16_t)(ntscv_palette[c][1] >> 16);
    for (int po = 0; po < 4; po++) {
        for (int k = 0; k < NTSCV_IJ_COLOR_CHAR_SAMPLES; k++) {
            ij_color_run[0][c][po][k] = s[(po + k) & 3]; /* 文字 (生レベル) */
            ij_color_run[1][c][po][k] = s[(po + k) & 3]; /* 背景 (生レベル) */
        }
    }
}

/* パレット変更 (PALコマンド/初期化) を描画表へ反映する */
void __not_in_flash_func(ntscv_render_palette_changed)(void)
{
    /* 色ごとに割込み禁止区間を分割する。全体を一度に禁止すると
       DMA合成IRQの遅延が再arm期限 (508µs) を超えて起動直後の画面が
       崩れるため (実機確認)。1色分は数µsで完了する */
    for (int c = 0; c < 16; c++) {
        uint32_t save = save_and_disable_interrupts();
        build_ij_color_run(c);
        restore_interrupts(save);
    }
}

static void __not_in_flash_func(render_ij_color)(uint16_t *w, int l)
{
    int row = ij_row_of_line[l];
    int tl = l - row * NTSCV_IJ_LINES_PER_ROW;
    int fontline = (tl < 8) ? tl : 7; /* 9走査線 → 8ラインフォント (zoom=1固定) */
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_IJ_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_IJ_COLS;
    for (int i = 0; i < NTSCV_IJ_COLS; i++) {
        uint8_t a = attr[i];
        int pi = i & 3; /* 文字位置の開始位相 (21≡1 mod 4) */
        /* 文字ドット = 前景色 (暗位相持ち上げ)、背景ドット = 背景色 (生) */
        const uint16_t *fg = ij_color_run[0][a & 15][pi];
        const uint16_t *bg = ij_color_run[1][(a >> 4) & 15][pi];
        uint8_t bits = ntscv_font[code[i] * 8 + fontline];
        /* カーソルセル: 単色モードと同じXORマスクで反転する
           (文字ドットと背景ドットの役割が入れ替わり、反転ブロックになる) */
        if ((int)(row * NTSCV_IJ_COLS + i) == ntscv_ij_cursor_cell)
            bits ^= ntscv_ij_cursor_xor;
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

/*---- カラーテキスト: 42×27、1ドット2サンプル (旧ドライバWIDETEXTと同一構造) ----
  1文字16サンプル = 搬送波4周期。偶数ドット=パレット位相0-1、奇数ドット=位相2-3。
  文字ドット = 属性の前景色、背景ドット = 属性の背景色 (生パレット値)。
  カーソル/反転は vram_to_tvram_all で属性の前景/背景を入替えて shadow に書くため、
  描画側では cursor 処理不要 (VIDEO 9 と同じ方式)。 */
static void __not_in_flash_func(render_color_text)(uint16_t *w, int l)
{
    int row = l >> 3;
    int fline = l & 7;
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_COLOR_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_COLOR_COLS;
    for (int i = 0; i < NTSCV_COLOR_COLS; i++) {
        uint8_t bits = ntscv_font[code[i] * 8 + fline];
        const uint32_t *fg = ntscv_palette[attr[i] & 15];
        const uint32_t *bg = ntscv_palette[(attr[i] >> 4) & 15];
        for (int j = 0; j < 4; j++) {
            *(uint32_t *)w = (bits & 0x80) ? fg[0] : bg[0];
            w += 2;
            *(uint32_t *)w = (bits & 0x40) ? fg[1] : bg[1];
            w += 2;
            bits <<= 2;
        }
    }
}

/*---- カラーグラフィック+テキスト: 336×216に42×27文字を重ねる (仕様書§3.3) ----
  GVRAMは1画素1バイトのパレット番号。フォントビットが立っている画素は文字色優先。
*/
static void __not_in_flash_func(render_color_graph)(uint16_t *w, int l)
{
    if (!ntscv_vram_graph) {
        render_color_text(w, l); /* グラフィックVRAM未登録ならテキストのみ */
        return;
    }
    int row = l >> 3;
    int fline = l & 7;
    const uint8_t *code = ntscv_vram_codes + row * NTSCV_COLOR_COLS;
    const uint8_t *attr = ntscv_vram_attrs + row * NTSCV_COLOR_COLS;
    const uint8_t *gline = ntscv_vram_graph + l * NTSCV_GRAPH_COLS;
    uint32_t *w32 = (uint32_t *)w;
    for (int i = 0; i < NTSCV_COLOR_COLS; i++) {
        uint8_t bits = ntscv_font[code[i] * 8 + fline];
        const uint32_t *fg = ntscv_palette[attr[i] & 15];
        const uint8_t *gp = gline + i * 8;
        for (int j = 0; j < 4; j++) {
            *w32++ = (bits & 0x80) ? fg[0] : ntscv_palette[gp[j * 2]][0];
            *w32++ = (bits & 0x40) ? fg[1] : ntscv_palette[gp[j * 2 + 1]][1];
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
static void __not_in_flash_func(render_bars)(uint16_t *w, int l)
{
    uint32_t *w32 = (uint32_t *)w;
    if (l < NTSCV_FIELD_ACTIVE_LINES - 27) {
        for (int bar = 0; bar < 7; bar++) {
            for (int c = 0; c < 24; c++) {
                *w32++ = bar_pair[bar][0];
                *w32++ = bar_pair[bar][1];
            }
        }
#ifdef NTSCV_MERGE_DEBUG
        /* DMA合成割込み合流回数を左上に16進4桁で重畳 (診断用) */
        if (l < 8 && ntscv_font) {
            uint16_t ev = ntscv_debug_events();
            for (int d = 0; d < 4; d++) {
                int shift = 12 - d * 4;
                int nibble = (ev >> shift) & 0xF;
                uint8_t ccode = (uint8_t)(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
                uint8_t bits = ntscv_font[ccode * 8 + l];
                uint16_t *wp = w + d * 8;
                for (int x = 0; x < 8; x++) {
                    wp[x] = (bits & (0x80 >> x)) ? NTSCV_LEVEL_WHITE : NTSCV_LEVEL_BLACK;
                }
            }
        }
#endif
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

/*---- 走査線組み立て (1ラインping-pong用) ----*/

/*---- 走査線組み立て (4チャネルリング用: 2走査線チャンク) ----*/

/* カラーバーストを載せるか: カラーモード、またはカラーバー表示中 */
static bool __not_in_flash_func(with_burst_now)(void)
{
    return (ntscv_mode == NTSCV_MODE_COLOR_TEXT) ||
           (ntscv_mode == NTSCV_MODE_COLOR_GRAPH) ||
           (ntscv_mode == NTSCV_MODE_IJ_COLOR) ||
           (ntscv_test_mode == 1);
}

/* 安全チャンク: DMAがバッファ終端からはみ出して再生した場合に
   流れる「同期+黒」の有効な2走査線分 (core.cが起動時に1度だけ作る)。
   白いゴミ (ガード値・隣接RAM) が出るのを防ぐ。 */
void ntscv_compose_safety_chunk(uint16_t *dst)
{
    for (int i = 0; i < NTSCV_CHUNK_LINES; i++) {
        emit_blank_line(dst + (size_t)i * NTSCV_SAMPLES_PER_LINE, false);
    }
}

/* フィールド境界フック: フレームマークライン (252/514) の合成時に1回呼ばれる
   (DMA割込み内)。マークライン直後はVSYNC+ブランキングの非表示窓
   (約46ライン≒2.9ms) が続くため、TVRAMコピー等の表示更新をこの窓で行うと
   ティアリングが完全に無くなる。
   (60Hzタイマーから非同期コピーすると、うなりでティア線が画面を一周し、
   画面ジャンプ/ロールに見えていた) */
static void (*field_hook_cb)(void) = 0;

void ntscv_set_field_hook(void (*cb)(void))
{
    field_hook_cb = cb;
}

/* 有効映像1ライン分の内容部 (サンプル172..843) を現在のモードで描画する。
   l: フィールド内の有効映像ライン (0..215) */
static void __not_in_flash_func(render_line_active)(uint16_t *lp, int l)
{
    uint16_t *act = lp + NTSCV_LINE_HEAD_SAMPLES;
    if (!ntscv_font || !ntscv_vram_codes || !ntscv_vram_attrs) {
        fill_samples(act, NTSCV_ACTIVE_SAMPLES, NTSCV_LEVEL_BLACK);
        return;
    }
    int tp = ntscv_test_mode;
    if (tp == 1) {
        render_bars(act, l);
    } else if (tp == 2) {
        render_grid(act, l);
    } else if (tp == 3) {
        render_diag(act, l); /* IJ経路診断 (実フォント/TVRAMの切り分け) */
    } else if (ntscv_mode == NTSCV_MODE_IJ_TEXT) {
        render_ij(act, l);
    } else if (ntscv_mode == NTSCV_MODE_IJ_COLOR) {
        render_ij_color(act, l);
    } else if (ntscv_mode == NTSCV_MODE_MONO_TEXT) {
        render_mono80(act, l);
    } else if (ntscv_mode == NTSCV_MODE_COLOR_GRAPH) {
        render_color_graph(act, l);
    } else {
        render_color_text(act, l);
    }
}

/* noinline+no-jump-tables必須: 本関数はSAVE等のフラッシュ書込み中 (XIP停止) に
   DMA IRQから実行される。switch/ifチェーンをGCCがジャンプテーブル (FLASH上の
   rodata) に変換すると、XIP停止中のテーブル読出しがゴミ値を返して mov pc が
   未知アドレスへ飛びHardFaultするため、テーブル生成を禁止する */
void __attribute__((noinline, optimize("O2", "no-jump-tables"))) __not_in_flash_func(ntscv_compose_chunk)(uint16_t *dst, uint16_t first_line)
{
    /* 停止中は書き込まない (ntscv_stopはエンジン自体を解放するため通常は不到達の安全策) */
    if (!ntscv_running) return;

    /* カラーバーストを載せるか: カラーモード、またはカラーバー表示中 */
    bool with_burst = with_burst_now();

    for (int i = 0; i < NTSCV_CHUNK_LINES; i++) {
        /* フレームは525ライン (奇数) のため、境界を跨ぐチャンク (524, 0) は
           ここでライン番号を折り返して処理する (mod算術が全ラインを自動追跡) */
        uint16_t line = first_line + (uint16_t)i;
        if (line >= NTSCV_LINES_PER_FRAME) {
            line = (uint16_t)(line - NTSCV_LINES_PER_FRAME);
        }
        uint16_t *lp = dst + (size_t)i * NTSCV_SAMPLES_PER_LINE;

        if (line == NTSCV_F1_MARK || line == NTSCV_F2_MARK) {
            /* 有効映像の直後のラインでフィールドシーケンスを進める (旧drawcount互換)。
               フレームは奇数ラインのためチャンク先頭の偶奇がフレームごとに反転し、
               マーク検出は行単位で行う (先頭一致だと半分のフィールドで発火しない) */
            ntscv_frame_seq++;
            if (field_hook_cb) field_hook_cb();
        }

        if (line <= NTSCV_F1_VSYNC_END) {
            /* フィールド1垂直同期 (ライン0-9)。
               S_Aプリセットでは元ドライバのロング同期 (10ライン連続) を再現。
               それ以外では規格のEQ/SERR (均等化パルス+セレーション+EQ) + ブランキング。 */
#if NTSCV_LONG_SYNC_VSYNC
            emit_long_sync_line(lp);
#else
            if (line <= 2 || (line >= 6 && line <= 8)) {
                emit_eq_line(lp);
            } else if (line <= 5) {
                emit_serr_line(lp);
            } else { /* line == 9: ブランキングヘッド (バースト無し) + 黒 */
                emit_blank_line(lp, false);
            }
#endif
            continue;
        }

        if (line <= NTSCV_F1_BLANK1_END) {
            /* f1ブランキング (前): 先頭8ライン (10-17) だけ全書き込みし
               各チャネルのヘッドを確立。残りは再生 */
            if (line <= NTSCV_F1_ANCHOR_END) {
                emit_blank_line(lp, with_burst);
            }
            continue;
        }

        if (line <= NTSCV_F1_ACTIVE_END) {
            /* f1有効映像: 内容部 (サンプル172..843) のみ書き換え。
               ヘッド/末尾はライン10-17の全書き込みが永続する */
            render_line_active(lp, (int)line - NTSCV_F1_ACTIVE_START);
            continue;
        }

        if (line <= NTSCV_F1_ERASE_END) { /* ライン252-260 */
            /* f1のブランキング (後): 内容部を黒で消去 (ヘッドは永続) */
            fill_samples(lp + NTSCV_LINE_HEAD_SAMPLES,
                         NTSCV_ACTIVE_SAMPLES, NTSCV_LEVEL_BLACK);
            continue;
        }
        if (line < NTSCV_F2_VSYNC_START) continue; /* 261: 再生 */

        if (line <= NTSCV_F2_VSYNC_END) {
            /* フィールド2垂直同期 (ライン262-272): f1と同一のパルス列が
               半ライン遅れで並ぶ (真インターレース)。デインターレーサは
               この半ラインオフセットでフィールドパリティを確定できる。
               ライン262/265/268/271は前半と後半でパターンが異なる混成ライン */
            uint16_t half = NTSCV_SAMPLES_PER_LINE / 2;
            uint16_t eq = NTSCV_EQ_PULSE_SAMPLES;
            uint16_t serr = half - NTSCV_H_SYNC_SAMPLES;
            /* if/elseチェーンで記述する (switchのジャンプテーブルはFLASH上の
               rodataに置かれ、フラッシュ書込み中のXIP停止でテーブル読出しが
               ゴミ値を返して mov pc が未知アドレスへ飛ぶため) */
            if (line == 262) { /* 前半ブランキング + 均等化パルス (262.5) */
                fill_samples(lp, half, NTSCV_LEVEL_BLACK);
                fill_samples(lp + half, eq, NTSCV_LEVEL_SYNC);
                fill_samples(lp + half + eq,
                             NTSCV_SAMPLES_PER_LINE - half - eq,
                             NTSCV_LEVEL_BLACK);
            } else if (line == 263 || line == 264 || line == 269 || line == 270) {
                /* 均等化パルス (263.0-264.5 / 269.0-270.5) */
                emit_eq_line(lp);
            } else if (line == 265) { /* 均等化パルス (265.0) + セレーション後半 (265.5-266.5) */
                fill_samples(lp, eq, NTSCV_LEVEL_SYNC);
                fill_samples(lp + eq, half - eq, NTSCV_LEVEL_BLACK);
                fill_samples(lp + half, serr, NTSCV_LEVEL_SYNC);
                fill_samples(lp + half + serr,
                             NTSCV_SAMPLES_PER_LINE - half - serr, NTSCV_LEVEL_BLACK);
            } else if (line == 266 || line == 267) { /* セレーション (266.0-268.0) */
                emit_serr_line(lp);
            } else if (line == 268) { /* セレーション前半 (268.0-268.5) + 均等化パルス (268.5) */
                fill_samples(lp, serr, NTSCV_LEVEL_SYNC);
                fill_samples(lp + serr, NTSCV_H_SYNC_SAMPLES, NTSCV_LEVEL_BLACK);
                fill_samples(lp + half, eq, NTSCV_LEVEL_SYNC);
                fill_samples(lp + half + eq,
                             NTSCV_SAMPLES_PER_LINE - half - eq,
                             NTSCV_LEVEL_BLACK);
            } else if (line == 271) { /* 均等化パルス (271.0) + 後半ブランキング */
                fill_samples(lp, eq, NTSCV_LEVEL_SYNC);
                fill_samples(lp + eq,
                             NTSCV_SAMPLES_PER_LINE - eq, NTSCV_LEVEL_BLACK);
            } else { /* 272: ブランキングヘッド (バースト無し) + 黒 */
                emit_blank_line(lp, false);
            }
            continue;
        }

        if (line <= NTSCV_F2_BLANK1_END) {
            /* f2ブランキング (前): 先頭8ライン (273-280) だけ全書き込み。
               残りは再生 */
            if (line <= NTSCV_F2_ANCHOR_END) {
                emit_blank_line(lp, with_burst);
            }
            continue;
        }

        if (line <= NTSCV_F2_ACTIVE_END) {
            /* f2有効映像: 内容部のみ書き換え (ヘッド/末尾は273-280が永続) */
            render_line_active(lp, (int)line - NTSCV_F2_ACTIVE_START);
            continue;
        }

        if (line <= NTSCV_F2_ERASE_END) { /* ライン514-522 */
            /* f2のブランキング (後): 内容部を黒で消去 (ヘッドは永続) */
            fill_samples(lp + NTSCV_LINE_HEAD_SAMPLES,
                         NTSCV_ACTIVE_SAMPLES, NTSCV_LEVEL_BLACK);
            continue;
        }
        /* 523/524: 再生 (書き込まない。消去済み内容がそのまま出る) */
    }
}

/*---- 仕様の整合性をコンパイル時に確認 ----*/

static_assert(NTSCV_F1_ACTIVE_END - NTSCV_F1_ACTIVE_START + 1 == NTSCV_FIELD_ACTIVE_LINES,
              "f1有効映像は216ライン");
static_assert(NTSCV_F1_MARK == NTSCV_F1_ACTIVE_START + NTSCV_FIELD_ACTIVE_LINES,
              "f1マークは有効映像の直後");

#if NTSCV_FIELDS_PER_FRAME == 2
/* フィールド2はフィールド1と同一の同期パルス列が半ライン遅れで
   現れる (真インターレース)。同期ブロックは262-272の11ライン */
static_assert(NTSCV_F2_VSYNC_START == NTSCV_F1_MARK + 10 &&
              NTSCV_F2_VSYNC_END == NTSCV_F2_VSYNC_START + 10,
              "f2同期ブロックはライン262-272");
static_assert(NTSCV_F2_ACTIVE_START == NTSCV_F2_VSYNC_END + NTSCV_FIELD_BLANK1_LINES,
              "f2ブランキング(前)はライン273-297の26ライン");
static_assert(NTSCV_LINES_PER_FRAME == NTSCV_F2_ERASE_END + 3,
              "フレームは525ライン (消去514-522 + 再生523/524)");
static_assert(NTSCV_F2_ACTIVE_END - NTSCV_F2_ACTIVE_START + 1 == NTSCV_FIELD_ACTIVE_LINES,
              "f2有効映像は216ライン");
static_assert(NTSCV_F2_MARK == NTSCV_F2_ACTIVE_START + NTSCV_FIELD_ACTIVE_LINES,
              "f2マークは有効映像の直後");
static_assert(NTSCV_F2_ANCHOR_END == NTSCV_F2_VSYNC_END + 8, "f2アンカーはライン273-280の8ライン");
static_assert(NTSCV_F2_ERASE_END == NTSCV_F2_MARK + 8, "f2消去書き込みはライン514-522");
#else
static_assert(NTSCV_LINES_PER_FRAME == NTSCV_F1_ERASE_END + 2,
              "プログレッシブ262ライン (消去252-260 + 再生261)");
#endif

static_assert(NTSCV_LINE_HEAD_SAMPLES + NTSCV_ACTIVE_SAMPLES <= NTSCV_SAMPLES_PER_LINE,
              "ラインのサンプル数構成");
static_assert(NTSCV_EQ_PULSE_SAMPLES <= NTSCV_H_SYNC_SAMPLES / 2,
              "均等化パルスはH同期の半幅以下 (同期分離器の幅判別でH同期と区別されるため)");
static_assert(NTSCV_IJ_TEXT_START % 4 == 0, "有効画素の開始位置は4サンプル境界 (位相整合)");

static_assert(NTSCV_IJ_COLS * NTSCV_IJ_CHAR_SAMPLES == NTSCV_ACTIVE_SAMPLES, "IJ 1行のサンプル数");
static_assert(NTSCV_COLOR_COLS * 16 == NTSCV_ACTIVE_SAMPLES, "カラー1行のサンプル数");
static_assert(NTSCV_MONO80_COLS * 8 == NTSCV_MONO80_ACTIVE_SAMPLES, "モノクロ1行のサンプル数");

static_assert(NTSCV_IJ_ROWS * NTSCV_IJ_LINES_PER_ROW == NTSCV_FIELD_ACTIVE_LINES, "IJ 縦構成");
static_assert(NTSCV_COLOR_ROWS * 8 == NTSCV_FIELD_ACTIVE_LINES, "カラー縦構成");
static_assert(NTSCV_MONO80_ROWS * 8 == NTSCV_FIELD_ACTIVE_LINES, "モノクロ縦構成");
static_assert(NTSCV_GRAPH_ROWS == NTSCV_FIELD_ACTIVE_LINES, "グラフィック縦構成");
static_assert(NTSCV_GRAPH_COLS == NTSCV_COLOR_COLS * 8, "グラフィック横構成");

static_assert(sizeof(uint16_t) * NTSCV_SAMPLES_PER_LINE % 4 == 0, "バッファは32ビット境界に一致");

/* 全書き込みアンカー/消去範囲 (4チャネルリング: 各領域の先頭8ライン) の確認。
    アンカーは8ライン幅 — チャンク再構築の後退8ライン連鎖が窓を跳び越えられない
    ことが永続化契約の前提 (f1有効映像→f1アンカー) */
static_assert(NTSCV_F1_ANCHOR_END == NTSCV_F1_VSYNC_END + 8, "f1アンカーはライン10-17の8ライン");
static_assert(NTSCV_F1_ERASE_END == NTSCV_F1_MARK + 8, "f1消去書き込みはライン252-260");
