//NEO.* / NEOSCROLL.* — WS2812ストリップ・マトリクス制御 (Annex WiFi互換コマンド群)
//NEO.*はWS.PUT/WS.MATRIXと同じ8KB専用バッファ (ws_matrix_buf、GRB 3バイト/px) を共用する
//(WS.SETUPの信号反転・SDON中の共有ピン保護はws_out経由で自動的に有効)
//色は16bit RGB555パック (R<<10|G<<5|B、0-32767。BASICの式が16bitのためAnnexの24bit色は不可)
//明るさ・ブレンドのgain/progressは1/256固定小数 (256=1.0倍)
#ifndef __EXT_NEO_H__
#define __EXT_NEO_H__
#include "all_includes.h"

//===== NEO.* (ストリップ) =====

static int neo_port = 7; // NEO.SETUP port (WS.LEDポート番号0-11、既定7=GP15)
static int neo_leds = 8; // NEO.SETUP nb_led (1-2730)

static int neo_clamp8(int v) {
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
    return v;
}

//RGB555パック色 → 8bit展開 (5bit値 v→(v<<3)|(v>>2): 31→255)
static void neo_unpack555(int c, int* r, int* g, int* b) {
    c &= 0x7fff;
    int r5 = (c >> 10) & 31, g5 = (c >> 5) & 31, b5 = c & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g5 << 3) | (g5 >> 2);
    *b = (b5 << 3) | (b5 >> 2);
}

//8bit → RGB555パック色 (各成分0-255にクランプ、上位5bit採用)
static int16 neo_pack555(int r, int g, int b) {
    return (int16)(((neo_clamp8(r) >> 3) << 10) | ((neo_clamp8(g) >> 3) << 5) | (neo_clamp8(b) >> 3));
}

//専用バッファのposへGRB888書込み (色成分は0-255にクランプ)
static void neo_put_px(int pos, int r, int g, int b) {
    uint8_t* p = ws_matrix_buf + (size_t)pos * 3;
    p[0] = (uint8_t)neo_clamp8(g); // GRB順で格納 (WS.LED mode3と同一形式)
    p[1] = (uint8_t)neo_clamp8(r);
    p[2] = (uint8_t)neo_clamp8(b);
}

static void neo_copy_px(int dst, int src) {
    uint8_t* d = ws_matrix_buf + (size_t)dst * 3;
    const uint8_t* s = ws_matrix_buf + (size_t)src * 3;
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
}

static void neo_zero_px(int pos) {
    uint8_t* p = ws_matrix_buf + (size_t)pos * 3;
    p[0] = p[1] = p[2] = 0;
}

//NEO.REFRESH — 専用バッファをNEO.SETUPのポート/LED数で送信
static void neo_refresh() {
    ws_data_mode = WS_MODE_BUFFER;
    ws_data_offset = 0;
    ws_out(neo_port, neo_leds, 1);
}

//関数共通: 括弧内の式をカンマ区切りでcount個読み、')'を確認する (func_rgbと同じパターン)
//戻り値: 1=成功、0=エラー設定済み
static int neo_func_args(int* out, int count) {
    for (int i = 0;; i++) {
        out[i] = token_expression();
        IJB_ERR_CHK1(0);
        if (i == count - 1)
            break;
        Token t; Token_get(t);
        if (t.code != TOKEN_COMMA) {
            command_error(ERR_SYNTAX_ERROR);
            return 0;
        }
    }
    Token t; Token_get(t);
    if (t.code != TOKEN_PAREN_E) {
        command_error(ERR_SYNTAX_ERROR);
        return 0;
    }
    return 1;
}

//NEO.SETUP port{,nb_led} — ポート (WS.LEDと同じ0-11) とLED数 (1-2730)
S_INLINE void command_neo_setup() {
    int port = token_expression();
    IJB_ERR_CHK();
    int nb = token_option1(neo_leds); // 省略時は現状維持
    IJB_ERR_CHK();
    if (port < 0 || port > 11 || nb < 1 || nb > WS_BUFFER_PIXELS) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    neo_port = port;
    neo_leds = nb;
    ws_data_mode = WS_MODE_BUFFER; // 専用バッファ (mode3)・オフセット0へ
    ws_data_offset = 0;
}

