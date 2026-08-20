#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/LTDC>
#include <cpp/Device/FMC>
#include <cpp/Device/GPU>
#include <c/data.h>
#include "../../../device/GT9147.h"
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// 画板为累积式绘制（笔迹要一直保留），用单缓冲直接写显示帧缓冲（照 HAL ctp_test）。
// 若用双缓冲，标题/笔迹会分布在两个缓冲上、换帧时交替闪现 → 闪烁。
#define LCD_DOUBLE_BUFFER 0


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

// 对应正点原子实验31 触摸屏实验（HAL库版）+ 实验15 LTDC
// 4.3寸 RGB 屏 800x480（面板 ID 0x4384，GT9147 电容触摸）；帧缓冲放外部 SDRAM
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

// 【双缓冲方案】LCD_DOUBLE_BUFFER=1：渲染到后台缓冲，等垂直消隐（当前行进入消隐区）再
// 切换层地址（CFBAR + SRCR.VBR），画面在帧边界整帧切换，无撕裂。
#define LCD_FB_STRIDE 800                        // 帧缓冲行宽（像素）
#define LCD_FB_LINES  480                        // 帧缓冲行数
#define LCD_FB_SIZE   (LCD_FB_STRIDE * LCD_FB_LINES * 2)// RGB565：每像素 2 字节
#define LCD_FB0_ADDR  LCD_FRAME_BUF_ADDR         // 缓冲 0（LTDC 初始层地址）
#define LCD_FB1_ADDR  (LCD_FRAME_BUF_ADDR + LCD_FB_SIZE)// 缓冲 1
#define BLOCK_SIZE 64                            // M2M 色块边长（像素，保留占位）

// 在指定帧缓冲地址绘制一行文本（16x8 点阵，RGB565；直接写目标缓冲，与 LTDC 层地址无关，
// 双缓冲时文本必须画到后台缓冲，否则会画到正在显示的缓冲上）
static void ltdc_text_fb(pureptr_t fb, stduint x, stduint y, const char* s) {
	const char* p = s;
	Color fg = Color::Red;
	while (*p) {
		char ch = *p++;
		if (ch < 32 || ch > 126) ch = '?';
		ch -= 0x20;
		const uint16* datptr = (const uint16*)&_BITFONT_ASCII_16x8[(byte)ch];
		for (byte col = 0; col < 8; col++) {
			uint16 dat = datptr[col];
			for (byte row = 0; row < 16; row++) {
				if (dat & _IMM1S(row)) {
					stduint px = x + col, py = y + (row ^ 0b111);
					if (px < LCD_FB_STRIDE && py < LCD_FB_LINES)
						((uint16*)fb)[py * LCD_FB_STRIDE + px] = fg.ToRGB565();
				}
			}
		}
		x += 8;
	}
}

#if LCD_DOUBLE_BUFFER
static pureptr_t _fb_front = (pureptr_t)LCD_FB0_ADDR;// 当前 LTDC 显示缓冲
static pureptr_t _fb_back  = (pureptr_t)LCD_FB1_ADDR;// 当前后台渲染缓冲
// 渲染完成后调用：等待垂直消隐（当前行 ≥ 有效行数）再切换层地址，无撕裂
static void lcd_swap() {
	while (LTDC[LTDCReg::CPSR].masof(0, 16) < LCD_FB_LINES);// 等待进入消隐区
	LTDC[1][LTDCLayerReg::CFBAR] = _IMM(_fb_back);          // 层地址指向刚渲染完的后台缓冲
	LTDC.Reload(LTDCReload::VerticalBlank);                  // 下次垂直同步时生效
	pureptr_t _t = _fb_front; _fb_front = _fb_back; _fb_back = _t;// 前后台角色互换
}
#else
#define _fb_back ((pureptr_t)LCD_FB0_ADDR)// 单缓冲：渲染目标就是显示缓冲
static void lcd_swap() {}
#endif

// 软件 I2C 位延时（照 10-IIC-OLED 例程；比 HAL 的 delay_us(2) 放慢，给内部上拉留足上升时间）
void iic_delay() { for(volatile int i = 0; i < 500; i++){} }

