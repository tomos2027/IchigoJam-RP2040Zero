#include <stdint.h>

#define X_RES	336 // Graphic横方向解像度
#define Y_RES	216 // Graphic縦方向解像度
#define WIDTH_X 42 // 横方向文字数
#define WIDTH_Y 27 // 縦方向文字数
#define ATTROFFSET (WIDTH_X*WIDTH_Y) // TVRAM上のカラーパレット格納位置
#define WIDTH_XBW	80 // 横方向文字数（モノクロ時）
#define ATTROFFSETBW	(WIDTH_XBW*WIDTH_Y) // TVRAM上のカラーパレット格納位置（モノクロ時）
#define WIDTH_XMAX WIDTH_XBW // 横方向最大文字数

// videomode値
//#define VMODE_T30 0 // 標準テキスト30文字互換モード
//#define VMODE_STDTEXT 1 // 標準テキスト36文字モード
//#define VMODE_T40 2 // 標準テキスト40文字互換モード（6ドットフォント）
#define VMODE_WIDETEXT 3 // ワイドテキスト42文字モード
//#define VMODE_WIDETEXT6dot 4 // ワイドテキスト64文字モード（6ドットフォント）
#define VMODE_MONOTEXT 5 // モノクロテキスト80文字モード
#define VMODE_IJTEXT 6 // IchigoJam 32x24文字 白黒モード
//#define VMODE_ZOEAGRPH 16 // type Z互換グラフィックモード
//#define VMODE_STDGRPH 17 // 標準グラフィック＋テキスト36文字モード
#define VMODE_WIDEGRPH 18 // ワイドグラフィック＋テキスト42文字モード

// textomode値
//#define TMODE_T30 0 // 標準テキスト30文字互換モード
//#define TMODE_STDTEXT 1 // 標準テキスト36文字モード
//#define TMODE_T40 2 // 標準テキスト40文字互換モード（6ドットフォント）
#define TMODE_WIDETEXT 3 // ワイドテキスト42文字モード
//#define TMODE_WIDETEXT6dot 4 // ワイドテキスト64文字モード（6ドットフォント）
#define TMODE_MONOTEXT 5 // モノクロテキスト80文字モード

// graphmode値
#define GMODE_NOGRPH 0 // グラフィック不使用
//#define GMODE_ZOEAGRPH 1 // type Z互換グラフィックモード
//#define GMODE_STDGRPH 2 // 標準グラフィック＋テキスト36文字モード
#define GMODE_WIDEGRPH 3 // ワイドグラフィック＋テキスト42文字モード

// NTSC出力 1ラインあたりのサンプル数
#define NUM_LINE_SAMPLES 908  // 227 * 4

// NTSC出力 走査線数
#define NUM_LINES 262  // 走査線262本
#define V_SYNC		10	// 垂直同期本数
#define V_PREEQ		26	// ブランキング区間上側
#define H_SYNC		68	// 水平同期幅、約4.7μsec
#define H_PICTURE (H_SYNC+8+9*4+60) // 映像開始位置

// IchigoJam 32x24文字 白黒モード (VMODE_IJTEXT)
#define IJ_WIDTH 32 // 横方向文字数
#define IJ_ROWS 24 // 縦方向文字数
#define IJ_LINES_PER_CHAR 9 // 1文字の走査線数 (8ラインフォントを9ラインに引き伸ばし)
#define IJ_HEIGHT (IJ_ROWS*IJ_LINES_PER_CHAR) // 映像走査線数 216 = Y_RES
#define IJ_CHAR_SAMPLES 21 // 1文字のサンプル数 (1ドット約2.6サンプルx8=21、字間なし、32文字で672サンプル=画面いっぱい)
#define IJ_ATTROFFSET (IJ_WIDTH*IJ_ROWS) // TVRAM上の属性格納位置 768
#define IJ_PICTURE (H_PICTURE) // 映像開始位置 (42文字モードと同じ最大幅を使う)
#define IJ_ATTR_INVERT 0x80 // 属性 反転ビット
#define IJ_ATTR_CURSOR 0x40 // 属性 カーソルビット (カーソル位置の文字をマスクXORして描画する)

void g_clearscreen(void);
void clearscreen(void);
void start_composite(void);
void stop_composite(void);
void rp2040_pwm_ntsc_init(uint8_t n);
void ntsc_release(void);
void ntsc_set_line_callback(void (*cb)(void));
int ntsc_active(void);
void set_palette(uint16_t c,unsigned char b,unsigned char r,unsigned char g);
void set_videomode(unsigned char m, unsigned char *gvram); //ビデオモードの切り替え

extern volatile uint16_t drawcount;
extern uint8_t TVRAM[]; // Textビデオメモリ
extern uint8_t* GVRAM; // Graphicビデオメモリ
extern uint8_t videomode,textmode,graphmode; //画面モード
extern int twidth,twidthy; //テキスト文字数（横）および（縦）
extern int attroffset; // TVRAMのカラー情報エリア位置
extern uint8_t* Fontp; //現在のフォントパターンの先頭アドレス

//診断用: DMAバッファをカナリアで挟む (structで物理レイアウトを保証)
struct ntsc_dma_buf_t {
    volatile uint32_t canary1;
    uint16_t buf[2][NUM_LINE_SAMPLES];
    volatile uint32_t canary2;
};
extern struct ntsc_dma_buf_t ntsc_dma_buf;
#define dma_buffer ntsc_dma_buf.buf
extern uint8_t ntsc_mag; //文字の拡大倍率 (1,2,4,8。VIDEOコマンドでvram_to_tvram_allが毎フレーム設定する)
extern uint8_t ntsc_cursor_mask; //カーソルの反転マスク (insert時0xff、それ以外0xf0。vram_to_tvram_allが毎フレーム設定)
uint16_t ntsc_scanline(void); //診断用: 走査線カウンタ
