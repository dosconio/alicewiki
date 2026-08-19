#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/LTDC>
#include <cpp/Device/FMC>
#include <c/data.h>
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
// 4.3寸 RGB 屏 800x480（面板 ID 0x4384）；帧缓冲放外部 SDRAM（照 16-SDRAM 例程 FMC_SDRAM_BANK1_BASE）
#define LCD_FRAME_BUF_ADDR FMC_SDRAM_BANK1_BASE

// SDRAM 模式寄存器值（照 16-SDRAM 例程，源自实验14）
#define SDRAM_MODEREG_BURST_LENGTH_1          ((uint16)0x0000)
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL   ((uint16)0x0000)
#define SDRAM_MODEREG_CAS_LATENCY_2           ((uint16)0x0020)
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD ((uint16)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE  ((uint16)0x0200)

// FMC SDRAM 引脚复用（照 16-SDRAM 例程：PC/PD/PE/PF/PG 全部 AF12）
static void sdram_gpio_raw(stduint base, const byte* pins, byte n) {
	Reference moder(base + 0x00), otyper(base + 0x04), speed(base + 0x08);
	Reference pupdr(base + 0x0C), afrl(base + 0x20), afrh(base + 0x24);
	for (byte i = 0; i < n; i++) {
		byte p = pins[i];
		moder.maset(p << 1, 2, 2);                 // AF 模式
		otyper.rstof(p);                           // 推挽
		speed.maset(p << 1, 2, 3);                 // Veryhigh
		pupdr.maset(p << 1, 2, 1);                 // 上拉
		(p < 8 ? afrl : afrh).maset((p & 7) << 2, 4, 12);  // AF12
	}
}

static void sdram_gpio_config() {
	// 使能 GPIO C/D/E/F/G 时钟（AHB4ENR bits 2-6）
	for (byte port = 2; port <= 6; port++)
		RCC[RCCReg::AHB4ENR].setof(port, true);
	static const byte pc[] = {0,2,3}, pd[] = {0,1,8,9,10,14,15}, pe[] = {0,1,7,8,9,10,11,12,13,14,15},
		pf[] = {0,1,2,3,4,5,11,12,13,14,15}, pg[] = {0,1,2,4,5,8,15};
	sdram_gpio_raw(0x58020800, pc, byteof(pc));  // GPIOC
	sdram_gpio_raw(0x58020C00, pd, byteof(pd));  // GPIOD
	sdram_gpio_raw(0x58021000, pe, byteof(pe));  // GPIOE
	sdram_gpio_raw(0x58021400, pf, byteof(pf));  // GPIOF
	sdram_gpio_raw(0x58021800, pg, byteof(pg));  // GPIOG
}

// SDRAM 初始化（照 16-SDRAM 例程 sdram_init：setMode + 时钟使能/预充电/自动刷新/模式寄存器/刷新率）
static void sdram_init() {
	sdram_gpio_config();// FMC 引脚复用（HAL_SDRAM_MspInit 部分）

	SDRAMInit ini;
	ini.bank = SDRAMBank::Bank1;
	ini.column = SDRAMColumn::C9;
	ini.row = SDRAMRow::R13;
	ini.dataWidth = SDRAMDataWidth::W16;
	ini.bankNum = SDRAMBankNum::Four;
	ini.cas = SDRAMCas::CL2;
	ini.clockPeriod = SDRAMClock::C2;
	ini.readBurst = true;
	SDRAMTiming tim;
	tim.loadToActive = 2;
	tim.exitSelfRefresh = 8;
	tim.selfRefreshTime = 6;
	tim.rowCycle = 7;// TRC 6→7：60ns→70ns ≥ W9825G6KH tRC min 65ns
	tim.writeRecovery = 2;
	tim.rpDelay = 2;
	tim.rcdDelay = 2;
	FMC.SDRAM.setMode(ini, tim);

	SDRAMCommand cmd;
	cmd.target = SDRAMTarget::Bank1;
	// 1) 时钟配置使能（随后至少延时 200us）
	cmd.mode = SDRAMCmd::ClockConfigEnable;
	cmd.autoRefreshNumber = 1;
	cmd.modeRegister = 0;
	FMC.SDRAM.setCommand(cmd);
	SysDelay_us(500);
	// 2) 预充电所有 bank
	cmd.mode = SDRAMCmd::PALL;
	FMC.SDRAM.setCommand(cmd);
	// 3) 自动刷新 8 次
	cmd.mode = SDRAMCmd::AutoRefresh;
	cmd.autoRefreshNumber = 8;
	FMC.SDRAM.setCommand(cmd);
	// 4) 配置模式寄存器
	cmd.mode = SDRAMCmd::LoadMode;
	cmd.autoRefreshNumber = 1;
	cmd.modeRegister = SDRAM_MODEREG_BURST_LENGTH_1 | SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL |
		SDRAM_MODEREG_CAS_LATENCY_2 | SDRAM_MODEREG_OPERATING_MODE_STANDARD | SDRAM_MODEREG_WRITEBURST_MODE_SINGLE;
	FMC.SDRAM.setCommand(cmd);
	// 5) 刷新率：COUNT = 64*1000*100/8192 - 20 = 761
	FMC.SDRAM.setRefreshRate(761);
}

// ---- LTDC 引脚（照实验15 HAL_LTDC_MspInit：LTDC 信号全部 AF14，背光 PB5）----
static void ltdc_gpio_config() {
	auto spd = GPIOSpeed::Veryhigh;
	// 背光 PB5：推挽输出 + 上拉，点亮
	GPIOB[5].setMode(GPIOMode::OUT_PushPull, spd).setPull(true);
	GPIOB[5] = true;
	// LTDC 数据/同步/时钟引脚（AF14）
	GPIOF[10].setMode(GPIOMode::OUT_AF_PushPull, spd)._set_alternate(14);
	GPIOG[6].setMode(GPIOMode::OUT_AF_PushPull, spd)._set_alternate(14);
	GPIOG[7].setMode(GPIOMode::OUT_AF_PushPull, spd)._set_alternate(14);
	GPIOG[11].setMode(GPIOMode::OUT_AF_PushPull, spd)._set_alternate(14);
	for (byte i = 9; i <= 15; i++)
		GPIOH[i].setMode(GPIOMode::OUT_AF_PushPull, spd)._set_alternate(14);
	static const byte PI_LTDC[] = { 0, 1, 2, 4, 5, 6, 7, 9, 10 };
	for0a(i, PI_LTDC)
		GPIOI[PI_LTDC[i]].setMode(GPIOMode::OUT_AF_PushPull, spd)._set_alternate(14);
}

// ---- LTDC 像素时钟（照实验15 LTDC_Clk_Set(5,160,24)：PLL3.R = 33MHz）----
// HSE=25MHz：VCO = 25/5*160 = 800MHz，R = 800/24 ≈ 33.3MHz
static void ltdc_clock_config() {
	RCC[RCCReg::CR].setof(28, false);            // PLL3ON = 0
	RCC[RCCReg::PLLCKSELR].maset(20, 6, 5);      // DIVM3 = 5（PLLSRC 已由 RCC.setClock 置为 HSE）
	RCC[RCCReg::PLL3DIVR] = (160U - 1U) | ((2U - 1U) << 9U) | ((2U - 1U) << 16U) | ((24U - 1U) << 24U);// N3 P3 Q3 R3
	RCC[RCCReg::PLLCFGR].maset(10, 2, 2);        // PLL3RGE = 2（输入 4~8MHz：25/5=5MHz）
	RCC[RCCReg::PLLCFGR].rstof(9);               // PLL3VCOSEL = 0（WIDE）
	RCC[RCCReg::PLLCFGR].rstof(8);               // PLL3FRACEN = 0（整数模式）
	RCC[RCCReg::PLLCFGR].setof(22);              // DIVP3EN
	RCC[RCCReg::PLLCFGR].setof(23);              // DIVQ3EN
	RCC[RCCReg::PLLCFGR].setof(24);              // DIVR3EN
	RCC[RCCReg::CR].setof(28, true);             // PLL3ON = 1
	while (!RCC[RCCReg::CR].bitof(29));          // 等待 PLL3RDY
}

// ---- LTDC 初始化（时序照实验15 面板 ID 0x4384；层配置照 mecocoa loader 的 unisym 用法）----
static void ltdc_init() {
	ltdc_gpio_config();
	ltdc_clock_config();
	// 800x480：hsw48 hbp88 hfp40 / vsw3 vbp32 vfp13
	auto& h = LTDC.refHorizontal();
	h.sync_len = 48; h.back_porch = 88; h.active_len = 800; h.front_porch = 40;
	auto& v = LTDC.refVertical();
	v.sync_len = 3; v.back_porch = 32; v.active_len = 480; v.front_porch = 13;
	LTDC.setMode(Color::Black);// 等价 HAL_LTDC_Init（内部含 enClock）
	// 层：帧缓冲在 SDRAM，RGB565
	LTDC_LAYER_t::LayerPara lpara{};
	LTDC_LAYER_t::layer_param_refer(&lpara);
	lpara.roleaddr = (pureptr_t)LCD_FRAME_BUF_ADDR;
	LTDC[1].setMode(lpara);// 等价 HAL_LTDC_ConfigLayer
}

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

// 文本绘制：VideoControlInterface 的用户实现（用内置 16x8 点阵字体，照 _TextChrome.cpp 的取模方式）
void LTDC_LAYER_t::DrawFont(const Point& disp, const DisplayFont& font, const String& str) const {
	const char* p = str.reference();
	stduint x = disp.x, y = disp.y;
	Color fg = font.forecolor;
	while (*p) {
		char ch = *p++;
		if (ch < 32 || ch > 126) ch = '?';
		ch -= 0x20;
		const uint16* datptr = (const uint16*)&_BITFONT_ASCII_16x8[(byte)ch];
		for (byte col = 0; col < 8; col++) {
			uint16 dat = datptr[col];
			for (byte row = 0; row < 16; row++) {
				if (dat & _IMM1S(row)) DrawPoint(Point(x + col, y + (row ^ 0b111)), fg);
			}
		}
		x += 8;
	}
}

// 在指定位置输出一行文本（16x8 点阵）
static void ltdc_text(stduint x, stduint y, const char* s) {
	char _tb[64]; String ts(_tb, byteof(_tb));
	ts = s;
	DisplayFont f{ nullptr, Color::Red, Size2(8, 16) };
	LTDC[1].DrawFont(Point(x, y), f, ts);
}

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

	sdram_init(); // 初始化 SDRAM（帧缓冲位于 0xC0000000）
	ltdc_init();  // 初始化 LTDC（引脚 + PLL3 像素时钟 + 时序 + 层）

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
