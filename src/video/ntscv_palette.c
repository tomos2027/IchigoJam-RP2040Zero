/*----------------------------------------------------------------------------

ntscv_palette.c — カラーパレット生成

Copyright (c) 2026 the IchigoJam-RP2040Zero authors. MIT license.

旧ドライバ (rp2040_pwm_ntsc_textgraph、同一ハードウェア+TVでカラー表示の
実績あり) と同一の式を移植したもの:

  輝度   Y = 0.587G + 0.114B + 0.299R
  信号   N = Y + 0.4921(B-Y)sinθ + 0.8773(R-Y)cosθ
  出力   S = N*(white-black)/255 + black  (white=9, black=2)

4位相 (θ=0°,90°,180°,270°+30°補正) の係数は旧ドライバの整数値
(441/1361/764/-786) をそのまま使う。上位クリップは意図的に行わない
(旧ドライバも s<0 のみクリップし、飽和色の明側はPWMの頭で潰れていた)。

----------------------------------------------------------------------------*/

#include "ntscv.h"

uint32_t ntscv_palette[257][2] __attribute__((aligned(4)));

/* RGB → 4位相のPWM比較値を計算する (旧ドライバと同一の式) */
void ntscv_palette_calc(uint16_t out4[4], uint8_t b, uint8_t r, uint8_t g)
{
    int32_t y = (150 * g + 29 * b + 77 * r + 128) / 256;

    /* 微妙な色ズレを修正する30°位相補正込みの係数 (旧ドライバと同一) */
    int32_t b_y_1 = (b - y) * 441;
    int32_t r_y_1 = (r - y) * 1361;
    int32_t b_y_2 = (b - y) * 764;
    int32_t r_y_2 = (r - y) * (-786);

    int32_t s;
    s = (y * 1792 + b_y_1 + r_y_1 + 2 * 65536 + 32768) / 65536;
    out4[0] = (uint16_t)(s < 0 ? 0 : s);
    s = (y * 1792 + b_y_2 + r_y_2 + 2 * 65536 + 32768) / 65536;
    out4[1] = (uint16_t)(s < 0 ? 0 : s);
    s = (y * 1792 - b_y_1 - r_y_1 + 2 * 65536 + 32768) / 65536;
    out4[2] = (uint16_t)(s < 0 ? 0 : s);
    s = (y * 1792 - b_y_2 - r_y_2 + 2 * 65536 + 32768) / 65536;
    out4[3] = (uint16_t)(s < 0 ? 0 : s);
}

void ntscv_palette_set(uint16_t idx, uint8_t b, uint8_t r, uint8_t g)
{
    if (idx > 256) return;
    uint16_t s[4];
    ntscv_palette_calc(s, b, r, g);
#ifdef NTSCV_COLOR_GRAYTEST
    /* 診断: 色差変調を除去し、各色をその平均レベル一色にする。
       ドット欠けが色差変調由来かの切り分け用 */
    {
        uint32_t sum = (uint32_t)s[0] + s[1] + s[2] + s[3];
        uint16_t mean = (uint16_t)((sum + 2) >> 2);
        s[0] = s[1] = s[2] = s[3] = mean;
    }
#endif
    /* 位相0-1 / 2-3 を32ビットペアにパック (リトルエンディアン) */
    ntscv_palette[idx][0] = (uint32_t)s[1] << 16 | s[0];
    ntscv_palette[idx][1] = (uint32_t)s[3] << 16 | s[2];
    if (idx < 16) {
        /* 描画表の再構築。palette_changed内部で色ごとに短い割込み禁止区間を
           取る (全体を禁止するとDMA合成の再arm期限508µsを超え、起動直後の
           画面が崩れるため) */
        ntscv_render_palette_changed();
    }
}

void ntscv_palette_default(void)
{
    /* 0-7: RGBビット組合せ (bit0=青、bit1=赤、bit2=緑)、8-15: 半輝度 */
    for (int i = 0; i < 8; i++) {
        ntscv_palette_set((uint16_t)i,
                          (uint8_t)(255 * (i & 1)),
                          (uint8_t)(255 * ((i >> 1) & 1)),
                          (uint8_t)(255 * (i >> 2)));
        ntscv_palette_set((uint16_t)(i + 8),
                          (uint8_t)(128 * (i & 1)),
                          (uint8_t)(128 * ((i >> 1) & 1)),
                          (uint8_t)(128 * (i >> 2)));
    }
    for (int i = 16; i < 256; i++) {
        ntscv_palette_set((uint16_t)i, 255, 255, 255);
    }
    ntscv_palette_set(256, 0, 0, 0); /* 背景 = 黒 */
}