//NEO.PIXEL pos,R,G,B{,disable} / NEO.PIXEL pos,COLOR{,disable}
//(pos以降の引数の個数で判別: 1-2個=COLOR{,disable}、3-4個=R,G,B{,disable}。disable=1は書込みのみ)
S_INLINE void command_neo_pixel() {
    int pos = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int a[4];
    int cnt = 0;
    for (;;) {
        a[cnt++] = token_expression();
        IJB_ERR_CHK();
        if (cnt == 4) {
            break;
        }
        if (token_getCode() != TOKEN_COMMA) {
            token_back();
            break;
        }
    }
    token_end();
    IJB_ERR_CHK();
    int r, g, b, disable;
    if (cnt <= 2) { // pos,COLOR{,disable}
        neo_unpack555(a[0], &r, &g, &b);
        disable = (cnt == 2) ? a[1] : 0;
    } else { // pos,R,G,B{,disable}
        r = a[0]; g = a[1]; b = a[2];
        disable = (cnt == 4) ? a[3] : 0;
    }
    if (pos < 0 || pos >= neo_leds) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    neo_put_px(pos, r, g, b);
    if (!disable) {
        neo_refresh();
    }
}

//NEO.STRIP s,e,R,G,B{,disable} / NEO.STRIP s,e,COLOR{,disable} — s〜e (両端含む) へ一括設定
//(s,e以降の引数の個数で判別: 1-2個=COLOR{,disable}、3-4個=R,G,B{,disable})
S_INLINE void command_neo_strip() {
    int s = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int e = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int a[4];
    int cnt = 0;
    for (;;) {
        a[cnt++] = token_expression();
        IJB_ERR_CHK();
        if (cnt == 4) {
            break;
        }
        if (token_getCode() != TOKEN_COMMA) {
            token_back();
            break;
        }
    }
    token_end();
    IJB_ERR_CHK();
    int r, g, b, disable;
    if (cnt <= 2) { // s,e,COLOR{,disable}
        neo_unpack555(a[0], &r, &g, &b);
        disable = (cnt == 2) ? a[1] : 0;
    } else { // s,e,R,G,B{,disable}
        r = a[0]; g = a[1]; b = a[2];
        disable = (cnt == 4) ? a[3] : 0;
    }
    if (s < 0 || e < s || e >= neo_leds) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    for (int i = s; i <= e; i++) {
        neo_put_px(i, r, g, b);
    }
    if (!disable) {
        neo_refresh();
    }
}

//NEO.ROTATELEFT/ROTATERIGHT n{,s,e,disable} — 範囲[s,e]内で回転 (端は反対側へ周回)
//ROTATELEFTは画素が先頭方向へn個移動 (1ステップ左回転をn回、追加メモリなし)
S_INLINE void command_neo_rotate(int right) {
    int n = token_expression();
    IJB_ERR_CHK();
    int s = 0, e = neo_leds - 1, disable = 0;
    if (token_getCode() == TOKEN_COMMA) {
        s = token_expression();
        IJB_ERR_CHK();
        if (token_getCode() != TOKEN_COMMA) {
            command_error(ERR_SYNTAX_ERROR);
            return;
        }
        e = token_expression();
        IJB_ERR_CHK();
        int code = token_getCode();
        if (code != TOKEN_COMMA) {
            token_back();
        } else {
            disable = token_expression();
            IJB_ERR_CHK();
        }
    } else {
        token_back();
    }
    token_end();
    IJB_ERR_CHK();
    if (n < 0 || s < 0 || e < s || e >= neo_leds) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    int len = e - s + 1;
    n %= len;
    if (right) {
        n = len - n;
    }
    for (int i = 0; i < n; i++) { // 左へ1ステップ×n
        uint8_t tmp[3];
        uint8_t* p = ws_matrix_buf + (size_t)s * 3;
        tmp[0] = p[0]; tmp[1] = p[1]; tmp[2] = p[2];
        for (int k = 0; k < len - 1; k++) {
            p[k * 3] = p[(k + 1) * 3];
            p[k * 3 + 1] = p[(k + 1) * 3 + 1];
            p[k * 3 + 2] = p[(k + 1) * 3 + 2];
        }
        uint8_t* q = ws_matrix_buf + (size_t)e * 3;
        q[0] = tmp[0]; q[1] = tmp[1]; q[2] = tmp[2];
    }
    if (!disable) {
        neo_refresh();
    }
}

