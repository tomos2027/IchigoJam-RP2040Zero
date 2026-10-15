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
        bool tone_on = !((mixer >> i) & 1);        // bit0-2: 0=トーン有効
        bool noise_on = !((mixer >> (i + 3)) & 1);  // bit3-5: 0=ノイズ有効
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
    // GP14をGPIO出力として設定 (PWM不使用、圧電ブザー直接駆動)
    gpio_init(SOUND_PIN);
    gpio_set_dir(SOUND_PIN, GPIO_OUT);
    gpio_put(SOUND_PIN, 0);

    // PSG初期化 (全チャンネル消音)
    psg_emu_init();

    // オーディオタイマー (~16kHz) 開始
    add_repeating_timer_us(-63, audio_timer_callback, NULL, &audio_timer);

    prev_tone = 0;
}

void set_tone() {
    // 60FPSタイマーから呼ばれる: MMLパーサーが _g.psgtone を設定した後
    // _g.psgtone → PSG period変換 → レジスタ設定
    if (prev_tone != _g.psgtone) {
        if (_g.psgtone) {
            // IchigoJam psgtone値 → 周波数 → PSG period
            uint16_t period = psg_emu_tone_to_period(_g.psgtone);
            psg_emu_set_period(0, period);   // chA周波数
            psg_emu_set_volume(0, 15);        // 最大音量 (圧電ブザーはON/OFFのみ)
            psg_regs[7] &= ~0x01;             // chAトーン有効
        } else {
            // 消音
            psg_emu_set_volume(0, 0);
        }
        prev_tone = _g.psgtone;
    }
}

//クロックを変更したときにPWMの分周比を再設定する
void sound_reconfig() {
    // PWM不使用のため再設定不要
    prev_tone = 0;//次のset_toneで再設定させる
}

static void sound_switch(int on) {
    if (on) {
        psg_regs[7] &= ~0x01;  // chAトーン有効
    } else {
        psg_regs[7] |= 0x3F;   // 全チャンネル無効
    }
}

#endif
