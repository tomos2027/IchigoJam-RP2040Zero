/*----------------------------------------------------------------------------

Copyright (C) 2024, KenKen, all right reserved.

This program supplied herewith by KenKen is free software; you can
redistribute it and/or modify it under the terms of the same license written
here and only for non-commercial purpose.

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of FITNESS FOR A PARTICULAR
PURPOSE. The copyright owner and contributors are NOT LIABLE for any damages
caused by using this program.

----------------------------------------------------------------------------*/

// This signal generation program (using PWM and DMA) is the idea of @lovyan03.
// https://github.com/lovyan03/

//#pragma GCC optimize ("O3")

#include <stdio.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "rp2040_pwm_ntsc_textgraph.h"

// デバッグ用、割込み処理中HIGHになるピン
//#define PIN_DEBUG_BUSY 15

uint8_t TVRAM[WIDTH_XMAX*WIDTH_Y*2+1];
uint8_t *GVRAM=0; //グラフィックVRAM開始位置のポインタ

volatile static uint16_t scanline;
volatile uint16_t drawcount=0; //　1画面表示終了ごとに1足す。アプリ側で0にする。
volatile uint8_t drawing; //　映像区間処理中は-1、その他は0
uint8_t videostop; // 1～3：ビデオ出力停止、0：ビデオ出力中

unsigned char videomode,textmode,graphmode; //画面モード
int twidth,twidthy; //テキスト文字数（横）および（縦）
int attroffset; // TVRAMのカラー情報エリア位置
uint8_t* Fontp; //現在のフォントパターンの先頭アドレス
uint8_t ntsc_mag = 1; //文字の拡大倍率 (1,2,4,8)
uint8_t ntsc_cursor_mask = 0; //カーソルの反転マスク (IJ_ATTR_CURSORのセルに適用)

// DMAピンポンバッファ (診断用カナリアで前後を挟む、structで物理レイアウトを保証)
struct ntsc_dma_buf_t ntsc_dma_buf;

//診断用: 現在の走査線カウンタ (破壊検知)
uint16_t ntsc_scanline(void) {
	return scanline;
}

// カラーパレット
uint16_t color_tbl[4*257] __attribute__ ((aligned (4)));

static uint pwm_dma_chan0,pwm_dma_chan1;
static uint ntsc_pin;
static uint pwm_slice_num;
static void (*ntsc_line_callback)(void) = 0;

void ntsc_set_line_callback(void (*cb)(void)) {
	ntsc_line_callback = cb;
}

int ntsc_active(void) {
	return videostop == 0;
}