//NEO.SHIFTLEFT/SHIFTRIGHT n{,s,e,disable} — 範囲[s,e]内でシフト (空いた画素は黒)
S_INLINE void command_neo_shift(int right) {
    int n = token_expression();
    IJB_ERR_CHK();
    int s = 0, e = neo_leds - 1, disable = 0;
    if (token_getCode() == TOKEN_COMMA) {
        s = token_expression();
        IJB_ERR_CHK();
        if (token_getCode() != TOKEN_COMMA) {
            command_error(ERR_SYNTAX_ERROR);
            return;
        }
        e = token_expression();
        IJB_ERR_CHK();
        int code = token_getCode();
        if (code != TOKEN_COMMA) {
            token_back();
        } else {
            disable = token_expression();
            IJB_ERR_CHK();
        }
    } else {
        token_back();
    }
    token_end();
    IJB_ERR_CHK();
    if (n < 0 || s < 0 || e < s || e >= neo_leds) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    int len = e - s + 1;
    if (n == 0) {
        // 変化なし
    } else if (n >= len) { // 全部黒
        for (int i = s; i <= e; i++) {
            neo_zero_px(i);
        }
    } else if (!right) { // LEFT: 前方向へコピー、末尾n個を黒
        for (int i = 0; i < len - n; i++) {
            neo_copy_px(s + i, s + i + n);
        }
        for (int i = len - n; i < len; i++) {
            neo_zero_px(s + i);
        }
    } else { // RIGHT: 後方向へコピー、先頭n個を黒
        for (int i = len - 1; i >= n; i--) {
            neo_copy_px(s + i, s + i - n);
        }
        for (int i = 0; i < n; i++) {
            neo_zero_px(s + i);
        }
    }
    if (!disable) {
        neo_refresh();
    }
}

//NEO.REFRESH — 専用バッファ全体を送信
S_INLINE void command_neo_refresh() {
    token_end();
    IJB_ERR_CHK();
    neo_refresh();
}

//NEO.RGB(r,g,b) → RGB555パック色 (0-32767)
S_INLINE int func_neo_rgb() {
    int n[3];
    if (!neo_func_args(n, 3)) {
        return 0;
    }
    return neo_pack555(n[0], n[1], n[2]);
}

//NEO.GETPIXEL(pos) → パック色 (バッファからGRB888で読み出し)
S_INLINE int func_neo_getpixel() {
    int n[1];
    if (!neo_func_args(n, 1)) {
        return 0;
    }
    if (n[0] < 0 || n[0] >= neo_leds) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return 0;
    }
    const uint8_t* p = ws_matrix_buf + (size_t)n[0] * 3;
    return neo_pack555(p[1], p[0], p[2]); // GRB → RGB
}

//c1→c2の線形補間 (p 0-256、256=c2)
static int neo_lerp8(int c1, int c2, int p) {
    if (p < 0) p = 0;
    else if (p > 256) p = 256;
    return c1 + (((c2 - c1) * p) >> 8);
}

//NEO.DIM(COLOR,gain) — 明るさ調整。gain 0-512 (1/256倍率、256=等倍、512=2倍)。255超はクランプ
S_INLINE int func_neo_dim() {
    int n[2];
    if (!neo_func_args(n, 2)) {
        return 0;
    }
    int r, g, b;
    neo_unpack555(n[0], &r, &g, &b);
    int gain = n[1];
    if (gain < 0) gain = 0;
    else if (gain > 512) gain = 512;
    return neo_pack555((r * gain) >> 8, (g * gain) >> 8, (b * gain) >> 8);
}

//NEO.LIGHTEN(COLOR,gain) — 白へブレンド。gain 0-256 (0=原色、256=白)
S_INLINE int func_neo_lighten() {
    int n[2];
    if (!neo_func_args(n, 2)) {
        return 0;
    }
    int r, g, b;
    neo_unpack555(n[0], &r, &g, &b);
    int p = n[1];
    return neo_pack555(neo_lerp8(r, 255, p), neo_lerp8(g, 255, p), neo_lerp8(b, 255, p));
}

//NEO.DARKEN(COLOR,gain) — 黒へブレンド。gain 0-256 (0=原色、256=黒)
S_INLINE int func_neo_darken() {
    int n[2];
    if (!neo_func_args(n, 2)) {
        return 0;
    }
    int r, g, b;
    neo_unpack555(n[0], &r, &g, &b);
    int p = n[1];
    if (p < 0) p = 0;
    else if (p > 256) p = 256;
    return neo_pack555((r * (256 - p)) >> 8, (g * (256 - p)) >> 8, (b * (256 - p)) >> 8);
}

//NEO.LINEARBLEND(C1,C2,p) — 2色ブレンド。p 0-256 (0=C1、256=C2)
S_INLINE int func_neo_linearblend() {
    int n[3];
    if (!neo_func_args(n, 3)) {
        return 0;
    }
    int r1, g1, b1, r2, g2, b2;
    neo_unpack555(n[0], &r1, &g1, &b1);
    neo_unpack555(n[1], &r2, &g2, &b2);
    return neo_pack555(neo_lerp8(r1, r2, n[2]), neo_lerp8(g1, g2, n[2]), neo_lerp8(b1, b2, n[2]));
}

