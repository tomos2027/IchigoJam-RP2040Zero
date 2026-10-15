/*
 * Givetake BASIC / RP2040-Zero：SDカード拡張（SPI1、FAT32）
 * 元実装: Canon-boy4 氏 GivetakeJam_P (ichigojam-firm rp2350-16130 ブランチ) MIT license
 * https://github.com/Canon-boy4/ichigojam-firm/tree/rp2350-16130
 *
 * Waveshare RP2040-Zero向け改変:
 *  - 基板にTFスロットがないため、側面エッジパッドのGP12-15へSDカードを外部配線する
 *    MISO=GP12 (カードDAT0)、CS=GP13 (カードDAT3)、SCK=GP14 (カードCLK)、MOSI=GP15 (カードCMD)
 *    IchigoJamのIN/OUTポートと完全に無衝突のため、SDON中もI/Oは常時利用可能
 *  - カード検出ピンは省略。未挿入はCMD0の失敗(エラー2)で検知する
 *  - ピン共有保護(sd_pin_reserved)機構は不要のため削除 (元実装の機構)
 *  - 400kHzで初期化し1MHzで転送する。読み取りに失敗したら100kHzへ自動フォールバック (元実装の機構を流用)
 *
 * 元実装の仕様 (維持):
 * ポーリング方式。DMAは使用せず、SD処理全体で割り込みを禁止しない (NTSC表示は維持される)。
 * 対応：512バイト/セクター、FAT32のMBR基本パーティションまたは直接配置。
 * 保存番号200～299はルートのP200.IJB～P299.IJB (バイナリ) またはP200.BAS～P299.BAS (アスキー、UTF-8)。
 * 1ファイル4096バイト。データ形式はBASIC内部形式 (.IJB) またはテキスト (.BAS、SAVE n,1)。
 * 読み取りCRC検査、書き込み後のCRC検査と全バイト照合を行う。
 * SAVEは新規クラスタへ書き、照合後にディレクトリを変更し、旧クラスタを解放する。
 * 電源断に対する完全な原子性はない。途中失敗で孤立クラスタ/FAT不一致が残り得る。
 * FAT12/FAT16、exFAT、GPT、拡張パーティション、サブフォルダーは未対応。
 * ルート探索上限は256セクター/32クラスタ。保存時のルート拡張は行わない。
 * RTC未使用のため、新規作成日/更新日は1980-01-01、時刻00:00として記録する。
 */
#ifndef GIVETAKE_SD_CARD_H
#define GIVETAKE_SD_CARD_H
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// SDカード (SPI1、外部配線: 側面エッジパッドGP12-15)
#define SD_MISO_PIN 12    // カードDAT0/MISO
#define SD_SELECT_PIN 13   // カードDAT3/CS
#define SD_CLK_PIN 14      // カードSCK/CLK
#define SD_MOSI_PIN 15     // カードCMD/MOSI
// クロック (内蔵スロットは配線が短くWaveshare公式デモも12.5MHz以上で実績がある)
#define SD_INIT_HZ 400000     // 初期化時 (SD仕様の上限は400kHz)
#define SD_HZ 1000000         // 通常転送
#define SD_FALLBACK_HZ 100000  // 読み取りエラー時のフォールバック先

static bool sd_pins_reserved = false;
static bool sd_initialized = false;
static bool sd_fat_ready = false;
static bool sd_files_index_ready = false;
static bool sd_block_addressed = false;
static unsigned sd_last_error = 0;
static uint32_t sd_read_hz = SD_HZ;
static unsigned sd_read_error = 0;
static uint8_t sd_read_r1 = 0xff, sd_read_token = 0xff;
static uint16_t sd_read_crc_wire = 0, sd_read_crc_calc = 0;
static uint32_t sd_read_lba = 0;
static uint8_t sd_sector[512];
typedef struct {
    uint32_t start, total, fat_lba, fat_size, data_lba, clusters, root_cluster, root_lba;
    uint8_t sectors_per_cluster, fats, active_fat, media;
    bool partitioned, mirrored;
    uint32_t fsinfo_lba, fsinfo_backup_lba;
} SdFatInfo;
static SdFatInfo sd_fat;
static unsigned sd_fs_error = 0;
static unsigned sd_fs_kind = 0;

static uint8_t sd_debug_ready[10], sd_debug_response[10];
static bool sd_debug_sent[10];
static unsigned sd_debug_count = 0;
static uint8_t sd_debug_last_ready = 0xff;
static bool sd_debug_last_sent = false;
// SPI1で1バイトを送受信する。SDの応答取得時は0xFFを送る。
static inline uint8_t sd_byte(uint8_t value) {
    uint8_t result; spi_write_read_blocking(spi1, &value, &result, 1); return result;
}
// CS (GP13) を制御する。選択LOW、非選択HIGH。
static void sd_select(bool selected) {
    gpio_put(SD_SELECT_PIN, !selected); sleep_us(10);
}
// CSを非選択にし、追加の8クロックでカード側の出力を解放する。
static void sd_release(void) { sd_select(false); sd_byte(0xff); }
// カードがMISOへ0xFFを返すまで待つ。時間上限に達したら失敗。
static bool sd_wait_ready(uint32_t ms) {
    uint64_t deadline = time_us_64() + (uint64_t)ms * 1000;
    do { sd_debug_last_ready = sd_byte(0xff); if (sd_debug_last_ready == 0xff) return true; } while (time_us_64() < deadline);
    return false;
}
// コマンドと引数を送る。CMD0/CMD8には有効CRCを付け、R1応答を待つ。
static uint8_t sd_command(uint8_t cmd, uint32_t arg) {
    sd_debug_last_sent = false;
    sd_release(); sd_select(true);
    if (!sd_wait_ready(500)) return 0xff;
    sd_debug_last_sent = true;
    sd_byte(0x40 | cmd);
    sd_byte(arg >> 24); sd_byte(arg >> 16); sd_byte(arg >> 8); sd_byte(arg);
    sd_byte(cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01);
    for (unsigned n = 0; n < 10; n++) { uint8_t r = sd_byte(0xff); if (!(r & 0x80)) return r; }
    return 0xff;
}
// カード検出: 本基板のTFスロットは検出ピンが未配線のため常に挿入されているものとして扱う。
// 未挿入はCMD0の失敗 (エラー2) で検知する。
static bool sd_card_present(void) {
    return true;
}
// SDを解放。CSをHIGH(プルアップ入力)に戻し、SPIピンを入力にする。診断値は保持する。
static void sd_card_off(void) {
    if (sd_pins_reserved) { sd_release(); spi_deinit(spi1); }
    gpio_put(SD_SELECT_PIN, 1);
    gpio_init(SD_SELECT_PIN); gpio_pull_up(SD_SELECT_PIN);
    gpio_init(SD_MISO_PIN); gpio_pull_up(SD_MISO_PIN);
    gpio_init(SD_CLK_PIN);
    gpio_init(SD_MOSI_PIN);
    sd_initialized = false; sd_fat_ready = false; sd_files_index_ready = false; sd_pins_reserved = false; sd_block_addressed = false;
}