static void makeDmaBuffer(uint16_t* buf, size_t line_num)
{
	static uint8_t* fbp;
	static uint8_t* tvp;
	static uint8_t tline = 0;
	static uint8_t trow = 0; //TVRAMの現在行 (拡大描画で使う)
	uint16_t* b = buf;

	if(videostop){
		if(videostop<3){
			drawing=0;
			while (b < buf + NUM_LINE_SAMPLES) *b++ = 0;
			videostop++;
		}
		if(line_num==V_SYNC+V_PREEQ+Y_RES){
			drawcount++;
		}
		return;
	}
	if (line_num < 2)
	{
		//垂直同期信号生成
		for (int j = 0; j < NUM_LINE_SAMPLES-H_SYNC; j++) *b++ = 0;
		while (b < buf + NUM_LINE_SAMPLES) *b++ = 2;
	}
	if(videomode==VMODE_MONOTEXT){
		//モノクロテキストモード
		if(line_num==V_SYNC || line_num==V_SYNC+1)
		{
			//水平同期
			for (int j = 0; j < H_SYNC; j++) *b++ = 0;
			while (b < buf + NUM_LINE_SAMPLES) *b++ = 2;
		}
		else if(line_num>=V_SYNC+V_PREEQ && line_num<V_SYNC+V_PREEQ+Y_RES)
		{
			//映像領域信号生成
			b+=H_PICTURE;
			if (line_num == V_SYNC + V_PREEQ)
			{
				tvp = TVRAM;
				tline = 0;
				drawing = -1;
			}
			for(int i=0;i<WIDTH_XBW;i++)
			{
				uint8_t d=Fontp[*tvp *8 +tline];
				if(*(tvp+ATTROFFSETBW)&0x80) d=~d; // カラー0x80以上の場合反転
				for(int j=0;j<8;j++)
				{
					if(d & 0x80){
						*b=9; //white level
					}
					else{
						*b=2; //black level
					}
					b++;
					d<<=1;
				}
				tvp++;
			}
			tline++;
			if(tline<8) tvp-=WIDTH_XBW;
			else tline=0;
		}
	}
	else if(videomode==VMODE_IJTEXT){
		//IchigoJam 32x24文字 白黒モード (1文字横21サンプルx縦9ラインで画面いっぱいに表示)
		if(line_num==V_SYNC || line_num==V_SYNC+1)
		{
			//水平同期
			for (int j = 0; j < H_SYNC; j++) *b++ = 0;
			while (b < buf + NUM_LINE_SAMPLES) *b++ = 2;
		}
		else if(line_num>=V_SYNC+V_PREEQ && line_num<V_SYNC+V_PREEQ+IJ_HEIGHT)
		{
			//映像領域信号生成
			b+=IJ_PICTURE;
			if (line_num == V_SYNC + V_PREEQ)
			{
				tvp = TVRAM;
				tline = 0;
				trow = 0;
				drawing = -1;
			}
			int mag = 1, mshift = 0;//mag=1<<mshift (2の冪のみ、除算・剰余はシフトで行う)
			if (ntsc_mag == 2) { mag = 2; mshift = 1; }
			else if (ntsc_mag == 4) { mag = 4; mshift = 2; }
			else if (ntsc_mag == 8) { mag = 8; mshift = 3; }
			uint8_t fontline;
			if (mag == 1) {
				fontline = tline < 8 ? tline : 7; //8ラインフォントを9ラインに引き伸ばし
			} else {
				//拡大時: 1文字はmag×magセルに同じコードが複製されている
				//このセル (trow&mag-1: グループ内の行位置) が担当するフォント行を求める (各フォント行がmag走査線に引き伸ばされる)
				int z = ((trow & (mag - 1)) * IJ_LINES_PER_CHAR + tline) >> mshift;
				fontline = z > 7 ? 7 : z;
			}
			for(int i=0;i<IJ_WIDTH;i++)
			{
				uint8_t d=Fontp[*tvp *8 +fontline];
				uint8_t attr = *(tvp+IJ_ATTROFFSET);
				if (attr & IJ_ATTR_INVERT) d=~d; // VIDEO 2, 4 画面反転
				if (attr & IJ_ATTR_CURSOR) d ^= ntsc_cursor_mask; // カーソル (カーソル位置の文字をマスクXOR、DVIと同じ表示)
				if (mag == 1) {
					for(int j=0;j<8;j++)
					{
						uint16_t s = (d & 0x80) ? 9 : 2; //white level or black level
						*b++=s;
						*b++=s;
						//1文字21サンプルを8ドットに均等配分 (ドット幅2,3,2,3,3,2,3,3。字間なしで隙間なく連結)
						if(0x5B & (0x80 >> j)) *b++=s;
						d<<=1;
					}
				} else {
					//拡大時: このセルはグリフの一部 (8>>mshiftドット) を担当する。列位置 (i&mag-1) で担当部分を選び、1ドットを太く描く
					int ndots = 8 >> mshift;
					d <<= (i & (mag - 1)) * ndots;
					int dotw = 20 >> (3 - mshift); //1ドットのサンプル幅 (mag=2→5、4→10、8→20)
					uint16_t s_last = 2;
					for(int j=0;j<ndots;j++)
					{
						uint16_t s = (d & 0x80) ? 9 : 2;
						for(int k=0;k<dotw;k++) *b++=s;
						s_last = s;
						d<<=1;
					}
					*b++=s_last; //境界は最後のドットの色で延長し、縦線が入らないようにする (グリフ間も隙間なく連結)
				}
				tvp++;
			}
			tline++;
			if(tline<IJ_LINES_PER_CHAR) tvp-=IJ_WIDTH;
			else {
				tline=0;
				if (++trow >= IJ_ROWS) trow = 0;
			}
		}
	}
	else{
		//カラーモード
		if(line_num==V_SYNC || line_num==V_SYNC+1)
		{
			//水平同期＋バースト信号生成
			for (int j = 0; j < H_SYNC; j++) *b++ = 0;
			for (int j = 0; j < 8; j++) *b++ = 2;
			for (int j = 0; j < 9; j++)
			{
				*b++=2;
				*b++=1;
				*b++=2;
				*b++=3;
			}
			while (b < buf + NUM_LINE_SAMPLES) *b++ = 2;
		}
		else if(line_num>=V_SYNC+V_PREEQ && line_num<V_SYNC+V_PREEQ+Y_RES)
		{
			//映像領域信号生成
			b+=H_PICTURE;
			if (line_num == V_SYNC + V_PREEQ)
			{
				fbp = GVRAM;
				tvp = TVRAM;
				tline = 0;
				drawing = -1;
			}
			if(videomode==VMODE_WIDETEXT){
				//テキストモード
				uint32_t bc1=*((uint32_t*)(color_tbl+256*4));
				uint32_t bc2=*((uint32_t*)(color_tbl+256*4+2));
				for(int i=0;i<WIDTH_X;i++)
				{
					uint8_t d=Fontp[*tvp *8 +tline];
					uint16_t* clp=color_tbl+(*(tvp+ATTROFFSET))*4;
					uint32_t c1=*((uint32_t*)clp);
					uint32_t c2=*((uint32_t*)(clp+2));
					for(int j=0;j<4;j++)
					{
						if(d & 0x80){
							*((uint32_t*)b)=c1;
						}
						else{
							*((uint32_t*)b)=bc1;
						}
						b+=2;
						if(d & 0x40){
							*((uint32_t*)b)=c2;
						}
						else{
							*((uint32_t*)b)=bc2;
						}
						b+=2;
						d<<=2;
					}
					tvp++;
				}
			}
			else if(videomode==VMODE_WIDEGRPH){
				//グラフィック＋テキストモード
				for(int i=0;i<WIDTH_X;i++)
				{
					uint8_t d=Fontp[*tvp *8 +tline];
					uint16_t* clp=color_tbl+(*(tvp+ATTROFFSET))*4;
					uint32_t c1=*((uint32_t*)clp);
					uint32_t c2=*((uint32_t*)(clp+2));
					for(int j=0;j<4;j++)
					{
						uint16_t t=*(uint16_t*)fbp;
						if(d & 0x80){
							*((uint32_t*)b)=c1;
						}
						else{
							*((uint32_t*)b)=*((uint32_t*)(color_tbl+(t & 0xff)*4));
						}
						b+=2;
						if(d & 0x40){
							*((uint32_t*)b)=c2;
						}
						else{
							*((uint32_t*)b)=*((uint32_t*)(color_tbl+(t >> 8)*4+2));
						}
						b+=2;
						fbp+=2;
						d<<=2;
					}
					tvp++;
				}
			}
			tline++;
			if(tline<8) tvp-=WIDTH_X;
			else tline=0;
		}
	}
	if(line_num==V_SYNC+V_PREEQ+Y_RES || line_num==V_SYNC+V_PREEQ+Y_RES+1)
	{
		if(line_num==V_SYNC+V_PREEQ+Y_RES){
			drawing=0;
			drawcount++;
		}
		//映像領域信号消去
		b+=H_PICTURE;
		for(int i=0;i<X_RES*2;i++) *b++ = 2;
	}
}