//NEO.BILINEARBLEND(UL,UR,LL,LR,x,y) — 4色ブレンド。x,y 0-256 (0,0=UL、256,256=LR)
S_INLINE int func_neo_bilinearblend() {
    int n[6];
    if (!neo_func_args(n, 6)) {
        return 0;
    }
    int r, g, b;
    neo_unpack555(n[0], &r, &g, &b);
    int rul = r, gul = g, bul = b;
    neo_unpack555(n[1], &r, &g, &b);
    int rur = r, gur = g, bur = b;
    neo_unpack555(n[2], &r, &g, &b);
    int rll = r, gll = g, bll = b;
    neo_unpack555(n[3], &r, &g, &b);
    int rlr = r, glr = g, blr = b;
    int rt = neo_lerp8(rul, rur, n[4]), gt = neo_lerp8(gul, gur, n[4]), bt = neo_lerp8(bul, bur, n[4]);
    int rb = neo_lerp8(rll, rlr, n[4]), gb = neo_lerp8(gll, glr, n[4]), bb = neo_lerp8(bll, blr, n[4]);
    return neo_pack555(neo_lerp8(rt, rb, n[5]), neo_lerp8(gt, gb, n[5]), neo_lerp8(bt, bb, n[5]));
}

//===== NEOSCROLL (WS2812Bドットマトリクス・キャンバス。モジュール寸法はSETUPで指定可) =====
//キャンバスに描画した内容をNEOSCROLL.SHOW / SCROLL / OSCILLATEでLEDへ転送する
//(SPRITESHEET/SPRITEはBMPファイルI/Oがないため非対応、フォントは内蔵5種のみ)

#define NS_CANVAS_MAX 8192 // キャンバス最大ピクセル数 (RGB555 2バイト/px、16KB)
static uint16_t ns_canvas[NS_CANVAS_MAX];
static int ns_used = 0;        // NEOSCROLL.SETUP済みか (DELETEで0)
static int ns_port = 7;        // 出力ポート (WS.LEDと同じ0-11)
static int ns_dw = 8, ns_dh = 8; // 表示寸法 (devices*mw × lines*mh)
static int ns_mw = 8, ns_mh = 8; // モジュール寸法 (NEOSCROLL.SETUPのmw,mh、既定8x8)
static int ns_cw = 8, ns_ch = 8; // キャンバス寸法 (省略時=表示寸法)
static int ns_layout = 0;      // 配線レイアウト 0-5
static int ns_flip = 0;        // 1=表示を上下反転
static int ns_bright = 128;    // 全体明るさ 0-255 (既定128、SHOWで適用、255=等倍)
static int ns_tbright = 255;   // テキスト明るさ 0-255 (既定255)
static int ns_font = 0;        // oled_font_tablesのindex (0=5x7プロポーショナル)
static int ns_curx = 0, ns_cury = 0; // 描画カーソル (左上原点・y下向き)
static int ns_mode = 0;        // スクロール方向 0=右→左 1=下→上 2=左→右 3=上→下
static int ns_lx1 = 0, ns_lx2 = 0, ns_ly1 = 0, ns_ly2 = 0; // スクロール範囲 (窓オフセット範囲)
static int ns_x = 0, ns_y = 0; // スクロール位置 (SHOW窓のオフセット)
static int ns_dir = 1;         // OSCILLATE方向 (1=順方向、-1=逆方向)
static int ns_lastcolor = 0;   // PRINTで色指定が短い/省略時に引き継ぐ色

//論理座標(x,y) → ストリップ物理位置 (layout 0-5、Annexの画像が参照できないため一般的な6構成の割当)
//0=行優先 (上→下、各行は左→右) / 1=行優先ジグザグ / 2=列優先 / 3=列優先ジグザグ
//4=モジュール単位・行方向 (各モジュールns_mw×ns_mhは正、モジュール行内は左→右) / 5=モジュール単位・行方向ジグザグ
static int ns_map(int x, int y) {
    int w = ns_dw, h = ns_dh;
    if (ns_flip) {
        x = w - 1 - x;
        y = h - 1 - y;
    }
    switch (ns_layout) {
        case 1: // 行ジグザグ (奇数行は列反転)
            if (y & 1) x = w - 1 - x;
            return y * w + x;
        case 2: // 列優先 (左→右、各列は上→下)
            return x * h + y;
        case 3: // 列ジグザグ (奇数列は行反転)
            if (x & 1) y = h - 1 - y;
            return x * h + y;
        case 4: { // モジュール単位・行方向 (任意寸法モジュール)
            int mp = ns_mw * ns_mh;  // 1モジュールの画素数
            int mpr = w / ns_mw;     // モジュール行あたりのモジュール数
            return (y / ns_mh) * (mpr * mp) + (x / ns_mw) * mp + (y % ns_mh) * ns_mw + (x % ns_mw);
        }
        case 5: { // モジュール単位・行方向ジグザグ (モジュール行でモジュール列反転、任意寸法)
            int mp = ns_mw * ns_mh;
            int mpr = w / ns_mw;
            int mrow = y / ns_mh, mcol = x / ns_mw;
            if (mrow & 1) mcol = mpr - 1 - mcol;
            return (mrow * mpr + mcol) * mp + (y % ns_mh) * ns_mw + (x % ns_mw);
        }
        default: // 0: 行優先
            return y * w + x;
    }
}

