#ifndef __SOUND_H__
#define __SOUND_H__
#include "all_includes.h"

//Waveshare RP2040-Zero: 側面パッドGP14 (IchigoJam_PのGP20から変更: 裏面パッド不使用のため)
#define SOUND_PIN 14

static uint16 prev_tone = 0;
static struct repeating_timer audio_timer;

// オーディオタイマーコールバック (~16kHz)
// PSGエミュレーションを1ステップ進め、チャンネル出力をGP14へ直接出力
// 圧電ブザー対応: PWMデューティ変調ではなくGPIO直接トグル (方形波)
static bool audio_timer_callback(repeating_timer_t* rt) {
    psg_emu_tick();  // PSG状態更新 (各ch出力トグル)

    // アクティブなチャンネルの出力をGP14へ (OR混合)
    uint8_t mixer = psg_regs[7];
    bool out = false;
    for (int i = 0; i < 3 && !out; i++) {
        bool tone_on = !((mixer >> i) & 1);
        bool noise_on = !((mixer >> (i + 3)) & 1);
        uint8_t vol = psg_regs[8 + i] & 0x0F;
        if (vol == 0) continue;
        if (tone_on && psg_ch[i].output) out = true;
        if (!out && noise_on && psg_noise.output) out = true;
    }
    gpio_put(SOUND_PIN, out);
    return true;
}

void sound_init() {
    _g.psgratio = 1;
    // GP14をGPIO出力として設定 (PWM不使用、圧電ブザー/スピーカー直接駆動)
    gpio_init(SOUND_PIN);
    gpio_set_dir(SOUND_PIN, GPIO_OUT);
    gpio_put(SOUND_PIN, 0);

    // PSG初期化 (全チャンネル消音)
    psg_emu_init();

    // MML状態初期化
    for (int ch = 0; ch < 3; ch++) {
        mml_init_state(&mml_ch[ch]);
    }

    // オーディオタイマー (~16kHz) 開始
    add_repeating_timer_us(-63, audio_timer_callback, NULL, &audio_timer);

    prev_tone = 0;
}

// MMLパーサーが直接PSGレジスタを書くため、set_tone()は不要
// (Phase 1からの移行: 60FPSタイマーから呼ばれていたが、psg_tick内で処理)
void set_tone() {
    // psg.hのpsg_process_voice()がPSGレジスタを直接書き込むため何もしない
}

//クロックを変更したとき (PWM不使用のため再設定不要)
void sound_reconfig() {
    prev_tone = 0;
}

static void sound_switch(int on) {
    if (on) {
        psg_regs[7] &= ~0x01;  // chAトーン有効
    } else {
        psg_regs[7] |= 0x3F;   // 全チャンネル無効
    }
}

#endif