void set_palette(uint16_t c,unsigned char b,unsigned char r,unsigned char g)
{
	// カラーパレット設定
	// c:パレット番号0-255、r,g,b:0-255
	// cが256のときバックグランドカラー

	// 輝度Y=0.587*G+0.114*B+0.299*R
	// 信号N=Y+0.4921*(B-Y)*sinθ+0.8773*(R-Y)*cosθ
	// 出力データS=N*(white-black)/255+black  white=9,black=2

	int32_t y,s;
	y=(150*g+29*b+77*r+128)/256;

	// 微妙な色ズレを修正
/*
	float th=30.0/180*3.14159;
	int32_t b_y_1=(int32_t)((b-y)*256*7*0.4921*sinf(0+th));
	int32_t r_y_1=(int32_t)((r-y)*256*7*0.8773*cosf(0+th));
	int32_t b_y_2=(int32_t)((b-y)*256*7*0.4921*sinf(3.14159/2+th));
	int32_t r_y_2=(int32_t)((r-y)*256*7*0.8773*cosf(3.14159/2+th));
*/
	int32_t b_y_1=(b-y)*441;
	int32_t r_y_1=(r-y)*1361;
	int32_t b_y_2=(b-y)*764;
	int32_t r_y_2=(r-y)*(-786);

	s=(y*1792 + b_y_1 + r_y_1 + 2*65536+32768)/65536;
	color_tbl[c*4] = s<0 ? 0 : s;
	s=(y*1792 + b_y_2 + r_y_2 + 2*65536+32768)/65536;
	color_tbl[c*4+1] = s<0 ? 0 : s;
	s=(y*1792 - b_y_1 - r_y_1 + 2*65536+32768)/65536;
	color_tbl[c*4+2] = s<0 ? 0 : s;
	s=(y*1792 - b_y_2 - r_y_2 + 2*65536+32768)/65536;
	color_tbl[c*4+3] = s<0 ? 0 : s;
}

