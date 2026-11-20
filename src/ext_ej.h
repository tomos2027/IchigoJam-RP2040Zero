//NEOJAM: EJ「OLED」コマンドエンジン
//Milkjam (src/ext_iotpad.h) のEJ OLED部をNEOJAMへ移植したもの
//BASICコマンド EJ "CMD"[,"ARG"] / 関数 EJ("CMD") として使用する
//対応サブコマンド: OLEDON OLEDOFF OLEDCLEAR OLEDCUR OLEDFONT(OLEDSIZE)
//                  GETFONT OLEDPRINT(OLED) OLEDPAGE(NEXTSCR)
//未対応 (MilkjamのWiFi/センサー系) は Unknown: 応答 (将来拡張の足場)
//※command_ej/func_ejともstaticバッファ使用 (スタック使用量0、Milkjam準拠)
#ifndef __EXT_EJ_H__
#define __EXT_EJ_H__
#include "all_includes.h"

#define EJ_CMD_MAX 48
#define EJ_ARG_MAX 128
#define EJ_RESP_MAX 96

//軽量文字列構築 (stdio書式機構はフラッシュが約+50KB増えるため自前実装)
//※応答はすべて短い固定形式のためバッファ (96バイト) 超過は起きない
static void ej_set(char* resp, int resp_len, const char* s) {
    int i = 0;
    while (s[i] && i < resp_len - 1) { resp[i] = s[i]; i++; }
    resp[i] = 0;
}
static char* ej_cat_str(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}
static char* ej_cat_num(char* p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char tmp[6];
    int n = 0;
    do { tmp[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}
//"123"/"-123"等の10進数を読む (読んだ文字数だけ*ppを進める。数字がなければ0を返す)
static int ej_atoi(const char** pp) {
    const char* p = *pp;
    int v = 0, neg = 0;
    if (*p == '-') { neg = 1; p++; }
    const char* start = p;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return (p == start && !neg) ? 0 : (neg ? -v : v);
}

//トークン列から文字列リテラルを取得 (コピーしてNUL終端)
//※token_skipstr()の返り値は'"'までの生ポインタのためコピーが必要
//toupper_mode=true: 大文字化 (コマンド名 — 小文字で打っても一致させる)
//toupper_mode=false: そのまま (OLEDPRINTの表示文字列等)
static void token_getstr_copy(char* buf, int len, bool toupper_mode) {
    char* s = token_skipstr();
    int i = 0;
    while (s[i] && s[i] != '"' && i < len - 1) {
        buf[i] = toupper_mode ? basic_toupper(s[i]) : s[i];
        i++;
    }
    buf[i] = 0;
}

//EJの引数取得: 引用符付き文字列、または引用符なしの英数字列を生テキストから読む
//(引用符忘れ対策: EJ "OLEDPRINT",Hello / EJ OLEDCUR,10,20 / EJ OLEDFONT,2 等。
// padのシリアルコマンド形式と同様に引用符を必須にしない、Milkjam準拠)
//※token_back()はpcを引数先頭(lasttoken)へ戻すため、トークン化結果に関係なく元テキストを読み直せる
//accept_comma: true=値引数 (EJ OLEDCUR,10,20 の「10,20」等、カンマを含めて読む)
//               false=コマンド名 (カンマ=引数境界なので読み取り停止)
static void token_get_ej_arg_raw(char* buf, int len, bool toupper_mode, bool accept_comma) {
    int i = 0;
    while (i < len - 1) {
        char c = pc[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || c == '-' || c == '_' || c == '.' || c == '@' || (accept_comma && c == ',')) {
            buf[i] = toupper_mode ? basic_toupper(c) : c;
            i++;
        } else {
            break;
        }
    }
    while (i > 0 && buf[i - 1] == ',') i--;//末尾カンマは引数に含めない
    buf[i] = 0;
    pc += i;
}
static void token_get_ej_arg(char* buf, int len, bool toupper_mode, bool accept_comma) {
    Token t; Token_get(t);
    if (t.code == TOKEN_STRING) {
        token_getstr_copy(buf, len, toupper_mode);
        return;
    }
    token_back();//引数の先頭まで戻す (pc = lasttoken)
    token_get_ej_arg_raw(buf, len, toupper_mode, accept_comma);
}

//EJコマンド実行の共通エンジン (OLEDサブセット)
//cmd: 大文字化済みサブコマンド ("OLEDPRINT"等), arg: 第2引数 (無指定="")
//resp: 応答文字列 (呼び出し側で表示), num: 数値結果 (関数形式EJ()の戻り値), num_valid: 数値結果の有無
static void ej_oled_exec(const char* cmd, const char* arg, char* resp, int resp_len, int16* num, bool* num_valid) {
    *num_valid = false;
    *num = 0;
    #define EJ_RESP(s) do { ej_set(resp, resp_len, (s)); } while (0)
    //===== OLED制御 (Milkjam互換) =====
    if (strcmp(cmd, "OLEDON") == 0) {
        if (oled_ok) {
            oled_direct = true;
            oled_cursor_x = 0;
            oled_cursor_y = 0;
            oled_buf_clear();
            oled_buf_send();
            EJ_RESP("OLED Direct ON");
        } else { EJ_RESP("OLED Not Found"); }
    } else if (strcmp(cmd, "OLEDOFF") == 0) {
        oled_direct = false;
        oled_update_display();
        EJ_RESP("OLED Direct OFF");
    } else if (strcmp(cmd, "OLEDCLEAR") == 0) {
        if (oled_ok) {
            oled_buf_clear();
            oled_buf_send();
            oled_cursor_x = oled_cursor_y = 0;
            EJ_RESP("OLED Cleared");
        } else { EJ_RESP("OLED Not Found"); }
    } else if (strcmp(cmd, "OLEDCUR") == 0) {
        if (oled_ok) {
            //"x,y"形式を軽量パース (Milkjamはsscanf使用)
            const char* p = arg;
            int x = ej_atoi(&p);
            if (p != arg && *p == ',') {
                p++;
                const char* ystart = p;
                int y = ej_atoi(&p);
                if (p != ystart) {
                    oled_cursor_x = x;
                    oled_cursor_y = y;
                    char* q = ej_cat_str(resp, "CUR:");
                    q = ej_cat_num(q, x);
                    *q++ = ',';
                    q = ej_cat_num(q, y);
                    *q = 0;
                } else {
                    EJ_RESP("Usage: EJ OLEDCUR x,y");
                }
            } else {
                EJ_RESP("Usage: EJ OLEDCUR x,y");
            }
        } else { EJ_RESP("OLED Not Found"); }
    } else if (strcmp(cmd, "OLEDFONT") == 0 || strcmp(cmd, "OLEDSIZE") == 0) {
        if (*arg == 0) {
            char* q = ej_cat_str(resp, "Font:");
            q = ej_cat_num(q, oled_font_index);
            q = ej_cat_str(q, "=");
            ej_cat_str(q, oled_font_names[oled_font_index]);
        } else {
            const char* p = arg;
            int idx = ej_atoi(&p);
            if (idx >= 0 && idx < OLED_FONT_COUNT && *p == 0) {
                oled_font_index = idx;
                oled_save_font_flash(idx);//フォント番号をflash設定領域へ保存 (Milkjam準拠)
                oled_update_display();//即座に再描画
                char* q = ej_cat_str(resp, "Font:");
                q = ej_cat_num(q, idx);
                q = ej_cat_str(q, "=");
                q = ej_cat_str(q, oled_font_names[idx]);
                ej_cat_str(q, " (saved)");
            } else {
                EJ_RESP("Usage: EJ OLEDFONT 0-4");
            }
        }
    } else if (strcmp(cmd, "GETFONT") == 0) {
        char* q = ej_cat_str(resp, "Font:");
        q = ej_cat_num(q, oled_font_index);
        q = ej_cat_str(q, "=");
        ej_cat_str(q, oled_font_names[oled_font_index]);
    } else if (strcmp(cmd, "OLEDPRINT") == 0 || strcmp(cmd, "OLED") == 0) {
        if (oled_ok && oled_direct) {
            //実測の描画幅でカーソルを進める (Milkjam/pad準拠)
            oled_cursor_x = oled_draw_str(oled_cursor_x, oled_cursor_y, arg);
            oled_buf_send();
            EJ_RESP("OK");
        } else { EJ_RESP("OLED Direct OFF"); }
    } else if (strcmp(cmd, "OLEDPAGE") == 0 || strcmp(cmd, "NEXTSCR") == 0) {
        if (oled_ok) {
            if (*arg == 0) {
                //次の画面へ (1→2→3→1)
                oled_current_screen = (oled_current_screen % OLED_SCREEN_COUNT) + 1;
            } else {
                const char* p = arg;
                int pg = ej_atoi(&p);
                if (pg >= 1 && pg <= OLED_SCREEN_COUNT && *p == 0) oled_current_screen = pg;
            }
            char* q = ej_cat_str(resp, "Page:");
            ej_cat_num(q, oled_current_screen);
            oled_update_display();
            *num = oled_current_screen;
            *num_valid = true;
        } else { EJ_RESP("OLED Not Found"); }
    } else {
        char* q = ej_cat_str(resp, "Unknown:");
        ej_cat_str(q, cmd);
    }
    #undef EJ_RESP
}

//EJ "CMD"[,"ARG"] — コマンド形式 (応答を画面に表示)
S_INLINE void command_ej() {
    //スタックオーバーフロー防止: バッファはstatic化 (Milkjam準拠)
    static char cmd[EJ_CMD_MAX];
    //コマンド名: 引用符付き/引用符なし両対応 (引用符なしは大文字化して読む。カンマで停止)
    token_get_ej_arg(cmd, sizeof cmd, true, false);
    if (!cmd[0]) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    static char arg[EJ_ARG_MAX];
    arg[0] = 0;
    Token t; Token_get(t);
    if (t.code == TOKEN_COMMA) {
        token_get_ej_arg(arg, sizeof arg, false, true);//値引数: 小文字を保持・カンマ可
    } else {
        token_back();
    }
    token_end();
    IJB_ERR_CHK();
    static char resp[EJ_RESP_MAX];
    int16 num;
    bool num_valid;
    ej_oled_exec(cmd, arg, resp, sizeof resp, &num, &num_valid);
    put_str(resp);
    put_chr('\n');
}

//EJ("CMD") — 関数形式 (数値結果を返す。数値を返さないコマンドはエラー)
S_INLINE int func_ej() {
    static char cmd[EJ_CMD_MAX];
    token_get_ej_arg(cmd, sizeof cmd, true, false);//引用符付き/引用符なし両対応 (カンマで停止)
    if (!cmd[0]) {
        command_error(ERR_SYNTAX_ERROR);
        return 0;
    }
    Token t; Token_get(t);
    if (t.code != TOKEN_PAREN_E) {
        command_error(ERR_SYNTAX_ERROR);
        return 0;
    }
    static char resp[EJ_RESP_MAX];//スタック使用量削減のためstatic化
    int16 num;
    bool num_valid;
    ej_oled_exec(cmd, "", resp, sizeof resp, &num, &num_valid);
    if (!num_valid) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return 0;
    }
    return num;
}

#endif
