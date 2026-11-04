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
#include "hardware/sync.h"

/* 輝度係数 (0.299/0.587/0.114) と色差係数 (0.492/0.877) のQ8/Q15表現 */
#define NTSCV_KR_B 77      /* 0.299 * 256 四捨五入 */
#define NTSCV_KB_B 29       /* 0.114 * 256 四捨五入 */
#define NTSCV_KG_B 150      /* 0.587 * 256 四捨五入 */
#define NTSCV_U_Q8 126      /* 0.492 * 256 四捨五入 */
#define NTSCV_V_Q8 224      /* 0.877 * 256 切り捨て */
#define NTSCV_LUMA_STEPS 7  /* 黒(2)→白(9) の7段分へY(0..255)を射影 */

/* 位相補正 (度)。サブキャリア1サンプル = 90° */
#define NTSCV_CHROMA_PHASE_DEG 30

/* 暗色の視認性保護。
   10段DAC (黒=2, 白=9, 輝度7段) では輝度Yが低い色 (既定MSX16色ではCOLOR 4
   濃青 y=61 のみ) は黒に対するコントラストが約1.5段しかなく、加えて色差に
   より半分のサンプルが黒以下に落ちて文字が視認できない。
   ルール1 (輝度引き上げ): 0<y<NTSCV_DARK_LUMA_MIN の色は輝度を引き上げる。
     色差 (uq,vq) は元の輝度で計算した値を保持するため、色相と彩度を保った
     まま明るくなる (信号空間で白を加えるのと等価)。黒 (y=0) は対象外。
   ルール2 (ドット平均の黒化落込み防止): 色差はドット内の輝度を位相ごとに
     変調するため、暗い位相に当たったドットは背景 (黒) に埋もれて「ドットが
     欠けて」見える。1ドット=2サンプル (隣接2位相) または3サンプル (連続3位相)
     の表示平均が黒+2段 (レベル4) を下回らないよう色差ベクトルを一様に縮小
     する (色相は不変)。既定16色ではCOLOR 4/6/8/12/13 が該当する。 */
#define NTSCV_DARK_LUMA_MIN 90

/* ドット平均の解析値目標 (表示値は>>8切り捨てで解析値より最大1段落ちるため
   解析値でレベル4.5以上を確保する。全範囲スイープで表示値の制約達成を確認済) */
#define NTSCV_DOT_MIN1 37   /* 単位相:  y + c     >= 37   (レベル>=3.5) */
#define NTSCV_DOT_MIN2 150  /* 2サンプル: 2y + c和 >= 150  (平均レベル>=4.5) */
#define NTSCV_DOT_MIN3 220  /* 3サンプル: 3y + c和 >= 220  (平均レベル>=4.5) */

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

/* 視認性ルールを適用していない生パレット (色0-15のみ)。
   VIDEO9の白レベル輝度+ティント描画が色差の源として使う
   (ルール2はドット平均保護のためのものだが、白レベル輝度方式では
   輝度を白に固定するため不要。生の色差の方がティントが濃い) */
uint32_t ntscv_palette_plain[16][2] __attribute__((aligned(4)));