// 触摸专用软件 I2C：强制 push_pull 模式（照 HAL ctiic 的 SDA 输入/输出切换）。
// 修复 IIC_SOFT 开漏模式下 SendAcknowledge(ACK) 后 SDA 被主机拉低不释放、
// 导致连续读字节变 0 的问题。
class TouchIIC : public uni::IIC_SOFT {
public:
	TouchIIC(uni::GPIN& sda, uni::GPIN& scl) : uni::IIC_SOFT(sda, scl, false) {
		push_pull = true;
		setMode();
	}
};

// 5 指颜色（照 HAL lcd.h POINT_COLOR_TBL：RED GREEN BLUE BROWN GRED）
static const uint16 touch_colors[5] = { 0xF800, 0x07E0, 0x001F, 0xBC40, 0xFFE0 };

// 在帧缓冲上画 2x2 粗点（照 HAL lcd_draw_bline size=2 的简化）
static void ltdc_dot2_fb(pureptr_t fb, stduint x, stduint y, uint16 c) {
	if (x >= LCD_FB_STRIDE || y >= LCD_FB_LINES) return;
	uint16* p = (uint16*)fb + y * LCD_FB_STRIDE + x;
	*p = c;
	if (x + 1 < LCD_FB_STRIDE) p[1] = c;
	if (y + 1 < LCD_FB_LINES) {
		p[LCD_FB_STRIDE] = c;
		if (x + 1 < LCD_FB_STRIDE) p[LCD_FB_STRIDE + 1] = c;
	}
}

