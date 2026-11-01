/*----------------------------------------------------------------------------

ntscv_palette.c — カラーパレット生成 (docs/ntsc_video_spec.md §2.5)

Copyright (c) 2026 the IchigoJam-RP2040Zero authors. MIT license.

NTSC標準の色符号化をそのまま固定小数点化しただけの独自実装:
  輝度   Y = 0.299R + 0.587G + 0.114B
  色差   U = 0.492(B-Y) (青系)、V = 0.877(R-Y) (赤系)
  信号   level(k) = 2 + (Y*7 + 7*(U*sinθk + V*cosθk)) / 256
  位相   θk = 90°*k + NTSCV_CHROMA_PHASE_DEG (サブキャリア1周期4サンプル)

NTSCV_CHROMA_PHASE_DEG(既定30°)はPWMゼロ次ホールドと外部RCフィルタ
(75Ω+560pF)による再構成遅延を補正する経験値。バーストは−sinグリッド
(サンプル値 2,1,2,3) で、パレット側位相+30°との相対関係で色相が合う。

----------------------------------------------------------------------------*/

#include "ntscv.h"

/* 輝度係数 (0.299/0.587/0.114) と色差係数 (0.492/0.877) のQ8/Q15表現 */
#define NTSCV_KR_B 77      /* 0.299 * 256 四捨五入 */
#define NTSCV_KB_B 29       /* 0.114 * 256 四捨五入 */
#define NTSCV_KG_B 150      /* 0.587 * 256 四捨五入 */
#define NTSCV_U_Q8 126      /* 0.492 * 256 四捨五入 */
#define NTSCV_V_Q8 224      /* 0.877 * 256 切り捨て */
#define NTSCV_LUMA_STEPS 7  /* 黒(2)→白(9) の7段分へY(0..255)を射影 */

/* 位相補正 (度)。サブキャリア1サンプル = 90° */
#define NTSCV_CHROMA_PHASE_DEG 30

/* sin/cosのQ15表 (θk = 90°k + 30°, k=0..3)
   sin30°=0.5, sin120°=√3/2, sin210°=-0.5, sin300°=-√3/2
   cos30°=√3/2, cos120°=-0.5, cos210°=-√3/2, cos300°=0.5 */
static const int32_t chroma_sin_q15[4] = {
    16384, 28378, -16384, -28378
};
static const int32_t chroma_cos_q15[4] = {
    28378, -16384, -28378, 16384
};

uint32_t ntscv_palette[257][2] __attribute__((aligned(4)));

/* RGB → 4位相のPWM比較値を計算する (仕様書§2.5の式) */
void ntscv_palette_calc(uint16_t out4[4], uint8_t b, uint8_t r, uint8_t g)
{
    int32_t y = (NTSCV_KG_B * g + NTSCV_KB_B * b + NTSCV_KR_B * r + 128) >> 8;
    int32_t uq = ((int32_t)(b - y) * NTSCV_U_Q8) >> 8;
    int32_t vq = ((int32_t)(r - y) * NTSCV_V_Q8) >> 8;
    for (int k = 0; k < 4; k++) {
        int32_t chroma = (uq * chroma_sin_q15[k] + vq * chroma_cos_q15[k] + 16384) >> 15;
        int32_t level = 2 + ((y * NTSCV_LUMA_STEPS + chroma * NTSCV_LUMA_STEPS + 128) >> 8);
        if (level < 0) level = 0;
        if (level > 10) level = 10;
        out4[k] = (uint16_t)level;
    }
}

void ntscv_palette_set(uint16_t idx, uint8_t b, uint8_t r, uint8_t g)
{
    if (idx > 256) return;
    uint16_t s[4];
    ntscv_palette_calc(s, b, r, g);
    /* 位相0-1 / 2-3 を32ビットペアにパック (リトルエンディアン) */
    ntscv_palette[idx][0] = (uint32_t)s[1] << 16 | s[0];
    ntscv_palette[idx][1] = (uint32_t)s[3] << 16 | s[2];
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
