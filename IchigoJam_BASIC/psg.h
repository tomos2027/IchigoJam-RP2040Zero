// Copyright 2014-2024 the IchigoJam authors. All rights reserved. MIT license.

#ifndef __PSG_H__
#define __PSG_H__

// playMML (3ch MMLパーサー、PWM音声出力)

// 定義必要
#ifndef PSG_TICK_FREQ
#define PSG_TICK_FREQ 60 // for every 1 frame
#endif
#ifndef PSG_TICK_PER_SEC
#define PSG_TICK_PER_SEC 60
#endif
#define PSG_DEFAULT_OCT 3 // ver 1.2
#define PSG_DEFAULT_LEN 8 // == 4分音符
#define PSG_DEFAULT_TEMPO 120
#define PSG_DEFAULT_VOLUME 15

S_INLINE void psg_tick();
S_INLINE void psg_playMML(char* mml);
void psg_playMML_multi(char* mml0, char* mml1, char* mml2);
S_INLINE void psg_beep(int tone, int len);
S_INLINE void psg_tempo(int tempo);

// sound.h
S_INLINE void sound_tick();
static void sound_switch(int on);
static void sound_init();
void sound_set_multi_mode(bool on, bool by_play); // 多声PSGモード切替 (sound.hで定義)

//===== 音声出力モード (sound.hと共有、psg.hが先にincludeされるためここで定義) =====
// false: PWMトーンモード (BEEP・単声PLAY: ハードPWM方形波でクリーンな音)
// true:  多声PSGエミュモード (PLAY多声・SOUND: ~16kHz割り込みで3ch混合デューティ出力)
static bool sound_multi_mode = false;
static bool sound_multi_play = false; // 多声PLAYがモードON (全声終了で自動OFF)

//===== 3ch MML状態 (多声PLAY対応) =====
// 単声 (BEEP・PLAY 1声): voice 0 → PWMトーン出力
// 多声 (PLAY 2-3声): 各voice → PSGエミュ (psg_emu.h) → 16kHz混合出力
struct mml_state {
    char* mml;        // MML文字列ポインタ (NULL=停止中)
    char* rep;        // リピート位置 ($コマンド)
    uint8_t oct;      // 現在のオクターブ
    uint8_t deflen;   // デフォルト音符長
    uint16_t tempo;   // テンポ
    uint16_t tone;    // 現在のトーン値 (TONEテーブル値)
    uint16_t len;     // 残り音符長 (60FPSティック)
    uint16_t waitcnt; // ウェイトカウンタ
    uint8_t volume;   // V0-V15 音量 (多声モードでPSG ch音量として反映)
};
static struct mml_state mml_ch[3];  // chA=0, chB=1, chC=2

static void mml_init_state(struct mml_state* st) {
    st->mml = NULL;
    st->rep = NULL;
    st->oct = PSG_DEFAULT_OCT;
    st->deflen = PSG_DEFAULT_LEN;
    st->tempo = PSG_DEFAULT_TEMPO;
    st->tone = 0;
    st->len = 0;
    st->waitcnt = 0;
    st->volume = PSG_DEFAULT_VOLUME;
}

//===== TONEテーブル =====
#define TONE_LEN (12 * 6) // O1C - O6B
const uint8 TONE[] = {
	240, 227, 214, 202, 191, 180, 170, 160, 151, 143, 135, 127, // O1
	120, 113, 107, 101, 95, 90, 85, 80, 76, 71, 67, 64,       // O2
	60, 57, 53, 50, 48, 45, 43, 40, 38, 36, 34, 32,            // O3
	30, 28, 27, 25, 24, 23, 21, 20, 19, 18, 17, 16,            // O4
	15, 14, 13, 13, 12, 11, 11, 10, 9, 9, 8, 8,                // O5
	8, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5,                        // O6
};

//===== ヘルパー関数 =====
static uint32 psg_parseInt(char** s) {
	if (!**s)
		return 0;
	while (**s == ' ')
		(*s)++;
	uint32 a = 0;
	while (**s >= '0' && **s <= '9') {
		a = a * 10 + (**s - '0');
		(*s)++;
	}
	return a;
}

S_INLINE uint8 psg_toupper(uint8 c) {
	if (c >= 'a' && c <= 'z') {
		return c & 0b1011111;
	}
	return c;
}

