// AY-3-8910 PSG Emulation (MSX-compatible)
// 3 tone channels + 1 noise channel + 1 envelope generator
// Based on: Yamaha YM2149 / AY-3-8910 PSG
// Reference clock: 1789772 Hz (MSX standard)

#ifndef __PSG_EMU_H__
#define __PSG_EMU_H__
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"   // __dmb (core間の書き込み順序保証)

// PSGクロック (MSX標準)
#define PSG_CLOCK       1789772
// オーディオ割り込み周期 (18µs ≈ 55.6kHz)
// 28kHzでは高音(方形波)の高調波がナイキスト(14kHz)を超えて折り返し、
// 「割れた」音になっていたため倍にして折り返しを低減する
#define PSG_EMU_TIMER_US 18
// 1割り込みあたりのPSGクロック数 (2のべき乗→小数補間をシフト演算で処理)
#define PSG_TICK_CLOCKS 32
// エミュレーションの実効PSGクロック (18µsごとに32クロック = 1777778Hz)
// 周波数→period変換をこの値で行うことで単声PWMとピッチが完全一致する
// (PSG_TICK_CLOCKS/PSG_EMU_TIMER_USの比は不変なので音程・ノイズ・エンベロープ速度も不変)
#define PSG_EMU_CLOCK  (PSG_TICK_CLOCKS * 1000000L / PSG_EMU_TIMER_US)

// PSGレジスタ (R0-R13)
// core0 (60FPS/MML) が書き、core1 (オーディオISR) が読むため volatile。
// 書き込み後に __dmb() で順序を保証する。
static volatile uint8_t psg_regs[16];

// チャンネル状態 (3 tone channels)
static struct {
    int32_t counter;   // 周期カウンタ (DDS)
    bool output;       // 現在の出力状態
    uint8_t gate;      // このtick内でHIGHだった割合 0-PSG_TICK_CLOCKS (小数エッジ補間用)
} psg_ch[3];

// 内部DDSピリオド (16bit拡張: AY実機の12bit下限は~218Hzだが、
// IchigoJamの低音域O1~O3 (32-217Hz) を再生するため内部は16bitで保持)
// core0が書きcore1が読むため volatile
static volatile uint16_t psg_ch_period[3];

// ノイズ状態
static struct {
    int32_t counter;
    uint32_t lfsr;     // 17bit LFSR
    bool output;
} psg_noise;

// エンベロープ状態
static struct {
    int32_t counter;
    uint8_t position;  // 0-31 (32step: AY実機同様、三角波は上下で32カウント)
    uint8_t level;     // 現在の音量 0-15
    bool hold;         // 停止後 hold
} psg_env;

// PWM設定はsound.hで管理 (SOUND_PWM_WRAP=1023, キャリア~154kHz)
// PSG_MIX_MAXはPSGエミュレーション内部での最大値
#define PSG_MIX_MAX 45

// エンベロープレベル計算
// shape (R13 bits 0-3): bit3=continue, bit2=attack, bit1=alternate, bit0=hold
// pos: 0-31 の32ステップ位置
//   attack  : 上昇 / alternate で三角波(上→下)
//   decay   : 下降 / alternate で三角波(下→上)
//   alternate なしは16ステップで折り返すノコギリ波
static uint8_t __not_in_flash_func(psg_env_calc)(uint8_t shape, uint8_t pos) {
    uint8_t p = pos & 31;
    if (shape & 0x04) { // attack
        return (shape & 0x02) ? (p < 16 ? p : 31 - p) // 三角波 (上→下)
                              : (p & 15);             // ノコギリ波 (上)
    } else { // decay
        return (shape & 0x02) ? (p < 16 ? 15 - p : p - 16) // 三角波 (下→上)
                              : (uint8_t)(15 - (p & 15));  // ノコギリ波 (下)
    }
}

// PSGレジスタ書き込み
void psg_emu_write(uint8_t reg, uint8_t val) {
    if (reg > 15) return;
    psg_regs[reg] = val;
    // R0-R5書き込み時: 12bitレジスタペアから内部DDSピリオドを再構築 (MSX互換: SOUND経由は12bit上限)
    if (reg <= 5) {
        int ch = reg >> 1;
        psg_ch_period[ch] = psg_regs[ch * 2] | ((psg_regs[ch * 2 + 1] & 0x0F) << 8);
    }
    // 特定レジスタの変更時に内部状態を更新
    switch (reg) {
        case 13: // エンベロープ形状
            psg_env.position = 0;
            psg_env.counter = 0;
            psg_env.hold = false;
            psg_env.level = psg_env_calc(val, 0);
            break;
    }
    __dmb(); // core1のオーディオISRへ書き込み順序を保証
}