static uint16 ns_getpx(int x, int y) {
    if (x < 0 || y < 0 || x >= ns_cw || y >= ns_ch) {
        return 0; // キャンバス外は黒
    }
    return ns_canvas[y * ns_cw + x];
}

static void ns_setpx(int x, int y, int c) {
    if (x < 0 || y < 0 || x >= ns_cw || y >= ns_ch) {
        return;
    }
    ns_canvas[y * ns_cw + x] = (uint16)(c & 0x7fff);
}

static int ns_check_used() {
    if (!ns_used) {
        command_error(ERR_ILLEGAL_ARGUMENT); // NEOSCROLL.SETUPを先に実行する必要がある
        return 0;
    }
    return 1;
}

//Annex互換の1文字色記号 → RGB555 (未定義文字は-1で色変更なし)
static int ns_color_of(char c) {
    switch (c) {
        case 'B': return neo_pack555(0, 0, 255);     // BLUE
        case 'b': return neo_pack555(0, 0, 128);     // NAVY
        case 'C': return neo_pack555(0, 255, 255);   // CYAN
        case 'c': return neo_pack555(0, 128, 128);   // DARKCYAN
        case 'G': return neo_pack555(0, 255, 0);     // GREEN
        case 'g': return neo_pack555(0, 128, 0);     // DARKGREEN
        case 'K': return 0;                          // BLACK
        case 'k': return neo_pack555(64, 64, 64);    // DARKGREY
        case 'M': return neo_pack555(255, 0, 255);   // MAGENTA
        case 'm': return neo_pack555(128, 0, 0);     // MAROON
        case 'R': return neo_pack555(255, 0, 0);     // RED
        case 'r': return neo_pack555(128, 0, 0);     // DARKRED
        case 'Y': return neo_pack555(255, 255, 0);   // YELLOW
        case 'y': return neo_pack555(173, 255, 47);  // GREENYELLOW
        case 'O': return neo_pack555(255, 165, 0);   // ORANGE
        case 'o': return neo_pack555(128, 128, 0);   // OLIVE
        case 'P': return neo_pack555(128, 0, 128);   // PURPLE
        case 'p': return neo_pack555(255, 192, 203); // PINK
        case 'W': return neo_pack555(255, 255, 255); // WHITE
        case 'w': return neo_pack555(211, 211, 211); // LIGHTGREY
        case 'S': return neo_pack555(192, 192, 192); // SILVER
        default: return -1;
    }
}

//テキスト描画 (oled_fonts.hのビットマップグリフ、オフ画素は透明=キャンバスの色を保持)
//グリフは列単位ビットマップ (data[col]、bit row、LSB=上)。u8g2のgx/gyオフセットを反映
static void ns_draw_text(const char* text, const char* colors) {
    const OLED_Char* tbl = oled_font_tables[ns_font];
    int tb = ns_tbright;
    if (tb < 0) tb = 0;
    else if (tb > 255) tb = 255;
    int r, g, b;
    neo_unpack555(ns_lastcolor, &r, &g, &b);
    if (tb < 255) {
        r = (r * tb) >> 8; g = (g * tb) >> 8; b = (b * tb) >> 8;
    }
    uint16 c = (uint16)neo_pack555(r, g, b);
    int ci = 0;
    for (const char* p = text; *p; p++) {
        if (*p == '\n') { // 改行: 行頭へ、フォント高さ+1で次行
            ns_curx = 0;
            ns_cury += oled_font_h[ns_font] + 1;
            continue;
        }
        if (*p < 0x20 || *p > 0x7E) {
            continue; // 印字可能範囲外はスキップ
        }
        if (colors && colors[ci]) {
            int nc = ns_color_of(colors[ci]);
            if (nc >= 0) {
                ns_lastcolor = nc;
                c = (uint16)nc;
                neo_unpack555(ns_lastcolor, &r, &g, &b);
                if (tb < 255) {
                    r = (r * tb) >> 8; g = (g * tb) >> 8; b = (b * tb) >> 8;
                    c = (uint16)neo_pack555(r, g, b);
                }
            }
        }
        ci++;
        const OLED_Char* ch = &tbl[*p - 0x20];
        int left = ns_curx + ch->gx;
        int top = ns_cury + ch->gy;
        for (int col = 0; col < ch->w; col++) {
            uint16_t bits = ch->data[col];
            for (int row = 0; row < ch->h; row++) {
                if (bits & (1 << row)) {
                    ns_setpx(left + col, top + row, c);
                }
            }
        }
        ns_curx += ch->adv;
    }
}