/* RGB → 4位相のPWM比較値を計算する (仕様書§2.5の式 + 視認性ルール) */
static void calc_levels(uint16_t out4[4], uint8_t b, uint8_t r, uint8_t g, bool rules)
{
    int32_t y = (NTSCV_KG_B * g + NTSCV_KB_B * b + NTSCV_KR_B * r + 128) >> 8;
    int32_t uq = ((int32_t)(b - y) * NTSCV_U_Q8) >> 8;
    int32_t vq = ((int32_t)(r - y) * NTSCV_V_Q8) >> 8;
    if (!rules) {
        for (int k = 0; k < 4; k++) {
            int32_t chroma = (uq * chroma_sin_q15[k] + vq * chroma_cos_q15[k] + 16384) >> 15;
            int32_t level = 2 + ((y * NTSCV_LUMA_STEPS + chroma * NTSCV_LUMA_STEPS + 128) >> 8);
            if (level < 0) level = 0;
            if (level > 10) level = 10;
            out4[k] = (uint16_t)level;
        }
        return;
    }
    /* ルール1: 暗色の輝度引き上げ (色差は元の輝度基準のまま保持) */
    if (y > 0 && y < NTSCV_DARK_LUMA_MIN) y = NTSCV_DARK_LUMA_MIN;
    if (y > 0) {
        /* ルール2: ドット平均の黒化落込み防止。全ドット (2/3サンプル窓) の
           解析平均がレベル4.5以上となるよう色差ベクトルを一様に縮小する。
           y>=90のためneed値はすべて負、制約違反時はc和も負なので0除算なし。 */
        int32_t c[4];
        int32_t s1, s2, s3;
        for (int k = 0; k < 4; k++)
            c[k] = (uq * chroma_sin_q15[k] + vq * chroma_cos_q15[k] + 16384) >> 15;
        s1 = s2 = s3 = c[0] + c[1] + c[2] + c[3];
        for (int k = 0; k < 4; k++) {
            if (c[k] < s1) s1 = c[k];
            int32_t a2 = c[k] + c[(k + 1) & 3];
            if (a2 < s2) s2 = a2;
            int32_t a3 = a2 + c[(k + 2) & 3];
            if (a3 < s3) s3 = a3;
        }
        int64_t f = 65536;
        int32_t n1 = NTSCV_DOT_MIN1 - y;
        int32_t n2 = NTSCV_DOT_MIN2 - 2 * y;
        int32_t n3 = NTSCV_DOT_MIN3 - 3 * y;
        if (s1 < n1) { int64_t ff = (int64_t)n1 * 65536 / s1; if (ff < f) f = ff; }
        if (s2 < n2) { int64_t ff = (int64_t)n2 * 65536 / s2; if (ff < f) f = ff; }
        if (s3 < n3) { int64_t ff = (int64_t)n3 * 65536 / s3; if (ff < f) f = ff; }
        if (f < 65536) {
            uq = (int32_t)((int64_t)uq * f >> 16);
            vq = (int32_t)((int64_t)vq * f >> 16);
        }
    }
    for (int k = 0; k < 4; k++) {
        int32_t chroma = (uq * chroma_sin_q15[k] + vq * chroma_cos_q15[k] + 16384) >> 15;
        int32_t level = 2 + ((y * NTSCV_LUMA_STEPS + chroma * NTSCV_LUMA_STEPS + 128) >> 8);
        if (level < 0) level = 0;
        if (level > 10) level = 10;
        out4[k] = (uint16_t)level;
    }
}

void ntscv_palette_calc(uint16_t out4[4], uint8_t b, uint8_t r, uint8_t g)
{
    calc_levels(out4, b, r, g, true);
}

/* 視認性ルールを適用しない生のレベル計算 (VIDEO9のティント色差の源) */
void ntscv_palette_calc_plain(uint16_t out4[4], uint8_t b, uint8_t r, uint8_t g)
{
    calc_levels(out4, b, r, g, false);
}

void ntscv_palette_set(uint16_t idx, uint8_t b, uint8_t r, uint8_t g)
{
    if (idx > 256) return;
    uint16_t s[4];
    ntscv_palette_calc(s, b, r, g);
#ifdef NTSCV_COLOR_GRAYTEST
    /* 診断: 色差変調を除去し、各色をその平均レベル一色にする。
       ドット欠けが色差変調由来かの切り分け用 (バースト/レンダラ/TVRAM経路は
       そのまま)。バー (bar_pair) はntscv_palette_calcを直接呼ぶため影響しない */
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
        /* 描画表 (ij_color_run) をDMA合成IRQと競合しないよう一旦更新する */
        uint32_t save = save_and_disable_interrupts();
        uint16_t p[4];
        ntscv_palette_calc_plain(p, b, r, g); /* ルール無しの生レベル (ティント用) */
#ifdef NTSCV_COLOR_GRAYTEST
        {
            uint32_t sum = (uint32_t)p[0] + p[1] + p[2] + p[3];
            uint16_t mean = (uint16_t)((sum + 2) >> 2);
            p[0] = p[1] = p[2] = p[3] = mean;
        }
#endif
        ntscv_palette_plain[idx][0] = (uint32_t)p[1] << 16 | p[0];
        ntscv_palette_plain[idx][1] = (uint32_t)p[3] << 16 | p[2];
        ntscv_render_palette_changed();
        restore_interrupts(save);
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