// SD_INIT_HZで初期化し、成功したらSD_HZへ上げる。エラー：1未挿入、2 CMD0、3 CMD8、4 ACMD41、5 OCR、6 CMD16。
static int sd_card_on(void) {
    sd_card_off(); sd_last_error = 0; sd_debug_count = 0; sd_read_error = 0; sd_fs_error = 0; sd_fs_kind = 0; sd_read_hz = SD_HZ;
    if (!sd_card_present()) return sd_last_error = 1;
    sd_pins_reserved = true;
    spi_init(spi1, SD_INIT_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI); gpio_pull_up(SD_MISO_PIN);
    gpio_set_function(SD_CLK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    // 出力ラッチをHIGHにしてから出力化し、切り替え時のLOWパルスを防ぐ。
    gpio_init(SD_SELECT_PIN); gpio_pull_up(SD_SELECT_PIN);
    gpio_put(SD_SELECT_PIN, 1); gpio_set_dir(SD_SELECT_PIN, GPIO_OUT);
    sleep_ms(10); for (unsigned i = 0; i < 10; i++) sd_byte(0xff);
    uint8_t r = 0xff;
    for (unsigned i = 0; i < 10; i++) {
        r = sd_command(0, 0);
        sd_debug_ready[i] = sd_debug_last_ready;
        sd_debug_sent[i] = sd_debug_last_sent;
        sd_debug_response[i] = r;
        sd_debug_count = i + 1;
        sd_release(); if (r == 1) break; sleep_ms(10);
    }
    if (r != 1) { sd_last_error = 2; goto fail; }
    r = sd_command(8, 0x1aa);
    bool v2 = false;
    if (r == 1) {
        uint8_t ext[4]; for (unsigned i = 0; i < 4; i++) ext[i] = sd_byte(0xff);
        v2 = (ext[2] == 1 && ext[3] == 0xaa);
        sd_release(); if (!v2) { sd_last_error = 3; goto fail; }
    } else { sd_release(); if (r != 5) { sd_last_error = 3; goto fail; } }
    {
        uint64_t deadline = time_us_64() + 2000000;
        do {
            r = sd_command(55, 0); sd_release();
            if (r > 1) { sd_last_error = 4; goto fail; }
            r = sd_command(41, v2 ? 0x40000000 : 0); sd_release();
            if (r == 0) break;
            if (r != 1 || !sd_card_present()) { sd_last_error = 4; goto fail; }
            sleep_ms(10);
        } while (time_us_64() < deadline);
        if (r != 0) { sd_last_error = 4; goto fail; }
    }
    r = sd_command(58, 0);
    if (r != 0) { sd_release(); sd_last_error = 5; goto fail; }
    {
        uint32_t ocr = 0; for (unsigned i = 0; i < 4; i++) ocr = (ocr << 8) | sd_byte(0xff);
        sd_release();
        if (!(ocr & 0x80000000) || !(ocr & 0x00300000)) { sd_last_error = 5; goto fail; }
        sd_block_addressed = v2 && (ocr & 0x40000000);
    }
    if (!sd_block_addressed) { r = sd_command(16, 512); sd_release(); if (r != 0) { sd_last_error = 6; goto fail; } }

    spi_set_baudrate(spi1, sd_read_hz); sd_initialized = true; return 0;
fail:
    sd_card_off(); return (int)sd_last_error;
}

// CRC16-CCITT（多項式0x1021、初期値0）。セクターおよび診断用に使用。
static uint16_t sd_crc16(const uint8_t *data, unsigned size) {
    uint16_t crc = 0;
    for (unsigned i = 0; i < size; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

// CMD17で512バイト読み、カードが付けたCRC16と比較する。
// エラー10未初期化/未挿入、11アドレス範囲、12 CMD17、13待機時間超過、14データトークン、15 CRC不一致。
static bool sd_read_sector(uint32_t lba, uint8_t *data) {
    sd_read_lba = lba; sd_read_error = 0; sd_read_r1 = 0xff; sd_read_token = 0xff;
    sd_read_crc_wire = 0; sd_read_crc_calc = 0;
    if (!sd_initialized || !sd_card_present()) { sd_read_error = 10; return false; }
    if (!sd_block_addressed && lba > 0xffffffffu / 512u) { sd_read_error = 11; return false; }
    sd_read_r1 = sd_command(17, sd_block_addressed ? lba : lba * 512u);
    if (sd_read_r1 != 0) { sd_read_error = 12; sd_release(); return false; }
    uint64_t deadline = time_us_64() + 500000;
    do {
        sd_read_token = sd_byte(0xff);
        if (sd_read_token != 0xff) break;
    } while (time_us_64() < deadline);
    if (sd_read_token != 0xfe) {
        sd_read_error = (sd_read_token == 0xff) ? 13 : 14;
        sd_release(); return false;
    }
    for (unsigned i = 0; i < 512; i++) data[i] = sd_byte(0xff);
    sd_read_crc_wire = (uint16_t)sd_byte(0xff) << 8;
    sd_read_crc_wire |= sd_byte(0xff);
    sd_release();
    sd_read_crc_calc = sd_crc16(data, 512);
    if (sd_read_crc_wire != sd_read_crc_calc) { sd_read_error = 15; return false; }
    return true;
}

// 読み取りラッパー。失敗したらSD_FALLBACK_HZへ落として再試行する。
static bool sd_read_checked(uint32_t lba) {
    if (sd_read_sector(lba, sd_sector)) return true;
    if (sd_read_hz <= SD_FALLBACK_HZ || !sd_card_present()) return false;
    sd_read_hz = SD_FALLBACK_HZ; spi_set_baudrate(spi1, sd_read_hz);
    return sd_read_sector(lba, sd_sector);
}
// カード上のリトルエンディアン16ビット値を、非アラインアクセスなしで読む。
static uint16_t sd_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
// カード上のリトルエンディアン32ビット値を読む。
static uint32_t sd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
// ブートセクター/MBR末尾の0x55,0xAAを確認する。
static bool sd_signature(const uint8_t *b) { return b[510] == 0x55 && b[511] == 0xaa; }
// BPBの基本形を確認する（512バイト、クラスタサイズが2のべき乗など）。
static bool sd_bpb_shape(const uint8_t *b) {
    unsigned spc = b[13];
    return sd_signature(b) && (b[0] == 0xeb || b[0] == 0xe9) && sd_le16(b + 11) == 512 &&
        spc && spc <= 128 && !(spc & (spc - 1)) && sd_le16(b + 14) && b[16];
}

// BPBから領域を計算し、範囲とFAT32のクラスタ数を確認する。
// FSエラー20署名、21基本パーティション、22 BPB、23 FAT12/16、24領域/サイズ、25 FAT予約項目、26ルートFAT。
static bool sd_parse_bpb(const uint8_t *b, uint32_t start, uint32_t partition_size) {
    sd_fs_kind = 0;
    if (!sd_bpb_shape(b)) { sd_fs_error = 22; return false; }
    uint32_t total = sd_le16(b + 19); if (!total) total = sd_le32(b + 32);
    uint32_t fat_size = sd_le16(b + 22); if (!fat_size) fat_size = sd_le32(b + 36);
    uint32_t root_sectors = ((uint32_t)sd_le16(b + 17) * 32u + 511u) / 512u;
    uint64_t overhead = (uint64_t)sd_le16(b + 14) + (uint64_t)b[16] * fat_size + root_sectors;
    if (!fat_size || !total || overhead >= total ||
        (partition_size && total > partition_size) || (uint64_t)start + total > 0x100000000ull) {
        sd_fs_error = 24; return false;
    }
    uint32_t clusters = (uint32_t)((total - overhead) / b[13]);
    sd_fs_kind = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if (sd_fs_kind != 32) { sd_fs_error = 23; return false; }
    uint32_t root = sd_le32(b + 44);
    uint16_t flags = sd_le16(b + 40);
    uint8_t active = (flags & 0x80) ? (uint8_t)(flags & 15) : 0;
    if (sd_le16(b + 17) != 0 || sd_le16(b + 22) != 0 || sd_le16(b + 19) != 0 ||
        sd_le16(b + 42) != 0 || clusters >= 0x0ffffff5u ||
        root < 2 || root >= clusters + 2u || active >= b[16] ||
        (uint64_t)fat_size * 128u < (uint64_t)clusters + 2u ||
        b[21] < 0xf0) { sd_fs_error = 24; return false; }
    sd_fat.mirrored = !(flags & 0x80);
    uint32_t reserved = sd_le16(b + 14), fsi = sd_le16(b + 48), backup = sd_le16(b + 50);
    sd_fat.fsinfo_lba = (fsi && fsi < reserved) ? start + fsi : 0;
    sd_fat.fsinfo_backup_lba = (sd_fat.fsinfo_lba && backup && backup != 0xffff && backup + fsi < reserved) ? start + backup + fsi : 0;
    sd_fat.start = start; sd_fat.total = total; sd_fat.fat_size = fat_size;
    sd_fat.fats = b[16]; sd_fat.active_fat = active; sd_fat.media = b[21];
    sd_fat.sectors_per_cluster = b[13]; sd_fat.clusters = clusters; sd_fat.root_cluster = root;
    sd_fat.fat_lba = start + sd_le16(b + 14) + (uint32_t)active * fat_size;
    sd_fat.data_lba = (uint32_t)((uint64_t)start + overhead);
    sd_fat.root_lba = sd_fat.data_lba + (root - 2u) * b[13];
    sd_fs_error = 0; return true;
}
// MBR/BPB、使用FAT、ルート先頭を読む。全領域の整合性検査ではない。
static bool sd_fat32_probe(void) {
    sd_fs_error = 0; sd_fs_kind = 0; sd_fat_ready = false; sd_fat.partitioned = false;
    if (!sd_read_checked(0)) return false;
    if (!sd_signature(sd_sector)) { sd_fs_error = 20; return false; }
    uint32_t start = 0, size = 0;
    if (!sd_bpb_shape(sd_sector)) {
        bool found = false;
        for (unsigned i = 0; i < 4; i++) {
            const uint8_t *p = sd_sector + 446 + i * 16;
            uint8_t type = p[4];
            if (type == 0x0b || type == 0x0c || type == 0x1b || type == 0x1c || type == 0x06 || type == 0x0e || type == 0x04) {
                start = sd_le32(p + 8); size = sd_le32(p + 12);
                if (start && size && (uint64_t)start + size <= 0x100000000ull && (p[0] == 0 || p[0] == 0x80)) { found = true; break; }
            }
        }
        if (!found) { sd_fs_error = 21; return false; }
        sd_fat.partitioned = true;
        if (!sd_read_checked(start)) return false;
    }
    if (!sd_parse_bpb(sd_sector, start, size)) return false;
    if (!sd_read_checked(sd_fat.fat_lba)) return false;
    uint32_t e0 = sd_le32(sd_sector) & 0x0fffffffu, e1 = sd_le32(sd_sector + 4) & 0x0fffffffu;
    if (e0 != (0x0fffff00u | sd_fat.media) || (e1 & 0x03ffffffu) != 0x03ffffffu) { sd_fs_error = 25; return false; }
    uint32_t root_fat_sector = sd_fat.fat_lba + sd_fat.root_cluster / 128u;
    if (root_fat_sector != sd_fat.fat_lba && !sd_read_checked(root_fat_sector)) return false;
    uint32_t next = sd_le32(sd_sector + (sd_fat.root_cluster % 128u) * 4u) & 0x0fffffffu;
    if (!(next >= 0x0ffffff8u || (next >= 2 && next < sd_fat.clusters + 2u))) { sd_fs_error = 26; return false; }
    if (!sd_read_checked(sd_fat.root_lba)) return false;
    sd_fat_ready = true; return true;
}

typedef struct { uint32_t cluster, size; } SdFileInfo;
static uint8_t sd_file_data[4096];
static unsigned sd_file_error = 0;

// 通常のデータクラスタ番号か確認する。0/1や予約番号は含めない。
static bool sd_valid_cluster(uint32_t c) { return c >= 2 && c < sd_fat.clusters + 2u; }
// FATから次クラスタを得る。終端は0x0FFFFFF8以上。異常番号は拒否。
static bool sd_next_cluster(uint32_t c, uint32_t *next) {
    if (!sd_valid_cluster(c)) { sd_file_error = 31; return false; }
    if (!sd_read_checked(sd_fat.fat_lba + c / 128u)) { sd_file_error = 35; return false; }
    *next = sd_le32(sd_sector + (c % 128u) * 4u) & 0x0fffffffu;
    if (*next >= 0x0ffffff8u) return true;
    if (!sd_valid_cluster(*next)) { sd_file_error = 31; return false; }
    return true;
}
// 8.3短縮名の11バイト（空白埋め）を比較する。
static bool sd_name_equal(const uint8_t *entry, const char *name11) {
    for (unsigned i = 0; i < 11; i++) if (entry[i] != (uint8_t)name11[i]) return false;
    return true;
}
// ルートを探索し、通常ファイルを探す。削除済み/LFN/ディレクトリは飛ばす。
// ファイルエラー30未発見、31クラスタ異常、32探索上限、33サイズ超過、34循環/途中終端、35読み取り。
static bool sd_find_root(const char *name11, SdFileInfo *out) {
    sd_file_error = 0; sd_read_error = 0;
    uint32_t c = sd_fat.root_cluster, seen[32]; unsigned nseen = 0, sectors = 0;
    for (;;) {
        if (!sd_valid_cluster(c)) { sd_file_error = 31; return false; }
        for (unsigned i = 0; i < nseen; i++) if (seen[i] == c) { sd_file_error = 34; return false; }
        if (nseen == 32) { sd_file_error = 32; return false; }
        seen[nseen++] = c;
        uint32_t first = sd_fat.data_lba + (c - 2u) * sd_fat.sectors_per_cluster;
        for (unsigned j = 0; j < sd_fat.sectors_per_cluster; j++) {
            if (sectors++ == 256) { sd_file_error = 32; return false; }
            if (!sd_read_checked(first + j)) { sd_file_error = 35; return false; }
            for (unsigned off = 0; off < 512; off += 32) {
                const uint8_t *e = sd_sector + off;
                if (e[0] == 0) { sd_file_error = 30; return false; }
                if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 0x18)) continue;
                if (sd_name_equal(e, name11)) {
                    out->cluster = (((uint32_t)sd_le16(e + 20) << 16) | sd_le16(e + 26)) & 0x0fffffffu;
                    out->size = sd_le32(e + 28);
                    if (out->size && !sd_valid_cluster(out->cluster)) { sd_file_error = 31; return false; }
                    return true;
                }
            }
        }
        uint32_t next;
        if (!sd_next_cluster(c, &next)) return false;
        if (next >= 0x0ffffff8u) { sd_file_error = 30; return false; }
        c = next;
    }
}
// 4096バイト以下のファイルを一時領域へ読む。分散クラスタに対応し、循環を拒否。
static bool sd_read_small_file(const SdFileInfo *file) {
    sd_file_error = 0; sd_read_error = 0;
    if (file->size > sizeof(sd_file_data)) { sd_file_error = 33; return false; }
    if (file->size == 0) return true;
    uint32_t c = file->cluster, seen[8], done = 0; unsigned nseen = 0;
    while (done < file->size) {
        if (!sd_valid_cluster(c)) { sd_file_error = 31; return false; }
        for (unsigned i = 0; i < nseen; i++) if (seen[i] == c) { sd_file_error = 34; return false; }
        if (nseen == 8) { sd_file_error = 34; return false; }
        seen[nseen++] = c;
        uint32_t first = sd_fat.data_lba + (c - 2u) * sd_fat.sectors_per_cluster;
        for (unsigned j = 0; j < sd_fat.sectors_per_cluster && done < file->size; j++) {
            if (!sd_read_checked(first + j)) { sd_file_error = 35; return false; }
            unsigned n = (unsigned)(file->size - done); if (n > 512) n = 512;
            for (unsigned k = 0; k < n; k++) sd_file_data[done + k] = sd_sector[k];
            done += n;
        }
        if (done < file->size) {
            uint32_t next;
            if (!sd_next_cluster(c, &next)) return false;
            if (next >= 0x0ffffff8u) { sd_file_error = 34; return false; }
            c = next;
        }
    }
    return true;
}

static uint32_t sd_program_file_size = 0;
// 保存番号200～299をPnnn.IJB (バイナリ) またはPnnn.BAS (アスキー) の11バイト短縮名に変換する。
static bool sd_slot_name(int n, char name[12], bool ascii) {
    if (n < 200 || n > 299) return false;
    name[0] = 'P'; name[1] = (char)('0' + n / 100);
    name[2] = (char)('0' + (n / 10) % 10); name[3] = (char)('0' + n % 10);
    for (unsigned i = 4; i < 8; i++) name[i] = ' ';
    if (ascii) { name[8] = 'B'; name[9] = 'A'; name[10] = 'S'; }
    else { name[8] = 'I'; name[9] = 'J'; name[10] = 'B'; }
    name[11] = 0;
    return true;
}

// 行番号・偶数に詰めた行長・文字列終端を検査する。戻り値はプログラム使用バイト数。
// レコードは行番号LE16、詰めた行長8ビット、本文、NUL/埋め草。終端行番号0は使用量に含めない。
static int sd_validate_program(const uint8_t *data, uint32_t size) {
    if (size < 2 || size > 4096) return -1;
    uint32_t pos = 0; unsigned previous = 0;
    for (;;) {
        // 4096バイトちょうど使った場合、終端2バイトはBASIC RAMの管理領域に追加する。
        if (pos == 4096 && size == 4096) return 4096;
        if (pos + 2 > size) return -1;
        unsigned line = sd_le16(data + pos);
        if (line == 0) return (int)pos;
        if (line > 32767 || line <= previous || pos + 3 > size) return -1;
        unsigned len = data[pos + 2];
        if (!len || (len & 1) || pos + len + 4 > size || data[pos + len + 3] != 0) return -1;
        bool nul = false;
        for (unsigned i = 0; i <= len; i++) {
            uint8_t c = data[pos + 3 + i];
            if (c == 0) nul = true;
            else if (nul || c == '\r' || c == '\n') return -1;
        }
        if (data[pos + 3] == 0) return -1;
        previous = line; pos += len + 4;
    }
}
// LOAD/LRUN用 (バイナリ.IJB)。ファイル全体を読んで検査し、成功後にBASIC側がRAMへ反映する。
// エラー36はSDON/FAT32確認が必要、37は内部プログラム形式の不正。
static int sd_prepare_program(int n) {
    sd_file_error = 0; sd_read_error = 0;
    if (!sd_initialized || !sd_fat_ready) { sd_file_error = 36; return -1; }
    char name[12]; if (!sd_slot_name(n, name, false)) { sd_file_error = 37; return -1; }
    SdFileInfo file;
    if (!sd_find_root(name, &file) || !sd_read_small_file(&file)) return -1;
    int length = sd_validate_program(sd_file_data, file.size);
    if (length < 0) { sd_file_error = 37; return -1; }
    sd_program_file_size = file.size; return length;
}

// ===== UTF-8 → IchigoJam内部文字コード変換 =====
// IchigoJamの半角カナはJIS X 0201の1バイトコード (0xA1-0xDF、romajikana.hと同じ配置)。
// UTF-8の.BASをPCで作成した場合に文字化けしないよう、読み込み時に変換する。
// 一致しないバイト・不完全な列はそのまま素通しするため、
// Shift_JIS相当の生バイト (旧形式) ファイルもそのまま読み込める (自動判別)。

// 全角カナ (U+30A1-30F6) → 半角カナ 変換テーブル。下位8bit=1文字目、上位8bit=2文字目 (0なら1文字)。0x0000=変換なし。
// (ﾞ=0xDE、ﾟ=0xDF。PythonのUnicode正規化で生成しromajikana.h (0xDD=ﾝ等) と照合済み)
static const uint16_t SD_FWKANA[86] = {
    0x00A7, 0x00B1, 0x00A8, 0x00B2, 0x00A9, 0x00B3,
    0x00AA, 0x00B4, 0x00AB, 0x00B5, 0x00B6, 0xDEB6,
    0x00B7, 0xDEB7, 0x00B8, 0xDEB8, 0x00B9, 0xDEB9,
    0x00BA, 0xDEBA, 0x00BB, 0xDEBB, 0x00BC, 0xDEBC,
    0x00BD, 0xDEBD, 0x00BE, 0xDEBE, 0x00BF, 0xDEBF,
    0x00C0, 0xDEC0, 0x00C1, 0xDEC1, 0x00AF, 0x00C2,
    0xDEC2, 0x00C3, 0xDEC3, 0x00C4, 0xDEC4, 0x00C5,
    0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0xDECA,
    0xDFCA, 0x00CB, 0xDECB, 0xDFCB, 0x00CC, 0xDECC,
    0xDFCC, 0x00CD, 0xDECD, 0xDFCD, 0x00CE, 0xDECE,
    0xDFCE, 0x00CF, 0x00D0, 0x00D1, 0x00D2, 0x00D3,
    0x00AC, 0x00D4, 0x00AD, 0x00D5, 0x00AE, 0x00D6,
    0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x0000,
    0x00DC, 0x0000, 0x0000, 0x00A6, 0x00DD, 0xDEB3,
    0x0000, 0x0000,
};
// UTF-8テキストをIchigoJam内部文字コードへ変換する (dst==src可。変換結果は必ず元長以下)。
// 変換: BOM除去、半角カナ(U+FF61-FF9F)→0xA1-0xDF、全角カナ(U+30A1-30F6)→半角カナ1-2バイト、
//       全角英数記号(U+FF01-FF5E)→ASCII、全角空白/、/。→SP/0xA4/0xA1、ー(U+30FC)→0xB0、¥(U+00A5)→0x5C。
static unsigned sd_utf8_to_ij(uint8_t *dst, const uint8_t *src, unsigned len) {
    unsigned r = 0, w = 0;
    while (r < len) {
        uint8_t b = src[r];
        if (b < 0x80) { dst[w++] = b; r++; continue; }
        if (b == 0xC2 && r + 1 < len && src[r + 1] == 0xA5) { // ¥ → 0x5C
            dst[w++] = 0x5C; r += 2; continue;
        }
        if (b == 0xEF && r + 2 < len) {
            uint8_t b2 = src[r + 1], b3 = src[r + 2];
            if (b2 == 0xBB && b3 == 0xBF) { r += 3; continue; } // BOM除去
            if (b2 == 0xBD) {
                if (b3 >= 0xA1 && b3 <= 0xBF) { dst[w++] = b3; r += 3; continue; } // 半角カナ ｡-ｿ
                if (b3 >= 0x80 && b3 <= 0x9E) { dst[w++] = (uint8_t)(b3 & 0x3F) + 0x60; r += 3; continue; } // 全角ASCII `-~
            } else if (b2 == 0xBE) {
                if (b3 >= 0x80 && b3 <= 0x9F) { dst[w++] = b3 + 0x40; r += 3; continue; } // 半角カナ ﾀ-ﾟ
            } else if (b2 == 0xBC) {
                if (b3 >= 0x81 && b3 <= 0xBF) { dst[w++] = b3 - 0x60; r += 3; continue; } // 全角ASCII !-_
            }
        }
        if (b == 0xE3 && r + 2 < len) {
            uint8_t b2 = src[r + 1], b3 = src[r + 2];
            if ((b2 == 0x80 && b3 >= 0x80 && b3 <= 0x82) || // 全角空白/、/。
                (b2 == 0x82 && b3 >= 0xA1 && b3 <= 0xBF) || // 全角カナ U+30A1-30BF
                (b2 == 0x83 && b3 >= 0x80 && b3 <= 0xBC)) { // 全角カナ U+30C0-30FC
                unsigned cp = 0x3000 + ((b2 & 0x3F) << 6) + (b3 & 0x3F);
                if (cp == 0x3000) { dst[w++] = ' '; r += 3; continue; }
                if (cp == 0x3001) { dst[w++] = 0xA4; r += 3; continue; } // ､
                if (cp == 0x3002) { dst[w++] = 0xA1; r += 3; continue; } // ｡
                if (cp == 0x30FC) { dst[w++] = 0xB0; r += 3; continue; } // ｰ
                if (cp >= 0x30A1 && cp <= 0x30F6) {
                    uint16_t v = SD_FWKANA[cp - 0x30A1];
                    if (v) {
                        dst[w++] = (uint8_t)(v & 0xFF);
                        if (v >> 8) dst[w++] = (uint8_t)(v >> 8);
                        r += 3; continue;
                    }
                }
            }
        }
        dst[w++] = b; r++; // 素通し (生バイト・未知・不完全な列)
    }
    return w;
}

// LOAD/LRUN用 (アスキー.BAS)。テキストファイルをsd_file_dataへ読む。
// 戻り値はテキスト長 (最初のNULまで。通常はファイルサイズそのまま)。
// 読み込み後、UTF-8の文字列をIchigoJam内部文字コードへ変換する (sd_utf8_to_ij)。
// ファイルエラーはsd_prepare_programと共通 (30未発見、33サイズ超過、35読み取り等)。
static int sd_prepare_text_program(int n) {
    sd_file_error = 0; sd_read_error = 0;
    if (!sd_initialized || !sd_fat_ready) { sd_file_error = 36; return -1; }
    char name[12]; if (!sd_slot_name(n, name, true)) { sd_file_error = 37; return -1; }
    SdFileInfo file;
    if (!sd_find_root(name, &file) || !sd_read_small_file(&file)) return -1;
    uint32_t size = file.size;
    while (size && sd_file_data[size - 1] == 0) size--; // 末尾のパディング除去
    for (uint32_t i = 0; i < size; i++) {
        if (sd_file_data[i] == 0) { size = i; break; } // NULは行終端として扱わないため打ち切り
    }
    size = sd_utf8_to_ij(sd_file_data, sd_file_data, size); // UTF-8→IchigoJam文字コード変換
    sd_program_file_size = file.size;
    return (int)size;
}

static uint8_t sd_verify_sector[512];
static unsigned sd_write_error = 0;
static uint8_t sd_write_r1 = 0xff, sd_write_response = 0xff;
static uint32_t sd_write_lba = 0;
// ディレクトリ更新を開始/照合済みか保持し、失敗時の曖昧さを区別する。
static bool sd_save_commit_started = false, sd_save_committed = false;
static uint32_t sd_alloc_hint = 2;
// 16ビット値をカード形式のリトルエンディアンで書き込む。
static void sd_put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
// 32ビット値をカード形式のリトルエンディアンで書き込む。
static void sd_put_le32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// CMD24で1セクターを書き、ビジー終了後に読み戻してCRCと全バイトを照合する。
// 書き込みエラー40未準備/範囲、41 CMD24、42応答拒否、43ビジー時間超過、44読み戻し通信、45内容不一致。
static bool sd_write_verified(uint32_t lba, const uint8_t *data) {
    sd_write_error = 0; sd_write_lba = lba; sd_write_r1 = 0xff; sd_write_response = 0xff;
    uint64_t end = (uint64_t)sd_fat.start + sd_fat.total;
    if (!sd_initialized || !sd_fat_ready || !sd_card_present() ||
        lba < sd_fat.start || (uint64_t)lba >= end ||
        (!sd_block_addressed && lba > 0xffffffffu / 512u)) { sd_write_error = 40; return false; }
    sd_write_r1 = sd_command(24, sd_block_addressed ? lba : lba * 512u);
    if (sd_write_r1 != 0) { sd_write_error = 41; sd_release(); return false; }
    sd_byte(0xff); sd_byte(0xfe);
    for (unsigned i = 0; i < 512; i++) sd_byte(data[i]);
    uint16_t crc = sd_crc16(data, 512); sd_byte((uint8_t)(crc >> 8)); sd_byte((uint8_t)crc);
    uint64_t deadline = time_us_64() + 500000;
    do { sd_write_response = sd_byte(0xff); if (sd_write_response != 0xff) break; } while (time_us_64() < deadline);
    if ((sd_write_response & 0x1f) != 0x05) { sd_write_error = 42; sd_release(); return false; }
    if (!sd_wait_ready(2000)) { sd_write_error = 43; sd_release(); return false; }
    sd_release();
    if (!sd_read_sector(lba, sd_verify_sector)) { sd_write_error = 44; return false; }
    for (unsigned i = 0; i < 512; i++) if (sd_verify_sector[i] != data[i]) { sd_write_error = 45; return false; }
    return true;
}

// FATの上位4ビットを保存して項目を更新する。ミラー有効時は全コピーを書き換える。
// 書く前にFATのセクター同士を照合し、不一致ならエラー54で中止する。
static bool sd_set_fat(uint32_t c, uint32_t value) {
    if (c >= sd_fat.clusters + 2u) { sd_file_error = 31; return false; }
    uint32_t sector = c / 128u, offset = (c % 128u) * 4u;
    uint32_t base = sd_fat.fat_lba - (uint32_t)sd_fat.active_fat * sd_fat.fat_size;
    if (!sd_read_checked(sd_fat.fat_lba + sector)) { sd_file_error = 35; return false; }
    unsigned copies = sd_fat.mirrored ? sd_fat.fats : 1;
    if (sd_fat.mirrored) {
        for (unsigned copy = 1; copy < copies; copy++) {
            if (!sd_read_sector(base + (uint32_t)copy * sd_fat.fat_size + sector, sd_verify_sector)) { sd_file_error = 35; return false; }
            for (unsigned i = 0; i < 512; i++) if (sd_verify_sector[i] != sd_sector[i]) { sd_file_error = 54; return false; }
        }
    }
    uint32_t old = sd_le32(sd_sector + offset);
    sd_put_le32(sd_sector + offset, (old & 0xf0000000u) | (value & 0x0fffffffu));
    for (unsigned copy = 0; copy < copies; copy++) {
        uint32_t lba = sd_fat.mirrored ? base + (uint32_t)copy * sd_fat.fat_size + sector : sd_fat.fat_lba + sector;
        if (!sd_write_verified(lba, sd_sector)) return false;
    }
    return true;
}
// 有効なFSInfoの空き数/次候補を不明（0xFFFFFFFF）にする。PC側で再計算できるようにする。
static bool sd_invalidate_fsinfo(uint32_t lba) {
    if (!lba) return true;
    if (!sd_read_checked(lba)) { sd_file_error = 35; return false; }
    if (sd_le32(sd_sector) != 0x41615252u || sd_le32(sd_sector + 484) != 0x61417272u ||
        sd_le32(sd_sector + 508) != 0xaa550000u) return true;
    sd_put_le32(sd_sector + 488, 0xffffffffu); sd_put_le32(sd_sector + 492, 0xffffffffu);
    return sd_write_verified(lba, sd_sector);
}
typedef struct {
    uint32_t lba, cluster, size, next_lba; unsigned offset;
    bool exists, ended; uint8_t attr;
} SdDirSlot;

// 既存ファイルを優先。なければ削除済み項目か終端項目を使う。ルート拡張は未対応。
static bool sd_find_save_slot(const char *name, SdDirSlot *slot) {
    uint32_t c = sd_fat.root_cluster, seen[32]; unsigned count = 0, sectors = 0; bool have_free = false;
    for (;;) {
        if (!sd_valid_cluster(c)) { sd_file_error = 31; return false; }
        for (unsigned i = 0; i < count; i++) if (seen[i] == c) { sd_file_error = 34; return false; }
        if (count == 32) { sd_file_error = 32; return false; } seen[count++] = c;
        uint32_t first = sd_fat.data_lba + (c - 2u) * sd_fat.sectors_per_cluster;
        for (unsigned j = 0; j < sd_fat.sectors_per_cluster; j++) {
            if (sectors++ == 256) { sd_file_error = 32; return false; }
            if (!sd_read_checked(first + j)) { sd_file_error = 35; return false; }
            for (unsigned off = 0; off < 512; off += 32) {
                const uint8_t *e = sd_sector + off;
                if (e[0] == 0xe5) {
                    if (!have_free) { *slot = (SdDirSlot){ .lba = first + j,.offset = off }; have_free = true; }
                    continue;
                }
                if (e[0] == 0) {
                    if (have_free) return true;
                    *slot = (SdDirSlot){ .lba = first + j,.offset = off,.ended = true };
                    if (off == 480) {
                        if (j + 1 < sd_fat.sectors_per_cluster) slot->next_lba = first + j + 1;
                        else { uint32_t next; if (!sd_next_cluster(c, &next)) return false;
                            if (next < 0x0ffffff8u) slot->next_lba = sd_fat.data_lba + (next - 2u) * sd_fat.sectors_per_cluster; }
                    }
                    return true;
                }
                if (e[11] == 0x0f) continue;
                if (sd_name_equal(e, name)) {
                    if (e[11] & 0x19) { sd_file_error = 50; return false; }
                    *slot = (SdDirSlot){ .lba = first + j,.offset = off,.exists = true,.attr = e[11],
                        .cluster = (((uint32_t)sd_le16(e + 20) << 16) | sd_le16(e + 26)) & 0x0fffffffu,.size = sd_le32(e + 28) };
                    return true;
                }
            }
        }
        uint32_t next; if (!sd_next_cluster(c, &next)) return false;
        if (next >= 0x0ffffff8u) { if (have_free) return true; sd_file_error = 51; return false; } c = next;
    }
}
// 上書き前に旧クラスタ列を調べる。最大8クラスタ、循環/サイズ異常は拒否する。
static bool sd_collect_old_chain(const SdDirSlot *slot, uint32_t old[8], unsigned *count) {
    *count = 0;
    if (!slot->exists || !slot->cluster) {
        if (slot->exists && slot->size) { sd_file_error = 31; return false; } return true;
    }
    if (slot->size > 4096) { sd_file_error = 53; return false; }
    uint32_t c = slot->cluster;
    while (c < 0x0ffffff8u) {
        if (!sd_valid_cluster(c)) { sd_file_error = 31; return false; }
        for (unsigned i = 0; i < *count; i++) if (old[i] == c) { sd_file_error = 34; return false; }
        if (*count == 8) { sd_file_error = 53; return false; } old[(*count)++] = c;
        if (!sd_next_cluster(c, &c)) return false;
    }
    if ((uint64_t)*count * sd_fat.sectors_per_cluster * 512u < slot->size) { sd_file_error = 34; return false; }
    return true;
}
// 空きクラスタを先に必要数探索し、その後FATへ新しい列を登録する。
static bool sd_allocate_chain(unsigned needed, uint32_t chain[8]) {
    unsigned count = 0; uint32_t c = sd_valid_cluster(sd_alloc_hint) ? sd_alloc_hint : 2, scanned = 0, last_sector = 0xffffffffu;

    while (scanned < sd_fat.clusters && count < needed) {
        uint32_t sector = c / 128u;
        if (sector != last_sector) { if (!sd_read_checked(sd_fat.fat_lba + sector)) { sd_file_error = 35; return false; } last_sector = sector; }
        if ((sd_le32(sd_sector + (c % 128u) * 4u) & 0x0fffffffu) == 0) chain[count++] = c;
        c++; scanned++; if (c >= sd_fat.clusters + 2u) c = 2;
    }
    if (count != needed) { sd_file_error = 52; return false; }
    sd_alloc_hint = c;
    for (unsigned i = 0; i < needed; i++) if (!sd_set_fat(chain[i], i + 1 < needed ? chain[i + 1] : 0x0fffffffu)) return false;
    return true;
}
// SAVE用。新規データの書き込みと照合→ディレクトリ切り替え→旧列解放の順に行う。
// 途中失敗したら再度SDONが必要。確保済み領域やディレクトリ更新が残る場合がある。
// ファイルエラー50保護/名前衝突、51ルート満杯、52空き不足、53旧ファイル/列の上限、54 FAT不一致。
static bool sd_save_program(int n, const uint8_t *program, unsigned size, bool ascii) {
    sd_file_error = 0; sd_read_error = 0; sd_write_error = 0; sd_save_commit_started = false; sd_save_committed = false;
    if (!sd_initialized || !sd_fat_ready) { sd_file_error = 36; return false; }
    char name[12]; if (size > 4096 || !sd_slot_name(n, name, ascii)) { sd_file_error = 37; return false; }
    for (unsigned i = 0; i < 4096; i++) sd_file_data[i] = i < size ? program[i] : 0;
    if (!ascii && sd_validate_program(sd_file_data, 4096) != (int)size) { sd_file_error = 37; return false; }
    SdDirSlot slot; if (!sd_find_save_slot(name, &slot)) return false;
    uint32_t old[8], fresh[8]; unsigned old_count;
    if (!sd_collect_old_chain(&slot, old, &old_count)) return false;
    if (!sd_read_checked(sd_fat.fat_lba)) { sd_file_error = 35; return false; }
    uint32_t flags = sd_le32(sd_sector + 4) & 0x0fffffffu; bool was_clean = (flags & 0x08000000u) != 0;

    // 変更前にクリーンビットを落とす。成功時のみ、もともとクリーンなら復元する。
    if (!sd_set_fat(1, flags & ~0x08000000u)) goto failure;
    if (!sd_invalidate_fsinfo(sd_fat.fsinfo_lba) || !sd_invalidate_fsinfo(sd_fat.fsinfo_backup_lba)) goto failure;
    unsigned needed = (8u + sd_fat.sectors_per_cluster - 1u) / sd_fat.sectors_per_cluster;
    if (!sd_allocate_chain(needed, fresh)) goto failure;
    for (unsigned sector = 0; sector < 8; sector++) {
        unsigned ci = sector / sd_fat.sectors_per_cluster, j = sector % sd_fat.sectors_per_cluster;
        uint32_t lba = sd_fat.data_lba + (fresh[ci] - 2u) * sd_fat.sectors_per_cluster + j;
        if (!sd_write_verified(lba, sd_file_data + sector * 512u)) goto failure;
    }

    if (slot.ended && slot.next_lba) {
        if (!sd_read_checked(slot.next_lba)) { sd_file_error = 35; goto failure; }
        sd_sector[0] = 0; if (!sd_write_verified(slot.next_lba, sd_sector)) goto failure;
    }
    if (!sd_read_checked(slot.lba)) { sd_file_error = 35; goto failure; }
    uint8_t *entry = sd_sector + slot.offset;
    if (!slot.exists) {
        for (unsigned i = 0; i < 32; i++) entry[i] = 0;
        for (unsigned i = 0; i < 11; i++) entry[i] = (uint8_t)name[i];
        sd_put_le16(entry + 16, 0x21); sd_put_le16(entry + 18, 0x21);
    }
    entry[11] = (uint8_t)(slot.attr | 0x20); entry[12] = 0;
    sd_put_le16(entry + 20, (uint16_t)(fresh[0] >> 16)); sd_put_le16(entry + 26, (uint16_t)fresh[0]);
    sd_put_le16(entry + 22, 0); sd_put_le16(entry + 24, 0x21);
    // .IJBは従来形式互換でサイズ4096固定、.BASはテキストの実サイズを記録する
    sd_put_le32(entry + 28, ascii ? size : 4096);
    if (slot.ended && slot.offset < 480) sd_sector[slot.offset + 32] = 0;
    // 新規データの照合が済んでから、ファイルが参照するクラスタ番号を切り替える。
    sd_save_commit_started = true;
    if (!sd_write_verified(slot.lba, sd_sector)) goto failure;
    sd_save_committed = true;
    for (unsigned i = 0; i < old_count; i++) if (!sd_set_fat(old[i], 0)) goto failure;
    if (was_clean && !sd_set_fat(1, flags)) goto failure;
    return true;
failure:
    sd_fat_ready = false; return false;
}

static SdFileInfo sd_files_index[100];
// 一覧状態：0未保存、1通常、2不正な項目または重複名。
static uint8_t sd_files_status[100];
// 一覧種別：0=なし、1=.IJB (バイナリ)、2=.BAS (アスキー)
static uint8_t sd_files_kind[100];
// P200.IJB/P200.BAS～P299の短縮名から、一覧用添字0～99を得る。拡張子をkindへ返す。
static int sd_directory_slot_number(const uint8_t *e, uint8_t *kind) {
    if (e[0] != 'P' || e[1] != '2' || e[2] < '0' || e[2] > '9' || e[3] < '0' || e[3] > '9') return -1;
    for (unsigned i = 4; i < 8; i++) if (e[i] != ' ') return -1;
    if (e[8] == 'I' && e[9] == 'J' && e[10] == 'B') *kind = 1;
    else if (e[8] == 'B' && e[9] == 'A' && e[10] == 'S') *kind = 2;
    else return -1;
    return (e[2] - '0') * 10 + (e[3] - '0');
}
// ルートを1回探索して100スロットの一覧を作る。BASICプログラムやFILE()は変更しない。
// 同じ番号に.IJBと.BASがある場合は.IJB (バイナリ) を優先する。
static bool sd_files_build_index(void) {
    sd_files_index_ready = false; sd_file_error = 0; sd_read_error = 0;
    if (!sd_initialized || !sd_fat_ready) { sd_file_error = 36; return false; }
    for (unsigned i = 0; i < 100; i++) { sd_files_status[i] = 0; sd_files_kind[i] = 0; }
    uint32_t c = sd_fat.root_cluster, seen[32]; unsigned count = 0, sectors = 0;
    for (;;) {
        if (!sd_valid_cluster(c)) { sd_file_error = 31; return false; }
        for (unsigned i = 0; i < count; i++) if (seen[i] == c) { sd_file_error = 34; return false; }
        if (count == 32) { sd_file_error = 32; return false; } seen[count++] = c;
        uint32_t first = sd_fat.data_lba + (c - 2u) * sd_fat.sectors_per_cluster;
        for (unsigned j = 0; j < sd_fat.sectors_per_cluster; j++) {
            if (sectors++ == 256) { sd_file_error = 32; return false; }
            if (!sd_read_checked(first + j)) { sd_file_error = 35; return false; }
            for (unsigned off = 0; off < 512; off += 32) {
                const uint8_t *e = sd_sector + off;
                if (e[0] == 0) { sd_files_index_ready = true; return true; }
                if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 0x18)) continue;
                uint8_t kind = 0;
                int n = sd_directory_slot_number(e, &kind); if (n < 0) continue;
                if (sd_files_status[n]) {
                    if (sd_files_kind[n] == 1) { // .IJB登録済み: .BASは無視、.IJB同士の重複は不正
                        if (kind == 1) sd_files_status[n] = 2;
                        continue;
                    }
                    if (kind == 2) { // .BAS同士の重複は不正
                        sd_files_status[n] = 2;
                        continue;
                    }
                    // .BAS登録済みに後から.IJBが見つかった→.IJBで置き換える (フォールスルー)
                }
                SdFileInfo *f = &sd_files_index[n];
                f->size = sd_le32(e + 28); f->cluster = (((uint32_t)sd_le16(e + 20) << 16) | sd_le16(e + 26)) & 0x0fffffffu;
                sd_files_kind[n] = kind;
                sd_files_status[n] = (f->size < 2 || f->size > 4096 || !sd_valid_cluster(f->cluster)) ? 2 : 1;
            }
        }
        uint32_t next; if (!sd_next_cluster(c, &next)) return false;
        if (next >= 0x0ffffff8u) { sd_files_index_ready = true; return true; } c = next;
    }
}