//NEOSCROLL.SETUP devices,lines,port{,layout}{,width,height}{,flip}{,module_w,module_h}
//表示寸法 = devices*module_w × lines*module_h (合計2730px以内)。module_w/h省略時=8x8。
//canvas省略時=表示寸法
S_INLINE void command_ns_setup() {
    int dev = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int lines = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int port = token_expression();
    IJB_ERR_CHK();
    int layout = 0, w = -1, h = -1, flip = 0, mw = 8, mh = 8;
    int code = token_getCode();
    if (code == TOKEN_COMMA) {
        layout = token_expression();
        IJB_ERR_CHK();
        code = token_getCode();
        if (code == TOKEN_COMMA) {
            w = token_expression();
            IJB_ERR_CHK();
            code = token_getCode();
            if (code == TOKEN_COMMA) {
                h = token_expression();
                IJB_ERR_CHK();
                code = token_getCode();
                if (code == TOKEN_COMMA) {
                    flip = token_expression();
                    IJB_ERR_CHK();
                    code = token_getCode();
                    if (code == TOKEN_COMMA) {
                        mw = token_expression();
                        IJB_ERR_CHK();
                        code = token_getCode();
                        if (code == TOKEN_COMMA) {
                            mh = token_expression();
                            IJB_ERR_CHK();
                        } else {
                            token_back();
                        }
                    } else {
                        token_back();
                    }
                } else {
                    token_back();
                }
            } else {
                token_back();
            }
        } else {
            token_back();
        }
    } else {
        token_back();
    }
    token_end();
    IJB_ERR_CHK();
    if (dev < 1 || dev > 255 || lines < 1 || lines > 255 || port < 0 || port > 11
        || layout < 0 || layout > 5 || flip < 0 || flip > 1
        || mw < 1 || mw > 64 || mh < 1 || mh > 64) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    int dw = dev * mw, dh = lines * mh;
    if ((long)dw * dh > WS_BUFFER_PIXELS) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    if (w < 0 && h < 0) { // キャンバス寸法省略時は表示寸法と同じ
        w = dw;
        h = dh;
    }
    if (w < 1 || h < 1 || (long)w * h > NS_CANVAS_MAX) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    ns_port = port;
    ns_layout = layout;
    ns_flip = flip;
    ns_dw = dw;
    ns_dh = dh;
    ns_mw = mw;
    ns_mh = mh;
    ns_cw = w;
    ns_ch = h;
    memset(ns_canvas, 0, sizeof ns_canvas);
    ns_bright = 128;
    ns_tbright = 255;
    ns_font = 0;
    ns_curx = 0;
    ns_cury = 0;
    ns_mode = 0;
    ns_lx1 = 0;
    ns_lx2 = ns_cw - ns_dw; // 窓オフセット範囲の既定 (キャンバス全体)
    ns_ly1 = 0;
    ns_ly2 = ns_ch - ns_dh;
    ns_x = 0;
    ns_y = 0;
    ns_dir = 1;
    ns_lastcolor = 0;
    ns_used = 1;
}

//NEOSCROLL.DELETE — 状態をリセット (未使用へ戻す)
S_INLINE void command_ns_delete() {
    token_end();
    IJB_ERR_CHK();
    ns_used = 0;
    memset(ns_canvas, 0, sizeof ns_canvas);
    ns_port = 7;
    ns_dw = ns_dh = ns_cw = ns_ch = 8;
    ns_mw = ns_mh = 8;
    ns_layout = ns_flip = 0;
    ns_bright = 128;
    ns_tbright = 255;
    ns_font = 0;
    ns_curx = ns_cury = 0;
    ns_mode = 0;
    ns_lx1 = ns_lx2 = ns_ly1 = ns_ly2 = 0;
    ns_x = ns_y = 0;
    ns_dir = 1;
    ns_lastcolor = 0;
}

//NEOSCROLL.FILL COLOR{,x,y,w,h} — キャンバス領域を塗潰し (省略時は全面)
S_INLINE void command_ns_fill() {
    int c = token_expression();
    IJB_ERR_CHK();
    int x = 0, y = 0, w = ns_cw, h = ns_ch;
    if (token_getCode() == TOKEN_COMMA) {
        x = token_expression();
        IJB_ERR_CHK();
        if (token_getCode() != TOKEN_COMMA) {
            command_error(ERR_SYNTAX_ERROR);
            return;
        }
        y = token_expression();
        IJB_ERR_CHK();
        if (token_getCode() != TOKEN_COMMA) {
            command_error(ERR_SYNTAX_ERROR);
            return;
        }
        w = token_expression();
        IJB_ERR_CHK();
        if (token_getCode() != TOKEN_COMMA) {
            command_error(ERR_SYNTAX_ERROR);
            return;
        }
        h = token_expression();
        IJB_ERR_CHK();
    } else {
        token_back();
    }
    token_end();
    IJB_ERR_CHK();
    if (!ns_check_used()) {
        return;
    }
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            ns_setpx(xx, yy, c);
        }
    }
}