static void init_palette(void){
	//カラーパレット初期化
	int i;
	for(i=0;i<8;i++){
		set_palette(i,255*(i&1),255*((i>>1)&1),255*(i>>2));
	}
	for(i=0;i<8;i++){
		set_palette(i+8,128*(i&1),128*((i>>1)&1),128*(i>>2));
	}
	for(i=16;i<256;i++){
		set_palette(i,255,255,255);
	}
	set_palette(256,0,0,0); //バックグランドカラー設定
}

static void irq_handler(void) {
	volatile uint32_t s1;

#if defined ( PIN_DEBUG_BUSY )
	gpio_put(PIN_DEBUG_BUSY, 1);
#endif
	s1=dma_hw->ints1;
	dma_hw->ints1 = s1;
	if(s1 & (1u << pwm_dma_chan1)){
		//DVIはIRQ0、NTSCはIRQ1を使う (DMA_IRQ_0はcore1のDVIハンドラが使っているため)
		//両方の完了ビットが立っているときは割り込みが合流したので、
		//更新できなかったch0側の1ライン分もscanlineを進めて表示の位相を保つ
		makeDmaBuffer(dma_buffer[1], scanline);
		dma_channel_set_read_addr(pwm_dma_chan1, dma_buffer[1], false);
		if(s1 & (1u << pwm_dma_chan0)) scanline++;
	}
	else{
		makeDmaBuffer(dma_buffer[0], scanline);
		dma_channel_set_read_addr(pwm_dma_chan0, dma_buffer[0], false);
	}
	if (++scanline >= NUM_LINES) {
		scanline = 0;
	}
	if (ntsc_line_callback) {
		ntsc_line_callback(); // IchigoJamのWAIT -n (TICK走査線) 用
	}
#if defined ( PIN_DEBUG_BUSY )
	gpio_put(PIN_DEBUG_BUSY, 0);
#endif
}

// グラフィック画面クリア
void g_clearscreen(void)
{
	// グラフィックメモリが確保されていない場合メッセージを表示して停止
	if(GVRAM==0){
		uint8_t message[]="Not allocated graphic memory";
		uint8_t *p=TVRAM, *s=message;
		while(*s){
			*(p+attroffset)=7;
			*p++=*s++;
		}
		while(true) asm("wfi");
	}
	unsigned int *vp=(unsigned int *)GVRAM;
	for(int i=0;i<X_RES*Y_RES/4;i++) *vp++=0;
}
//テキスト画面クリア
void clearscreen(void)
{
	unsigned int *vp;
	int i;
	vp=(unsigned int *)TVRAM;
	for(i=0;i<WIDTH_XMAX*WIDTH_Y*2/4;i++) *vp++=0;
}