// トーン周波数設定 (MMLパーサーから使用)
// period: 内部DDSは16bitをそのまま使用 (O1低音域まで対応)
//         上位12bitをMSXレジスタR0/R1, R2/R3, R4/R5へミラー
void psg_emu_set_period(int ch, uint16_t period) {
    if (ch < 0 || ch > 2) return;
    uint16_t old = psg_ch_period[ch];
    if (!period) {
        psg_ch[ch].counter = 0;
    } else if (old && old != period) {
        // 位相(periodに対する割合)を保ったまま周期を変える。
        // これをしないと、より高い音(短いperiod)へ切り替えた瞬間に
        // 旧位相が新periodを超えて不要なトグル(出力反転)が起き、
        // ノート切替ごとに「プツ」というクリックが出る。
        int32_t c = (int32_t)((int64_t)psg_ch[ch].counter * period / old);
        if (c >= period) c = period - 1;
        psg_ch[ch].counter = c;
    }
    psg_ch_period[ch] = period;
    psg_regs[ch * 2] = period & 0xFF;         // fine (12bitミラー)
    psg_regs[ch * 2 + 1] = (period >> 8) & 0x0F; // coarse
    __dmb();
}

// 音量設定 (0-15、bit4=エンベロープ使用)
void psg_emu_set_volume(int ch, uint8_t vol) {
    if (ch < 0 || ch > 2) return;
    psg_regs[8 + ch] = vol & 0x1F;
    __dmb();
}

// ミキサー設定 (R7: bit0-2=tone off, bit3-5=noise off, 0=有効)
void psg_emu_set_mixer(uint8_t mixer) {
    psg_regs[7] = mixer & 0x3F;
    __dmb();
}

// 指定chのトーンを有効/無効 (R7 bit0-2、0=有効。ノイズ等他のビットは維持) - 多声MML用
void psg_emu_set_tone_enable(int ch, bool enable) {
    if (ch < 0 || ch > 2)
        return;
    if (enable)
        psg_regs[7] &= ~(1 << ch);
    else
        psg_regs[7] |= (1 << ch);
    __dmb();
}

// 周波数→PSG period変換 (エミュ実効クロックで計算: 単声PWMとピッチ完全一致)
// 16bitまで対応 (AY実機の12bit下限~218Hzを超え、IchigoJam O1の32Hzまでカバー)
uint16_t psg_emu_freq_to_period(int freq_hz) {
    if (freq_hz < 1) return 0;
    uint32_t period = (uint32_t)PSG_EMU_CLOCK / (2 * freq_hz);
    if (period > 65535) period = 65535;
    if (period < 1) period = 1;
    return (uint16_t)period;
}

// IchigoJam psgtone値→PSG period変換
// psgtone値は周波数に反比例 (freq = 7830 / psgtone)
uint16_t psg_emu_tone_to_period(int psgtone) {
    if (psgtone < 1) return 0;
    int freq = 60 * 261 / 2 / psgtone;
    return psg_emu_freq_to_period(freq);
}

// PSG初期化 (全チャンネル消音)
void psg_emu_init() {
    for (int i = 0; i < 16; i++) psg_regs[i] = 0;
    psg_regs[7] = 0x3F;  // 全チャンネル無効
    for (int i = 0; i < 3; i++) {
        psg_ch[i].counter = 0;
        psg_ch[i].output = false;
        psg_ch[i].gate = 0;
        psg_ch_period[i] = 0;
    }
    psg_noise.counter = 0;
    psg_noise.lfsr = 0x1FFFF;  // 17bit LFSR初期値
    psg_noise.output = false;
    psg_env.counter = 0;
    psg_env.position = 0;
    psg_env.level = 0;
    psg_env.hold = false;
    __dmb();
}

