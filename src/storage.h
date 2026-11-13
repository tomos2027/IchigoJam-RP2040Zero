//picoに焼いたプログラムはフラッシュメモリの先頭に配置される
//プログラム領域の終わりは、build/IchigoJam_P.elf.mapのflash_binary_endの下7桁を見ればわかるが、かなり余裕がある(2ブロックに収まっている)
//IchigoJamのプログラムはフラッシュメモリの後ろの方に保存する
//フラッシュメモリの容量=2M=0x200000, 1ブロックの容量=0x10000, FLASH_SECTOR_SIZE=0x1000(単位はバイト)
#ifndef __STORAGE_H__
#define __STORAGE_H__
#include "all_includes.h"
#include "hardware/uart.h"

#define FLASH_IJ_OFFSET (0x200000 - FLASH_SECTOR_SIZE * 2)//SAVE0,LOAD0で操作するセクタの先頭(一番最後のセクタはKBDの設定値などを保存するのに使うので、その1つ前のセクタ)
// ウォッチドッグ定数 (system.hは後でincludeされるためここでも定義する)
#ifndef WATCHDOG_TIMEOUT_MS
#define WATCHDOG_TIMEOUT_MS 8000
#define WATCHDOG_SAVE_TIMEOUT_MS 15000
#endif
void led_builtin(bool on);//実体はext_ws.h、include順の関係で前方宣言
// core1パーク/レジューム/フラッシュ委譲 (実体はsound.h、include順の関係で前方宣言)
void psg_core1_park(void);
void psg_core1_resume(void);
static void __not_in_flash_func(psg_core1_flash_op_and_wait)(uint32_t offset, const uint8_t *data);
// 診断: シリアルログ (uart0=GP0, 115200, ポーリング送信+CRLF)。
// 割込み禁止中も動作する (UARTはハードウェアで自律送出されるため)。
// フラッシュ書込中 (XIP停止) は呼べないが、呼び出し箇所はすべてXIP生存領域
// のため文字列リテラルも安全。CPUが固まっても送信済み/FO内の行は端末に届く
static void diag_log(const char *s) {
    while (*s) {
        uart_putc_raw(uart0, *s++);
    }
    uart_putc_raw(uart0, '\r');
    uart_putc_raw(uart0, '\n');
}
// 診断: ラベル+16進数値を1行で出力 (park失敗時のステータスダンプ用)
static void diag_log_hex(const char *label, uint32_t v) {
    char buf[40];
    int i = 0;
    while (*label && i < 24) buf[i++] = *label++;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int b = 28; b >= 0; b -= 4) {
        uint8_t nib = (uint8_t)((v >> b) & 0xFu);
        buf[i++] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    buf[i++] = '\r'; buf[i++] = '\n'; buf[i] = 0;
    const char *p = buf;
    while (*p) uart_putc_raw(uart0, *p++);
}



#define EEPROM_OFFSET 100
//4K版: 外部EEPROMのスロット数 (1スロット=4096バイト、32スロット=128KB、表示/保存範囲は100..131固定)
#define EEPROM_SIZE 32
#define SIZELIMIT (IJB_SIZEOF_LIST - 2)//=1プログラムのサイズ (4K版: 4096)

// core1 フラッシュ書込み用の 4 byte アラインメントバッファ (ROM の program 関数が要求)
static uint8_t flash_sector_buffer[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

INLINE int IJB_file() {
    return _g.lastfile;
}

uint32_t calc_flash_offset(int n) {
    return FLASH_IJ_OFFSET - n * FLASH_SECTOR_SIZE;
}

uint32_t get_config_offset() {
    return calc_flash_offset(-1);//一番最後のセクタ
}

uint8_t* get_flash(uint32_t offset) {
    return (uint8_t*)(XIP_BASE + offset);
}

// err:1 no_err:0
static int IJB_save(int n, uint8* list, int size) {
    _g.lastfile = n;
    int res;
    if (0 <= n && n < N_FLASH_STORAGE) {
        uint32_t offset = calc_flash_offset(n);
        // core1にフラッシュ消去/書込みを委譲し、core0は映像DMA割込みを継続させる。
        // ROMの program 関数はデータバッファが 4 byte アラインメントされていることを要求するため、
        // スタック上の list をアラインバッファへコピーして渡す。
        memcpy(flash_sector_buffer, list, FLASH_SECTOR_SIZE);
        psg_core1_flash_op_and_wait(offset, flash_sector_buffer); // RAM配置: フラッシュビジー中もXIP不要
        res = 0;
    } else if (EEPROM_OFFSET <= n && n < EEPROM_OFFSET + EEPROM_SIZE) {
        res = write_eeprom((n - EEPROM_OFFSET) * SIZELIMIT, list, SIZELIMIT);
    } else {
        res = 1;
    }
    return res;
}

// ret:size if:-1 err
static int IJB_load(int n, uint8* list, int sizelimit, int init) {
    if (init) {
        _g.lastfile = n;
    }
    if (0 <= n && n < N_FLASH_STORAGE) {
        const uint8_t* flash = get_flash(calc_flash_offset(n));
        //TODO IchigoJam以外で使ったことのあるpicoでは正しく判定できないので、別の方法を考える
        if (*(int16*)flash == -1) {//未使用領域の初期値
            *(uint16*)list = 0;
            return -1;
        }
        memcpy(list, flash, sizelimit);
        return sizelimit;
    } else if (EEPROM_OFFSET <= n && n < EEPROM_OFFSET + EEPROM_SIZE) {
        return read_eeprom((n - EEPROM_OFFSET) * SIZELIMIT, list, sizelimit);
    } else {
        return -1;
    }
}

#endif