#ifndef __DISPLAY_H__
#define __DISPLAY_H__
#include "all_includes.h"

#define CHAR_MAX_COLS 32
#define CHAR_MAX_ROWS 24

//Waveshare RP2040-ZeroはNTSCコンポジット出力専用 (HDMI/DVI非対応のためDVI機能は削除)

void ntsc_init();//init.hで定義

void video_on() {
    SCREEN_W = CHAR_MAX_COLS >> _g.screen_big;
    SCREEN_H = CHAR_MAX_ROWS >> _g.screen_big;
    if (ntsc_active()) {
        return;//動作中なら再初期化不要 (VIDEOコマンドの反転/拡大は内容の変更だけのため)
    }
    //NTSCはflash書込等で割り込みが長時間止まると走査線カウンタが表示位置からずれるため、
    //開始するときは毎回チェーンを再初期化して位相を合わせ直す
    ntsc_release();
    ntsc_init();
}

//クロックの変更機能はつけない
void video_off(int clkdiv) {
    stop_composite();
}

static inline int video_active() {
    return ntsc_active();
}

void video_waitSync(uint num) {
    //NTSCモードでは1フレームごとにdrawcountが増えるのを待つ
    for (uint i = 0; i < num; i++) {
        uint16_t count = drawcount;
        uint64_t start = time_us_64();
        while (drawcount == count) {
            if (time_us_64() - start > 100000) {
                return;//100msで諦める (映像停止中などに起動が固まるのを防ぐ)
            }
            __wfi();
        }
    }
}

#endif