// 先頭セクターをCRC検査し、先頭行を表示用に返す。ファイル全体の検査はLOADが担当。
// .IJBは内部形式の先頭行、.BASはテキストの先頭行 ("行番号 本文") を返す。
// 戻り値0未保存、1先頭行、2空プログラム、3不正、-1通信/状態エラー。
static int sd_files_preview(int number, char *text, unsigned capacity) {
    sd_file_error = 0; sd_read_error = 0;
    if (number < 200 || number > 299 || !capacity || !sd_files_index_ready || !sd_fat_ready) { sd_file_error = 36; return -1; }
    text[0] = 0; unsigned n = (unsigned)(number - 200);
    if (sd_files_status[n] == 0) return 0;
    if (sd_files_status[n] == 2) return 3;
    const SdFileInfo *f = &sd_files_index[n];
    uint32_t first = sd_fat.data_lba + (f->cluster - 2u) * sd_fat.sectors_per_cluster;
    if (!sd_read_checked(first)) { sd_file_error = 35; return -1; }
    if (sd_files_kind[n] == 2) {
        // アスキー形式: 先頭行 "行番号 本文" を取り出し、UTF-8→IchigoJam変換後に本文をプレビューする
        uint32_t size = f->size;
        if (size < 2 || size > 4096) return 3;
        static uint8_t linebuf[300]; // 1行分の一時バッファ
        unsigned lc = 0;
        while (lc < 296 && sd_sector[lc] != '\r' && sd_sector[lc] != '\n' && sd_sector[lc] != 0) {
            linebuf[lc++] = sd_sector[lc];
        }
        lc = sd_utf8_to_ij(linebuf, linebuf, lc);
        unsigned p = 0;
        while (p < lc && linebuf[p] >= '0' && linebuf[p] <= '9') p++;
        if (p == 0 || p >= lc || linebuf[p] != ' ') return 3;
        p++;
        unsigned written = 0;
        while (p < lc) {
            if (written + 1 < capacity) text[written++] = (char)(linebuf[p] < 32 ? '.' : linebuf[p]);
            p++;
        }
        text[written] = 0;
        return written ? 1 : 3;
    }
    // バイナリ形式 (.IJB)
    unsigned line = sd_le16(sd_sector); if (line == 0) return 2;
    unsigned len = sd_sector[2];
    if (line > 32767 || !len || (len & 1) || len + 4 > f->size || sd_sector[len + 3] != 0 || sd_sector[3] == 0) return 3;
    bool nul = false; unsigned written = 0;
    for (unsigned i = 0; i <= len; i++) {
        uint8_t ch = sd_sector[3 + i];
        if (ch == 0) nul = true;
        else if (nul || ch == '\r' || ch == '\n') return 3;
        else if (written + 1 < capacity) text[written++] = (char)(ch < 32 ? '.' : ch);
    }
    text[written] = 0; return 1;
}

#endif
