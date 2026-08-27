#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <c/data.h>
#include "../_opendev/RGB-LCD.hpp"// LTDC + SDRAM 共享初始化（RGB-LCD.cpp / SDRAM.cpp 已加入工程）
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];
GPIN& KEYU = GPIOA[ 0];//   Up   (WK_UP)
GPIN& KEYL = GPIOC[13];// Left  (KEY2)
GPIN& KEYD = GPIOH[ 2];// Down  (KEY1)
GPIN& KEYR = GPIOH[ 3];// Right (KEY0)

char _buf[64]; String buf(_buf, byteof(_buf));

extern "C" {
char* StrHeap(const char* valit_str){ (void)valit_str; return nullptr; }
char* StrHeapAppendChars(char* dest, char chr, size_t n){(void)dest; (void)chr; (void)n; return nullptr; }
char* salc(size_t size){ return 0; }
void outtxt(const char* str, stduint len) {XART1.out(str, len);}
}

// 对应正点原子实验15 LTDC LCD实验（HAL库版）
// 4.3寸 RGB 屏 800x480（面板 ID 0x4384）；帧缓冲放外部 SDRAM
// 说明：SDRAM / LTDC 初始化改用共享抽象 _opendev/RGB-LCD.hpp：
//       sdram_init() / ltdc_init() / ltdc_text() 由 RGB-LCD.cpp + SDRAM.cpp 提供

// 12 种背景色循环（照实验15 main 的 12 色）
static const Color bg_colors[] = {
	Color::FromRGB888(0xFFFFFF),// WHITE
	Color::FromRGB888(0x000000),// BLACK
	Color::FromRGB888(0x0000FF),// BLUE
	Color::FromRGB888(0xFF0000),// RED
	Color::FromRGB888(0xFF00FF),// MAGENTA
	Color::FromRGB888(0x00FF00),// GREEN
	Color::FromRGB888(0x00FFFF),// CYAN
	Color::FromRGB888(0xFFFF00),// YELLOW
	Color::FromRGB888(0xBB0000),// BRRED（棕红）
	Color::FromRGB888(0x808080),// GRAY
	Color::FromRGB888(0xC0C0C0),// LGRAY
	Color::FromRGB888(0xA52A2A),// BROWN
};

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;// DS0
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// WK_UP (PA0)
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);// KEY2 (PC13)
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);// KEY1 (PH2)
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);// KEY0 (PH3)

	XART1.setMode(115200);
	XART1.OutFormat("APOLLO STM32H7\r\n");
	XART1.OutFormat("LTDC TEST\r\n");

	sdram_init(); // 共享抽象：初始化 SDRAM（帧缓冲位于 0xC0000000）
	ltdc_init();  // 共享抽象：初始化 LTDC（引脚 + PLL3 像素时钟 + 时序 + 层）

	XART1.OutFormat("LTDC OK, PixClk=%dHz\r\n", LTDC.getFrequency());

	byte x = 0;
	while (true) {
		// 清屏为当前背景色（照实验15 12 色循环）
		LTDC[1].DrawRectangle(GrafRect(0, 0, 800, 480, bg_colors[x]));
		// 显示文本（照实验15 main 的几行）
		ltdc_text(10, 40, "STM32H7");
		ltdc_text(10, 80, "Alice.Wiki 20-LTDC TEST");
		ltdc_text(10, 110, "@ArinaMgk");
		// ltdc_text(10, 130, "LCD ID:4384");
		ltdc_text(10, 150, __DATE__);
		x++;
		if (x == 12) x = 0;
		LEDB.Toggle();// DS0 闪烁指示运行
		SysDelay_ms(1000);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