void set_dma_channel_chain(uint ch,uint ch_to){
	uint32_t a;
	a=dma_channel_hw_addr(ch)->al1_ctrl;
	a=(a & ~DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS) | (ch_to << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB);
	dma_channel_hw_addr(ch)->al1_ctrl=a;
}
//ビデオ出力開始
void start_composite(void)
{
	scanline=0;
	videostop=0;
/*
	makeDmaBuffer(dma_buffer[0], 0);
	makeDmaBuffer(dma_buffer[1], 1);
	dma_set_irq0_channel_mask_enabled((1u<<pwm_dma_chan0)|(1u<<pwm_dma_chan1),true);
	set_dma_channel_chain(pwm_dma_chan0, pwm_dma_chan1);
	set_dma_channel_chain(pwm_dma_chan1, pwm_dma_chan0);
	dma_channel_start(pwm_dma_chan0);
*/
}
//ビデオ出力停止
void stop_composite(void)
{
	videostop=1;
/*
	dma_set_irq0_channel_mask_enabled((1u<<pwm_dma_chan0)|(1u<<pwm_dma_chan1),false);
	set_dma_channel_chain(pwm_dma_chan0, pwm_dma_chan0);
	set_dma_channel_chain(pwm_dma_chan1, pwm_dma_chan1);
	dma_channel_abort(pwm_dma_chan0);
	dma_channel_abort(pwm_dma_chan1);
*/
}

//ビデオ出力システムを完全に解放する (SWITCHでDVIに戻すときに使用)
//DMAチャンネル・割込み・PWMを解放する
void ntsc_release(void)
{
	videostop=1;
	dma_set_irq1_channel_mask_enabled((1u<<pwm_dma_chan0)|(1u<<pwm_dma_chan1),false);
	set_dma_channel_chain(pwm_dma_chan0, pwm_dma_chan0);
	set_dma_channel_chain(pwm_dma_chan1, pwm_dma_chan1);
	dma_channel_abort(pwm_dma_chan0);
	dma_channel_abort(pwm_dma_chan1);
	dma_channel_unclaim(pwm_dma_chan0);
	dma_channel_unclaim(pwm_dma_chan1);
	irq_set_enabled(DMA_IRQ_1, false);
	irq_remove_handler(DMA_IRQ_1, irq_handler);
	pwm_set_enabled(pwm_slice_num, false);
	gpio_set_function(ntsc_pin, GPIO_FUNC_NULL);
}