//NEOSCROLL.TEXT.POS x,y — 描画カーソル (左上原点・y下向き。Annexは左下原点なので注意)
S_INLINE void command_ns_textpos() {
    int x = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int y = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (!ns_check_used()) {
        return;
    }
    ns_curx = x;
    ns_cury = y;
}

//NEOSCROLL.TEXT.FONT n — 内蔵フォント 1-5 (1=5x7プロポーショナル、既定1)
S_INLINE void command_ns_textfont() {
    int n = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (n < 1 || n > OLED_FONT_COUNT) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    ns_font = n - 1;
}

//NEOSCROLL.TEXT.BRIGHTNESS b — テキスト明るさ 0-255 (既定255)
S_INLINE void command_ns_textbright() {
    int b = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (b < 0 || b > 255) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    ns_tbright = b;
}

//NEOSCROLL.BRIGHTNESS b — 全体明るさ 0-255 (既定128、SHOW時に適用)
S_INLINE void command_ns_bright() {
    int b = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (b < 0 || b > 255) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    ns_bright = b;
}

//NEOSCROLL.PRINT "text"{,"colors"} — カーソル位置へテキスト描画
//colorsは1文字がtextの1文字に対応する色記号 (RGBYWO... Annex互換)。短い/省略時は前回色を継続
S_INLINE void command_ns_print() {
    Token t; Token_get(t);
    if (t.code != TOKEN_STRING) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    char* text = token_skipstr();
    char* colors = 0;
    Token_get(t);
    if (t.code == TOKEN_COMMA) {
        Token_get(t);
        if (t.code != TOKEN_STRING) {
            command_error(ERR_SYNTAX_ERROR);
            return;
        }
        colors = token_skipstr();
    } else if (t.code != TOKEN_NULL && t.code != TOKEN_ELSE) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    token_end();
    IJB_ERR_CHK();
    if (!ns_check_used()) {
        return;
    }
    ns_draw_text(text, colors);
}

//NEOSCROLL.SHOW x,y — キャンバスの(x,y)を左上とする表示範囲を明るさ適用してLEDへ転送
S_INLINE void command_ns_show() {
    int x = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int y = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (!ns_check_used()) {
        return;
    }
    int bright = ns_bright;
    if (bright < 0) bright = 0;
    else if (bright > 255) bright = 255;
    for (int ly = 0; ly < ns_dh; ly++) {
        for (int lx = 0; lx < ns_dw; lx++) {
            uint16 c = ns_getpx(x + lx, y + ly);
            int r, g, b;
            neo_unpack555(c, &r, &g, &b);
            if (bright < 255) {
                r = (r * bright) >> 8; g = (g * bright) >> 8; b = (b * bright) >> 8;
            }
            int idx = ns_map(lx, ly);
            ws_matrix_buf[idx * 3] = (uint8_t)g;      // GRB順
            ws_matrix_buf[idx * 3 + 1] = (uint8_t)r;
            ws_matrix_buf[idx * 3 + 2] = (uint8_t)b;
        }
    }
    ws_data_mode = WS_MODE_BUFFER;
    ws_data_offset = 0;
    ws_out(ns_port, ns_dw * ns_dh, 1);
}

//NEOSCROLL.LIMITS x1{,x2}{,y1}{,y2} — スクロール範囲 (窓オフセットの範囲。省略時は現状維持)
S_INLINE void command_ns_limits() {
    int x1 = ns_lx1, x2 = ns_lx2, y1 = ns_ly1, y2 = ns_ly2;
    if (token_getCode() == TOKEN_COMMA) {
        x1 = token_expression();
        IJB_ERR_CHK();
        if (token_getCode() == TOKEN_COMMA) {
            x2 = token_expression();
            IJB_ERR_CHK();
            if (token_getCode() == TOKEN_COMMA) {
                y1 = token_expression();
                IJB_ERR_CHK();
                if (token_getCode() == TOKEN_COMMA) {
                    y2 = token_expression();
                    IJB_ERR_CHK();
                } else {
                    token_back();
                }
            } else {
                token_back();
            }
        } else {
            token_back();
        }
    } else {
        token_back();
    }
    token_end();
    IJB_ERR_CHK();
    if (!ns_check_used()) {
        return;
    }
    ns_lx1 = x1;
    ns_lx2 = x2;
    ns_ly1 = y1;
    ns_ly2 = y2;
}