//===== 1ch分のMML処理 (psg_tickから呼ばれる) =====
// 単声モード: ch=0のトーン → _g.psgtone → set_tone() → PWM
// 多声モード: 各chの周波数・音量 → PSGレジスタ (psg_emu) → 16kHz混合出力
static void psg_process_voice(int ch) {
    struct mml_state* st = &mml_ch[ch];

    // 音符長カウントダウン
    if (st->len) {
        if (--st->len) {
            return; // まだ音符が鳴っている
        }
        // 音符終了
        if (!st->mml) {
            // MMLなし → 音を止める
            st->tone = 0;
            if (sound_multi_mode) {
                psg_emu_set_volume(ch, 0); // このchを消音 (多声モード中のBEEP終了含む)
            }
            if (ch == 0) {
                _g.psgtone = 0;
                sound_switch(0); // PWM消音 (多声モード中はsound_switch内で無視)
            }
        }
    }

    if (!st->mml) {
        return;
    }

    // MMLパーサー: 次の音符を解析
    int flg = 0;
    for (;;) {
        int t = -2;
        int32 s = 0;
        int c = psg_toupper(*st->mml++);
        if (c == '<') { // オクターブ上
            st->oct++;
            continue;
        } else if (c == '>') { // オクターブ下
            st->oct--;
            continue;
        } else if (c == 'O') {
            char c2 = *st->mml++;
            if (!c2) {
                st->mml--;
                continue;
            }
            st->oct = c2 - '0';
            continue;
        } else if (c == ' ') {
            continue;
        } else if (c == 'L') {
            uint32 l = psg_parseInt(&st->mml);
            if (l) {
                st->deflen = 32 / l;
                int c3 = *st->mml++;
                if (c3 == '.') {
                    st->deflen += st->deflen >> 1;
                } else
                    st->mml--;
            }
            continue;
        } else if (c == 'T') {
            st->tempo = psg_parseInt(&st->mml);
            continue;
        } else if (c == 'V') { // V0-V15 音量コマンド (多声モードでPSG ch音量として反映)
            uint32 v = psg_parseInt(&st->mml);
            if (v > 15) v = 15;
            st->volume = v;
            continue;
        } else if (c == 'N') {
            s = psg_parseInt(&st->mml);
            if (s > 255)
                s = 255;
        } else if (c == 'C') {
            t = 0;
        } else if (c == 'D') {
            t = 2;
        } else if (c == 'E') {
            t = 4;
        } else if (c == 'F') {
            t = 5;
        } else if (c == 'G') {
            t = 7;
        } else if (c == 'A') {
            t = 9;
        } else if (c == 'B') {
            t = 11;
        } else if (c == 'R') {
            t = -2;
        } else if (c == '$') {
            if (*st->mml)
                st->rep = st->mml;
            continue;
        } else { // その他の記号で停止、$があればリピート
            if (st->rep && !flg) {
                st->mml = st->rep;
                flg = 1;
                continue;
            }
            // MML終了 → 音を止める
            st->mml = NULL;
            st->tone = 0;
            st->len = 0;
            if (sound_multi_mode) {
                psg_emu_set_volume(ch, 0); // このchを消音
            }
            if (ch == 0) {
                _g.psgmml = NULL;
                _g.psgtone = 0;
                sound_switch(0);
            }
            return;
        }

        // +/-/# 修正
        int c2 = *st->mml++;
        if (c2 == '+' || c2 == '#') {
            if (t != 11)
                t++;
        } else if (c2 == '-') {
            if (t)
                t--;
        } else
            st->mml--;

        // 音符長
        uint32 len = psg_parseInt(&st->mml);
        if (len) {
            len = 32 / len;
        } else {
            len = st->deflen;
        }
        int c4 = *st->mml++;
        if (c4 == '.') {
            len += len >> 1;
        } else
            st->mml--;

        // オクターブ計算
        if (t > -2)
            t += ((st->oct - 1) * 12);

        // TONEテーブル参照
        if (!s) {
            if (t >= 0 && t < TONE_LEN) {
                if (_g.psgratio == 1) {
                    if (t < TONE_LEN - 12)
                        s = TONE[t];
                    else
                        s = TONE[t - 12] / 2;
                } else {
                    t -= 24;
                    if (t >= 0)
                        s = TONE[t];
                    else if (t >= -12)
                        s = TONE[t + 12] * 2;
                    else
                        s = TONE[t + 24] * 4;
                }
            }
        }

        // トーン設定 (多声モード: PSGレジスタへ / 単声モード: set_tone()がPWM周波数を制御)
        st->tone = s;
        st->waitcnt = s - 1;
        st->len = calcDivU(len * ((60 * PSG_TICK_FREQ) >> 3) * _g.psgratio, st->tempo);

        if (sound_multi_mode) {
            // 多声PSGエミュモード: このchの周波数・音量をPSGレジスタへ
            if (s) {
                psg_emu_set_period(ch, psg_emu_tone_to_period(s));
                psg_emu_set_volume(ch, st->volume);
                psg_emu_set_tone_enable(ch, true); // トーン有効 (ノイズビットは維持)
            } else {
                psg_emu_set_volume(ch, 0); // 休符
            }
        }
        if (ch == 0) {
            _g.psgtone = s;
            _g.psglen = st->len;
        }
        break;
    }
}

