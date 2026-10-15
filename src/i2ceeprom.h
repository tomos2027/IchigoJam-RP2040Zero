//https://www.mouser.jp/datasheet/2/268/24AA1025_24LC1025_24FC1025_1024_Kbit_I2C_Serial_EE-2853738.pdf 参照
/*client addressは最下位ビットを0ビット目とすると、
0ビット目はread/writeの指定(readなら1,writeなら0)　pico-sdkのread/writeで指定する
1~2ビット目は他のEEPROMと識別するときに使う(ここでは00でよい)
3ビット目はEEPROMのアドレスの指定に使う
4~7ビット目は1010で固定*/
//24FC1025-I/Pの容量は1Mbit=128KB=2^17Bなので、A16ビットをclient address側に出す必要がある
//24LC64/24LC256ではA16は不要だが、下位アドレス領域では0b1010000のままアクセスできる
//このため、client addressは毎回memory_addressから再計算する (4K版、GivetakeJam_P準拠)

#ifndef __I2CEEPROM_H__
#define __I2CEEPROM_H__
#include "all_includes.h"

//24LC64(32B/ページ)/24LC256(64B/ページ)/24FC1025(128B/ページ)の3デバイスに
//単一バイナリで対応するため、最小の安全なページサイズ(32バイト)で書き、
//ページ境界をまたがないようにする (4K版、GivetakeJam_P準拠)
#define EEPROM_SAFE_PAGE_SIZE 32

static uint8_t calc_client_address(int memory_address) {
    uint8_t res = 0b1010000;
    if ((memory_address >> 16) & 1) { //24FC1025のA16 (小容量EEPROMでは実質未使用)
        res |= 0b100;
    }
    return res;
}

static void make_write_src(int memory_address, uint8_t* list, int len, uint8_t* src) {
    src[0] = (uint8_t)(memory_address >> 8);
    src[1] = (uint8_t)memory_address;
    for (int i = 0; i < len; i++) {
        src[2 + i] = list[i];
    }
}

//32バイトずつ小分けに読み、チャンクごとにclient addressを再計算する
//(読み出し自体はページ制限を受けないが、24FC1025の64KB境界越えを安全にする)
// ret:size if:-1 err
int read_eeprom(int memory_address, uint8_t* list, int len) {
    int total = 0;

    while (len > 0) {
        int tlen = len > EEPROM_SAFE_PAGE_SIZE ? EEPROM_SAFE_PAGE_SIZE : len;

        uint8_t client_address = calc_client_address(memory_address);

        uint8_t src[2];
        src[0] = (uint8_t)(memory_address >> 8);
        src[1] = (uint8_t)memory_address;

        int res = i2c_write_timeout_us(i2c_default, client_address, src, 2, true, TIMEOUT_US);
        if (res < 0) {
            return -1;
        }

        res = i2c_read_timeout_us(i2c_default, client_address, list, tlen, false, TIMEOUT_US);
        if (res != tlen) {
            return -1;
        }

        memory_address += tlen;
        list += tlen;
        len -= tlen;
        total += tlen;
    }

    return total;
}

// err:1 no_err:0
//1回の書き込みコマンドでページ境界をまたがない (24LC64/24LC256のページラップ破壊を防止)
int write_eeprom(int memory_address, uint8_t* list, int len) {
    uint8_t src[EEPROM_SAFE_PAGE_SIZE + 2];

    while (len > 0) {
        //24FC1025の64KBバンク越えに備え、チャンクごとにclient addressを再計算する
        uint8_t client_address = calc_client_address(memory_address);

        //ページ境界をまたがないように書き込み長を制限する
        int page_remain = EEPROM_SAFE_PAGE_SIZE - (memory_address % EEPROM_SAFE_PAGE_SIZE);
        int tlen = len > page_remain ? page_remain : len;

        make_write_src(memory_address, list, tlen, src);

        int res = i2c_write_timeout_us(i2c_default, client_address, src, tlen + 2, false, TIMEOUT_US);

        //EEPROM内部の書き込みサイクル待ち (5msはこれらのデバイスで安全な値)
        sleep_us(5000);

        if (res < 0) {
            return 1;
        }

        memory_address += tlen;
        list += tlen;
        len -= tlen;
    }

    return 0;
}

//I2Cアドレス0x50に外部EEPROMが存在するか (あり:1 なし:0)
//24LC64/24LC256/24FC1025はいずれも0x50で応答する
//書き込み直後はbusyの可能性があるため、数回リトライする
S_INLINE int i2c_eeprom_exists() {
    uint8_t dummy;
    uint8_t addr = 0x50;

    //I2C初期化
    i2c0_init();

    for (int i = 0; i < 3; i++) {
        int res = i2c_read_timeout_us(i2c_default, addr, &dummy, 1, false, TIMEOUT_US);
        if (res == 1) {
            return 1;
        }
        sleep_us(2000);
    }
    return 0;
}

#endif