//NEOSCROLL.SYNC — スクロール位置を現在のMODEの始端へリセット (方向も順方向へ)
static void ns_sync() {
    switch (ns_mode) {
        case 0: // 右→左: 始端は右端
            ns_x = ns_lx1 < ns_lx2 ? ns_lx2 : ns_lx1;
            ns_y = ns_ly1;
            break;
        case 1: // 下→上: 始端は下端
            ns_x = ns_lx1;
            ns_y = ns_ly1 < ns_ly2 ? ns_ly2 : ns_ly1;
            break;
        default: // 2=左→右、3=上→下: 始端は左端/上端
            ns_x = ns_lx1;
            ns_y = ns_ly1;
            break;
    }
    ns_dir = 1;
}

S_INLINE void command_ns_sync() {
    token_end();
    IJB_ERR_CHK();
    if (!ns_check_used()) {
        return;
    }
    ns_sync();
}

//NEOSCROLL.MODE m — スクロール方向 0=右→左 1=下→上 2=左→右 3=上→下
S_INLINE void command_ns_mode() {
    int m = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (m < 0 || m > 3) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    ns_mode = m;
}

//1ステップスクロール。戻り値: SCROLL=周回で1、OSCILLATE=始点へ復帰で1
static int ns_advance(int oscillate) {
    int dx = 0, dy = 0;
    switch (ns_mode) {
        case 0: dx = -1; break;
        case 1: dy = -1; break;
        case 2: dx = 1; break;
        default: dy = 1; break;
    }
    int xlo = ns_lx1 < ns_lx2 ? ns_lx1 : ns_lx2;
    int xhi = ns_lx1 < ns_lx2 ? ns_lx2 : ns_lx1;
    int ylo = ns_ly1 < ns_ly2 ? ns_ly1 : ns_ly2;
    int yhi = ns_ly1 < ns_ly2 ? ns_ly2 : ns_ly1;
    if (!oscillate) { // SCROLL: 端で反対端へ周回
        ns_x += dx;
        ns_y += dy;
        int wrapped = 0;
        if (dx) {
            if (ns_x < xlo) { ns_x = xhi; wrapped = 1; }
            else if (ns_x > xhi) { ns_x = xlo; wrapped = 1; }
        }
        if (dy) {
            if (ns_y < ylo) { ns_y = yhi; wrapped = 1; }
            else if (ns_y > yhi) { ns_y = ylo; wrapped = 1; }
        }
        return wrapped;
    }
    // OSCILLATE: 端で反転し、始点 (SYNC位置) へ戻ったら周期完了
    int nx = ns_x + dx * ns_dir;
    int ny = ns_y + dy * ns_dir;
    int done = 0;
    if (dx) {
        if (nx < xlo || nx > xhi) {
            ns_dir = -ns_dir;
            nx = ns_x + dx * ns_dir;
        }
        ns_x = nx;
        if (ns_dir < 0) { // 逆進中: 順方向の始点へ戻ったら完了
            int start = dx < 0 ? xhi : xlo;
            if (ns_x == start) {
                ns_dir = 1;
                done = 1;
            }
        }
    }
    if (dy) {
        if (ny < ylo || ny > yhi) {
            ns_dir = -ns_dir;
            ny = ns_y + dy * ns_dir;
        }
        ns_y = ny;
        if (ns_dir < 0) {
            int start = dy < 0 ? yhi : ylo;
            if (ns_y == start) {
                ns_dir = 1;
                done = 1;
            }
        }
    }
    return done;
}

//NEOSCROLL.SCROLL / NEOSCROLL.OSCILLATE — コマンド形式 (1px進める)
S_INLINE void command_ns_scroll(int oscillate) {
    token_end();
    IJB_ERR_CHK();
    if (!ns_check_used()) {
        return;
    }
    ns_advance(oscillate);
}

//NEOSCROLL.SCROLL / NEOSCROLL.OSCILLATE — 関数形式 (周期完了時に1を返す)
S_INLINE int func_ns_scroll(int oscillate) {
    if (!ns_check_used()) {
        return 0;
    }
    return ns_advance(oscillate);
}

//NEOSCROLL.X() / NEOSCROLL.Y() — 現在のスクロール位置
S_INLINE int func_ns_x() {
    Token t; Token_get(t);
    if (t.code != TOKEN_PAREN_E) {
        command_error(ERR_SYNTAX_ERROR);
        return 0;
    }
    return ns_x;
}

S_INLINE int func_ns_y() {
    Token t; Token_get(t);
    if (t.code != TOKEN_PAREN_E) {
        command_error(ERR_SYNTAX_ERROR);
        return 0;
    }
    return ns_y;
}

#endif