//ビデオ出力システムを初期化し開始する
//n:信号出力するGPIO番号
void rp2040_pwm_ntsc_init(uint8_t n)
{
#if defined ( PIN_DEBUG_BUSY )
	gpio_init(PIN_DEBUG_BUSY);
	gpio_set_dir(PIN_DEBUG_BUSY, GPIO_OUT);
#endif
	//ビデオモード初期化
	videomode=VMODE_WIDETEXT;
	textmode=TMODE_WIDETEXT;
	graphmode=GMODE_NOGRPH;
	twidth=WIDTH_X;
	twidthy=WIDTH_Y;
	attroffset=ATTROFFSET;

	init_palette();
	clearscreen();
	//Fontpはアプリ側で設定する (IchigoJamのフォントをコピーしたRAMフォント)

	// CPUを157.5MHzで動作させる
	uint32_t freq_khz = 157500;

	// PWM周期を11サイクルとする (157.5 [MHz] / 11 = 14318181 [Hz])
	uint32_t pwm_div = 11;

	// ※ NTSCのカラー信号を1周期4サンプルで出力する。
	// 出力されるカラーバースト信号は  14318181 [Hz] / 4 = 3579545 [Hz] となる。

	set_sys_clock_khz(freq_khz, true);

	ntsc_dma_buf.canary1 = 0xD1A6B001;//診断用カナリア
	ntsc_dma_buf.canary2 = 0xD1A6B002;

	ntsc_pin = n;
	gpio_set_function(n, GPIO_FUNC_PWM);
	pwm_slice_num = pwm_gpio_to_slice_num(n);

	pwm_config config = pwm_get_default_config();
	pwm_config_set_clkdiv(&config, 1);

	pwm_init(pwm_slice_num, &config, true);
	pwm_set_wrap(pwm_slice_num, pwm_div - 1);

	volatile void* wr_addr = &pwm_hw->slice[pwm_slice_num].cc;
	wr_addr = (volatile void*)(((uintptr_t)wr_addr) + 2);

	pwm_dma_chan0 = dma_claim_unused_channel(true);
	pwm_dma_chan1 = dma_claim_unused_channel(true);

	dma_channel_config c = dma_channel_get_default_config(pwm_dma_chan0);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, DREQ_PWM_WRAP0 + pwm_slice_num);
	channel_config_set_chain_to(&c, pwm_dma_chan1);
	dma_channel_configure(
		pwm_dma_chan0,
		&c,
		wr_addr,
		dma_buffer[0],
		NUM_LINE_SAMPLES,
		false
	);

	c = dma_channel_get_default_config(pwm_dma_chan1);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, DREQ_PWM_WRAP0 + pwm_slice_num);
	channel_config_set_chain_to(&c, pwm_dma_chan0);
	dma_channel_configure(
		pwm_dma_chan1,
		&c,
		wr_addr,
		dma_buffer[1],
		NUM_LINE_SAMPLES,
		false
	);
	scanline=0;
	videostop=0;
	makeDmaBuffer(dma_buffer[0], 0);
	makeDmaBuffer(dma_buffer[1], 1);
	dma_set_irq1_channel_mask_enabled((1u<<pwm_dma_chan0)|(1u<<pwm_dma_chan1),true);
	irq_set_exclusive_handler(DMA_IRQ_1, irq_handler);
	irq_set_enabled(DMA_IRQ_1, true);
	//最高優先度にする (他の割り込みはデフォルト優先度2)
	//60Hzタイマー(tuh_task等)やUSB割り込みより優先しないと割り込みが合流し、
	//走査線バッファの更新が1ライン分飛んで垂直同期がずれる(画面が流れる)
	irq_set_priority(DMA_IRQ_1, 0);
	dma_channel_start(pwm_dma_chan0);
}

//ビデオモードの切り替え
// m:ビデオモード
// gvram:グラフィック用メモリ先頭アドレス
void set_videomode(unsigned char m, unsigned char *gvram){

	unsigned int *fontROMp,*fontRAMp;
	unsigned int i;

	if(videomode==m) return;
	stop_composite();
	switch(m){
		case VMODE_WIDETEXT: // ワイドテキスト48文字モード
			if(textmode!=TMODE_WIDETEXT){
				textmode=TMODE_WIDETEXT;
				twidth=WIDTH_X;
				attroffset=ATTROFFSET;
				clearscreen();
			}
			break;
		case VMODE_MONOTEXT: // モノクロテキスト80文字モード
			if(textmode!=TMODE_MONOTEXT){
				textmode=TMODE_MONOTEXT;
				twidth=WIDTH_XBW;
				attroffset=ATTROFFSETBW;
				clearscreen();
			}
			break;
		case VMODE_IJTEXT: // IchigoJam 32x24文字 白黒モード
			twidth=IJ_WIDTH;
			twidthy=IJ_ROWS;
			attroffset=IJ_ATTROFFSET;
			clearscreen();
			break;
		case VMODE_WIDEGRPH: // ワイドグラフィック＋テキスト48文字モード
			graphmode=GMODE_WIDEGRPH;
			if(textmode!=TMODE_WIDETEXT){
				textmode=TMODE_WIDETEXT;
				twidth=WIDTH_X;
				attroffset=ATTROFFSET;
				clearscreen();
			}
			break;
	}
	videomode=m;
	if(m>=16){
		// グラフィック使用モード
		GVRAM=gvram;
		g_clearscreen();
	}
	else{
		// グラフィック不使用モード
		graphmode=0;
	}
	start_composite();
}