// メインtick (約56kHzオーディオタイマーから呼ばれる)
// 3chトーン+ノイズ+エンベロープを処理し、双極性混合値を返す
// 戻り値: -45..+45 (3ch各±15の双極性混合、DCオフセットなし)
int8_t __not_in_flash_func(psg_emu_tick)(void) {
    __dmb(); // core0が書いたレジスタを確実に読む
    // --- トーンチャンネル (小数エッジ補間付きDDS) ---
    // トグル点がtick途中に存在する場合、tick内のHIGH割合 (0-PSG_TICK_CLOCKS) を算出して
    // エッジを線形補間する → エッジ量子化ノイズ(高音のビットクラッシュ)を低減
    for (int i = 0; i < 3; i++) {
        uint16_t period = psg_ch_period[i];
        if (period == 0) {
            psg_ch[i].output = false;
            psg_ch[i].gate = 0;
            continue;
        }
        psg_ch[i].counter += PSG_TICK_CLOCKS;
        int toggles = 0;
        while (psg_ch[i].counter >= period) {
            psg_ch[i].counter -= period;
            psg_ch[i].output = !psg_ch[i].output;
            toggles++;
        }
        if (toggles == 1) {
            // counter = エッジからの経過時間 → このtick内のHIGH割合 (×64)
            // ※period変更直後は前音符の持ち越し位相でcb>64になり得る → クランプ
            //   (gate破壊 = ノート切替ごとの「プツ」音の原因)
            uint32_t cb = psg_ch[i].counter;
            if (cb > PSG_TICK_CLOCKS)
                cb = PSG_TICK_CLOCKS;
            psg_ch[i].gate = psg_ch[i].output ? cb : (PSG_TICK_CLOCKS - cb);
        } else {
            // トグル0回 または period<64の超高音で多回トグル: 硬いゲート
            psg_ch[i].gate = psg_ch[i].output ? PSG_TICK_CLOCKS : 0;
        }
    }

    // --- ノイズチャンネル ---
    uint8_t noise_period = psg_regs[6] & 0x1F;
    if (noise_period > 0) {
        psg_noise.counter += PSG_TICK_CLOCKS;
        // ノイズはトーンの半分のレートでシフト
        while (psg_noise.counter >= (noise_period << 4)) {
            psg_noise.counter -= (noise_period << 4);
            // 17bit LFSR (AY-3-8910準拠のタップ: bit0 ^ bit3)
            uint32_t bit = ((psg_noise.lfsr >> 0) ^ (psg_noise.lfsr >> 3)) & 1;
            psg_noise.lfsr = (psg_noise.lfsr >> 1) | (bit << 16);
            psg_noise.output = (psg_noise.lfsr >> 16) & 1;
        }
    }

    // --- エンベロープ ---
    uint16_t env_period = psg_regs[11] | (psg_regs[12] << 8);
    uint8_t env_shape = psg_regs[13] & 0x0F;
    bool env_active = (env_period > 0);
    if (env_active && !psg_env.hold) {
        psg_env.counter += PSG_TICK_CLOCKS;
        while (psg_env.counter >= env_period) {
            psg_env.counter -= env_period;
            psg_env.level = psg_env_calc(env_shape, psg_env.position);
            uint8_t next = (psg_env.position + 1) & 31;
            // サイクル終端: alternateは32step(三角波)、それ以外は16step(ノコギリ波)
            bool cycle_end = (env_shape & 0x02) ? (next == 0) : (next == 16);
            if (cycle_end && (!(env_shape & 0x08) || (env_shape & 0x01))) {
                // continueなし or hold → 最終レベルで停止
                psg_env.hold = true;
                psg_env.level = (env_shape & 0x04) ? 15 : 0;
                break;
            }
            psg_env.position = next;
        }
    }

    // --- ミキサー (双極性: 各ch ±音量、トーンは小数エッジ補間) ---
    uint8_t mixer = psg_regs[7];
    int8_t mixed = 0;

    for (int i = 0; i < 3; i++) {
        uint8_t vol_reg = psg_regs[8 + i];
        bool use_env = (vol_reg & 0x10) != 0;
        uint8_t vol = use_env ? psg_env.level : (vol_reg & 0x0F);

        bool tone_on = !((mixer >> i) & 1);    // bit0-2: 0=tone on
        bool noise_on = !((mixer >> (i + 3)) & 1); // bit3-5: 0=noise on

        if (tone_on || noise_on) {
            if (noise_on) {
                // ノイズ併用chは硬いゲート (ノイズは元々広帯域で補間の効果が薄い)
                bool out = (tone_on && psg_ch[i].output) || psg_noise.output;
                mixed += out ? (int8_t)vol : -(int8_t)vol;
            } else {
                // トーンのみ: 小数補間付き双極性出力 (gate 0-N → -音量..+音量)
                // vol×(2×gate-N)/N: gate=0→-音量, N→+音量, N/2→0
                // (PSG_TICK_CLOCKSに依存しない式: 32→÷32がシフトに最適化される)
                mixed += (int8_t)(vol * (2 * (int)psg_ch[i].gate - PSG_TICK_CLOCKS) / PSG_TICK_CLOCKS);
            }
        }
        // トーン・ノイズとも無効なch (ミキサーR7でOFF) は0出力
    }

    return mixed;
}

// 全チャンネル消音 (SOUND off / MML終了時)
void psg_emu_silence() {
    psg_regs[8] = 0;
    psg_regs[9] = 0;
    psg_regs[10] = 0;
    psg_regs[7] = 0x3F;  // 全トーン・ノイズ無効
    __dmb();
}

#endif
