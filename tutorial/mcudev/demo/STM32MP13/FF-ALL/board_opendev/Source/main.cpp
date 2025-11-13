// ASCII CPP TAB4 CRLF
// @dosconio 2024

#define BOARD_DETAIL "5DAE7" // STM32MP135-DAE7
#include <cpp/MCU/ST/STM32MP13>
#include <cpp/string>
#include "metutor.h"

#define setAF(x) setMode(GPIOMode::OUT_AF_PushPull, spd)._set_alternate(x)

bool exist_ddr = true;
bool exist_cache = true;

void hand() { LED.Toggle(); }

bool init_ddr();
bool init() {
	auto spd = GPIOSpeed::Veryhigh;
	if (!init_specific() || !init_clock()) return false;
	// LED
	LED.setMode(GPIOMode::OUT_PushPull);
	// DDR
	if (exist_ddr && !init_ddr()) return false;
	// LCD
	LCD_BL.setMode(GPIOMode::OUT_PushPull).setPull(true);
	LCD_DE.setAF(11);
	LCD_CLK.setAF(13);
	LCD_HSYNC.setAF(13);
	LCD_VSYNC.setAF(11);
	for0a(i, LCDR) LCDR[i]->setAF(LCDR_AF[i]);
	for0a(i, LCDG) LCDG[i]->setAF(LCDG_AF[i]);
	for0a(i, LCDB) LCDB[i]->setAF(LCDB_AF[i]);
	LCD_BL = true;
	// default polarity D'DP and LTDC_PCPOLARITY_IPC
	auto& hpara = LTDC.refHorizontal(); {
		hpara.active_len = 800;
		hpara.back_porch = 88;
		hpara.front_porch = 40;
		hpara.sync_len = 48;
	}
	auto& vpara = LTDC.refVertical(); {
		vpara.active_len = 480;
		vpara.back_porch = 32;
		vpara.front_porch = 13;
		vpara.sync_len = 3;
	}
	if (LTDC.getFrequency() != 33e6) return false;// 33MHz
	LTDC.setMode(Color::Black);
	{
		LTDC_LAYER_t::LayerPara lpara;
		LTDC_LAYER_t::layer_param_refer(&lpara);
		lpara.roleaddr;
		asrtret(LTDC[0].setMode(lpara));
	}
	// EXTI
	GPIOA[3].setMode(GPIORupt::Anyedge);// USART2_RX
	GPIOG[10].setMode(GPIOMode::OUT_PushPull);// FDCAN1_TX
	GPIOA[3].setInterrupt(hand);
	GPIOI[0].setMode(GPIOMode::INN);
	GPIOI[2].setMode(GPIOMode::OUT) = 1;
	// SDCard
	asrtret(SDCard1.setMode());

	return true;
}

_ESYM_C void f_1();
_ESYM_C void f_2();_ESYM_C void f_3();_ESYM_C void f_4();
//_ESYM_C void f_0(){ static int pp = 0; VConsole.OutFormat("(%d)", pp++); }
//_ESYM_C void f_read_selfdef();
extern "C" byte local_out_lock;
extern "C" int  outsfmt0(const char* fmt, ...) {
	local_out_lock = 0;
	Letpara(args, fmt);
	uint32 a = para_next(args, uint32);
	uint32 b = para_next(args, uint32);
	VConsole.OutFormat(fmt, a, b);
}

extern "C" {
extern uint32_t* Buffer0;
}

fn main() -> int {
	if (!init()) loop;
	GPIOA[3].enInterrupt();
	//GPIOI[0].enInterrupt();
	Circle circ(Point(200,200), 200);
	Rectangle scrn_rect(Point(0, 0), Size2(800, 480), Color::AliceBlue);
	LCD.Draw(scrn_rect);
	VConsole.OutFormat("Ciallo %[32H]", 0x4567);

	{

		SDCard1.Read(0x500, Buffer0);
		SDCard1.Read(0x501, (char*)Buffer0 + 512);
		outsfmt0(" --- ", 0, 0);
		for(int i=0; i< 16; i++) outsfmt0(" {%x}", *((char*)Buffer0 + i), 0);
		outsfmt0(" - ", 0, 0);
		for(int i=512; i< 520; i++) outsfmt0(" {%x}", *((char*)Buffer0 + i), 0);
		// write blk1 and then read to blk0
		for0(i,512) *((char*)Buffer0 + 512 + i) = i;
		SDCard1.Write(0x666, (char*)Buffer0 + 512);
		SDCard1.Read(0x666, Buffer0);
		outsfmt0(" --- ", 0, 0);
		for(int i=0; i< 512; i++) outsfmt0(" {%x}", *((char*)Buffer0 + i), 0);
	}
	loop {
		static unsigned k = 10;
		LED.Toggle();
		SysDelay(250);
	}
}

// Global Data
VideoControlBlock LCD = LTDC[1].getControlBlock();
VideoConsole VConsole(LTDC[1], Size2(80, 25));

void LTDC_LAYER_t::DrawFont(const Point& disp, const DisplayFont& font) const {}
void outtxt(const char* str, stduint len) {str; len;}
extern "C" { void erro(void) { loop{ LED.Toggle(); SysDelay(2000); } } }