//===== メインTICK (60FPSタイマーから呼ばれる) =====
S_INLINE void psg_tick() {
    for (int ch = 0; ch < 3; ch++) {
        psg_process_voice(ch);
    }
    // 多声PLAY終了検出: 全声のMML終了+BEEP残余なし → PWMトーンモードへ自動戻り
    if (sound_multi_play && !mml_ch[0].mml && !mml_ch[1].mml && !mml_ch[2].mml && !mml_ch[0].len) {
        sound_set_multi_mode(false, false);
    }
}

//===== 多声PLAY開始 =====
void psg_playMML_multi(char* mml0, char* mml1, char* mml2) {
    // 2声以上指定 → 多声PSGエミュモード / 単声・停止 → PWMトーンモード (クリーンな音)
    if (mml1 != NULL || mml2 != NULL) {
        psg_emu_init(); // 前回の多声/SOUNDコマンドの状態をリセット
        sound_set_multi_mode(true, true);
    } else {
        sound_set_multi_mode(false, false);
    }
    char* mmls[3] = { mml0, mml1, mml2 };
    for (int ch = 0; ch < 3; ch++) {
        struct mml_state* st = &mml_ch[ch];
        if (mmls[ch]) {
            mml_init_state(st);
            st->mml = mmls[ch];
        } else if (st->mml) {
            st->mml = NULL;
            st->tone = 0;
            st->len = 0;
            if (ch == 0) {
                sound_switch(0);
            }
        }
    }
    // 後方互換 (voice 0)
    _g.psgmml = mml_ch[0].mml;
    _g.psgtone = mml_ch[0].tone;
    _g.psglen = mml_ch[0].len;
    _g.psgoct = mml_ch[0].oct;
    _g.psgdeflen = mml_ch[0].deflen;
    _g.psgrep = mml_ch[0].rep;
    sound_switch(mml_ch[0].mml != NULL);
}

//===== 単声PLAY (従来互換) =====
S_INLINE void psg_playMML(char* mml) {
    psg_playMML_multi(mml, NULL, NULL);
}

//===== BEEP =====
S_INLINE void psg_beep(int tone, int len) {
    struct mml_state* st = &mml_ch[0];
    st->len = len * (PSG_TICK_FREQ / PSG_TICK_PER_SEC) * _g.psgratio;
    st->tone = tone * _g.psgratio;
    st->waitcnt = st->tone - 1;
    st->mml = NULL;

    if (sound_multi_mode) {
        // 多声モード中はPSG chAでBEEP (chB/CのBGMは維持)
        if (st->tone > 0) {
            psg_emu_set_period(0, psg_emu_tone_to_period(st->tone));
            psg_emu_set_volume(0, PSG_DEFAULT_VOLUME);
            psg_emu_set_tone_enable(0, true);
        } else {
            psg_emu_set_volume(0, 0);
        }
    }

    // 後方互換 (PWMモード: set_tone()が周波数設定 / 多声モード中はsound_switch無視)
    _g.psgtone = st->tone;
    _g.psglen = st->len;
    _g.psgmml = NULL;
    sound_switch(tone);
}

//===== TEMPO (全声に適用) =====
S_INLINE void psg_tempo(int tempo) {
    _g.psgtempo = tempo;
    for (int ch = 0; ch < 3; ch++) {
        mml_ch[ch].tempo = tempo;
    }
}

//===== サウンド状態 =====
S_INLINE int psg_sound() {
    return _g.psgtone || _g.psgmml;
}

// __PSG_H__
#endif