// 在帧缓冲上画粗线（Bresenham，每步 2x2 点，照 HAL lcd_draw_bline）
static void ltdc_line_fb(pureptr_t fb, stduint x0, stduint y0, stduint x1, stduint y1, uint16 c) {
	stdsint dx = x1 > x0 ? (stdsint)x1 - x0 : (stdsint)x0 - x1;
	stdsint dy = y1 > y0 ? (stdsint)y1 - y0 : (stdsint)y0 - y1;
	stdsint sx = x0 < x1 ? 1 : -1;
	stdsint sy = y0 < y1 ? 1 : -1;
	stdsint err = dx - dy;
	while (true) {
		ltdc_dot2_fb(fb, x0, y0, c);
		if (x0 == x1 && y0 == y1) break;
		stdsint e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

// DMA2D R2M 整屏填充（白底清屏用）
static void ltdc_fill_white(pureptr_t fb) {
	DMA2D.setMode(DMA2DMode::R2M, PixelFormat::RGB565);
	DMA2D.Transfer((pureptr_t)uint32(Color::White), fb, 800, 480);
}

// 【预期现象】（APOLLO STM32H7 + 4.3寸 800x480 RGB 电容屏）
// ① 上电后屏幕显示白底 + 红字标题（STM32H7 / Alice.Wiki 20-LTDC-DMA2D-Touch /
//    @ArinaMgk / Touch Paint Board / RST corner to clear），稳定不闪烁。
// ② 串口依次打印：APOLLO STM32H7 → LTDC-DMA2D-TOUCH TEST → SDRAM OK →
//    LTDC OK, PixClk=33333334Hz → TP init... → GT9147 OK；之后每帧打印触摸坐标 X:..,Y:..
// ③ 手指在屏上滑动画出彩色笔迹并保留（最多 5 指，各指一色：红/绿/蓝/棕/GRED）。
// ④ 点击屏幕右上角约 24x20 的 RST 区，整屏清为白底。
// ⑤ DS0（LEDB）闪烁指示程序运行。
// 说明：
//   - 触摸芯片兼容 GT911 / GT9147 / GT1158 / GT9271（照 HAL，本板实测为 GT1158）。
//   - 单缓冲直写，快速画线时偶有轻微撕裂属正常现象（HAL 同款方案）。
//   - 若串口停在 TP init FAIL，按打印的 ACK_stage / PID 排查触摸 I2C。

int main() {
	L1C.enAbleICacheAll();// 只开 I-Cache；D-Cache 关闭，CPU 写帧缓冲直达 SDRAM，LTDC 立即可见
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
	XART1.OutFormat("LTDC-DMA2D-TOUCH TEST\r\n");

	sdram_init(); // 初始化 SDRAM（帧缓冲位于 0xC0000000）
	// 快速验证 SDRAM 读写（排除"SDRAM 没配好"）
	{
		volatile uint32* sdram_probe = (volatile uint32*)FMC_SDRAM_BANK1_BASE;
		sdram_probe[0] = 0x55AA55AA;
		if (sdram_probe[0] != 0x55AA55AA) erro("SDRAM fail");
		XART1.OutFormat("SDRAM OK\r\n");
	}
	ltdc_init();  // 初始化 LTDC（引脚 + PLL3 像素时钟 + 时序 + 层）

	XART1.OutFormat("LTDC OK, PixClk=%dHz\r\n", LTDC.getFrequency());

	// ---- 白底画板（先清屏+标题，再初始化触摸：即使触摸失败也能看到画板界面）----
	ltdc_fill_white((pureptr_t)LCD_FB0_ADDR);
#if LCD_DOUBLE_BUFFER
	ltdc_fill_white((pureptr_t)LCD_FB1_ADDR);
#endif
	ltdc_text_fb(_fb_back, 10, 40, "STM32H7");
	ltdc_text_fb(_fb_back, 10, 80, "Alice.Wiki 20-LTDC-DMA2D-Touch");
	ltdc_text_fb(_fb_back, 10, 110, "@ArinaMgk");
	ltdc_text_fb(_fb_back, 10, 130, "Touch Paint Board");
	ltdc_text_fb(_fb_back, 10, 150, "RST corner to clear");
	lcd_swap();

	// ---- 触摸初始化（GT9147：SCL=PH6 / SDA=PI3 软件 I2C，RST=PI8，INT=PH7）----
	XART1.OutFormat("TP init...\r\n");
	TouchIIC touch_iic(GPIOI[3], GPIOH[6]);// SDA=PI3, SCL=PH6（push_pull 模式）
	touch_iic.func_delay = iic_delay;
	GT9147_t tp(touch_iic, GPIOI[8], GPIOH[7]);// RST=PI8, INT=PH7
	if (!tp.init()) {
		XART1.OutFormat("TP init FAIL: ACK_stage=%d PID=%02X%02X%02X%02X\r\n",
			tp.dbg_ack_stage, tp.dbg_pid[0], tp.dbg_pid[1], tp.dbg_pid[2], tp.dbg_pid[3]);
		erro("GT9147 init fail");
	}
	XART1.OutFormat("GT9147 OK\r\n");

	// 各指上次坐标（用于连线）与有效标记
	static uint16 last_x[5], last_y[5];
	static bool last_valid[5] = { false, false, false, false, false };

	while (true) {
		byte cnt = tp.scan();
		if (cnt) {
			for (byte i = 0; i < cnt && i < 5; i++) {
				uint16 tx = tp.x[i], ty = tp.y[i];
				if (tx < 800 && ty < 480) {// 坐标合法
					if (tx > 800 - 24 && ty < 20) {// 右上角 RST 区：清屏（照 HAL Load_Drow_Dialog）
						ltdc_fill_white((pureptr_t)LCD_FB0_ADDR);
						ltdc_fill_white((pureptr_t)LCD_FB1_ADDR);
						for (byte k = 0; k < 5; k++) last_valid[k] = false;
					} else {
						if (last_valid[i]) ltdc_line_fb(_fb_back, last_x[i], last_y[i], tx, ty, touch_colors[i]);
						last_x[i] = tx; last_y[i] = ty; last_valid[i] = true;
					}
				}
			}
			// 串口打印首指坐标（两者都要）
			XART1.OutFormat("X:%d,Y:%d\r\n", tp.x[0], tp.y[0]);
		} else {
			for (byte i = 0; i < 5; i++) last_valid[i] = false;// 全部松开，断开连线
		}
		lcd_swap();// 双缓冲：垂直消隐期切换层地址
		LEDB.Toggle();// DS0 指示运行
		SysDelay_ms(5);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
