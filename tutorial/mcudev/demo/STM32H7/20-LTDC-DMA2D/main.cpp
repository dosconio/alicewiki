#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/GPU>
#include <c/data.h>
#include "../_opendev/RGB-LCD.hpp"// LTDC + SDRAM 共享初始化（RGB-LCD.cpp / SDRAM.cpp 已加入工程）
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

#define LCD_DOUBLE_BUFFER 1


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

// 4.3寸 RGB 屏 800x480（面板 ID 0x4384）；帧缓冲放外部 SDRAM
// 说明：SDRAM / LTDC 初始化改用共享抽象 _opendev/RGB-LCD.hpp：
//       sdram_init() / ltdc_init() 由 RGB-LCD.cpp + SDRAM.cpp 提供
#define LCD_FRAME_BUF_ADDR SDRAM_BANK1_BASE// 帧缓冲基址（_opendev/SDRAM.hpp）

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

// 【帧缓冲方案（宏选择）】开关见文件顶部：`#define LCD_DOUBLE_BUFFER`（0=单缓冲，1=双缓冲）
// LCD_DOUBLE_BUFFER=0：单缓冲 —— 直接渲染到 LTDC 正在扫描的帧缓冲；整屏重写时，
//                       扫描到一半被新内容截断，画面上下出现新旧分界（撕裂/闪烁）。
// LCD_DOUBLE_BUFFER=1：双缓冲 —— 渲染到后台缓冲，等垂直消隐（当前行进入消隐区）再
//                       切换层地址（CFBAR + SRCR.VBR），画面在帧边界整帧切换，无撕裂。

#define LCD_FB_STRIDE 800                        // 帧缓冲行宽（像素）
#define LCD_FB_LINES  480                        // 帧缓冲行数
#define LCD_FB_SIZE   (LCD_FB_STRIDE * LCD_FB_LINES * 2)// RGB565：每像素 2 字节
#define LCD_FB0_ADDR  LCD_FRAME_BUF_ADDR         // 缓冲 0（LTDC 初始层地址）
#define LCD_FB1_ADDR  (LCD_FRAME_BUF_ADDR + LCD_FB_SIZE)// 缓冲 1
#define BLOCK_SIZE 64                            // M2M 色块边长（像素）

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

// ---- DMA2D 加速渲染演示 ----
#if LCD_DOUBLE_BUFFER
static pureptr_t _fb_front = (pureptr_t)LCD_FB0_ADDR;// 当前 LTDC 显示缓冲
static pureptr_t _fb_back  = (pureptr_t)LCD_FB1_ADDR;// 当前后台渲染缓冲
static uint32* const _buf_argb  = (uint32*)(LCD_FB1_ADDR + LCD_FB_SIZE);                 // PFC 转换源：ARGB8888 渐变
static uint16* const _buf_block = (uint16*)(LCD_FB1_ADDR + LCD_FB_SIZE + 800 * 480 * 4); // M2M 拷贝源：RGB565 色块
// 渲染完成后调用：等待垂直消隐（当前行 ≥ 有效行数）再切换层地址，无撕裂
static void lcd_swap() {
	while (LTDC[LTDCReg::CPSR].masof(0, 16) < LCD_FB_LINES);// 等待进入消隐区
	LTDC[1][LTDCLayerReg::CFBAR] = _IMM(_fb_back);          // 层地址指向刚渲染完的后台缓冲
	LTDC.Reload(LTDCReload::VerticalBlank);                  // 下次垂直同步时生效
	pureptr_t _t = _fb_front; _fb_front = _fb_back; _fb_back = _t;// 前后台角色互换
}
#else
#define _fb_back ((pureptr_t)LCD_FB0_ADDR)// 单缓冲：渲染目标就是显示缓冲
static uint32* const _buf_argb  = (uint32*)(LCD_FRAME_BUF_ADDR + 0x100000);                  // PFC 转换源：ARGB8888 渐变
static uint16* const _buf_block = (uint16*)(LCD_FRAME_BUF_ADDR + 0x100000 + 800 * 480 * 4);  // M2M 拷贝源：RGB565 色块
static void lcd_swap() {}
#endif

// 【预期现象】
// 上电后屏幕循环演示三种 DMA2D 渲染，每种持续约 2 秒，DS0 每 1 秒翻转一次：
//   ① R2M FILL：整屏背景色循环（白→黑→蓝→红→……共 12 色），每 0.5 秒换一种，
//                屏幕左上方显示 "DMA2D R2M FILL" —— 说明 DMA2D 寄存器到内存填充正常。
//   ② M2M BLIT：深灰色背景上一个绿色 64×64 色块沿斜线运动、碰到屏幕边缘反弹，
//                左上方显示 "DMA2D M2M BLIT" —— 说明 DMA2D 内存到内存拷贝正常。
//   ③ M2M PFC ：整屏显示红→绿→蓝渐变色（离屏 ARGB8888 渐变实时转成 RGB565 后上屏），
//                左上方显示 "DMA2D M2M PFC" —— 说明 DMA2D 像素格式转换正常。
// 若某阶段黑屏/花屏/画面静止：先看串口是否打印 "LTDC OK, PixClk=..." 与 "DMA2D OK"，
// 若打印正常则检查该阶段 DMA2D 配置或离屏缓冲地址（_buf_argb/_buf_block 是否与帧缓冲重叠）。
// 若开启双缓冲（LCD_DOUBLE_BUFFER=1）后仍有闪烁，检查垂直消隐轮询与 Reload(VerticalBlank)。

// 生成 ARGB8888 渐变（CPU 一次性生成，之后由 DMA2D 硬件转成 RGB565）
static void dma2d_prepare_gradient() {
	for (stduint y = 0; y < 480; y++)
		for (stduint x = 0; x < 800; x++)
			_buf_argb[y * 800 + x] = 0xFF000000U | ((x * 255 / 799) << 16) | ((y * 255 / 479) << 8) | ((x + y) * 255 / 1279);
}

// 生成 64x64 绿色色块：连续 64×64 排布源（FGOR=0），拷贝时目标用 OOR 换行
static void dma2d_prepare_block() {
	for (stduint y = 0; y < BLOCK_SIZE; y++)
		for (stduint x = 0; x < BLOCK_SIZE; x++)
			_buf_block[y * BLOCK_SIZE + x] = 0x07E0;
}

// 显示标题文本到后台缓冲（基线条目 + 当前演示名）
static void ltdc_texts(const char* phase_name) {
	ltdc_text_fb(_fb_back, 10, 40, "STM32H7");
	ltdc_text_fb(_fb_back, 10, 80, "Alice.Wiki 20-LTDC-DMA2D");
	ltdc_text_fb(_fb_back, 10, 110, "@ArinaMgk");
	ltdc_text_fb(_fb_back, 10, 150, __DATE__);
	ltdc_text_fb(_fb_back, 10, 180, phase_name);
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

	sdram_init(); // 共享抽象：初始化 SDRAM（帧缓冲位于 0xC0000000）
	ltdc_init();  // 共享抽象：初始化 LTDC（引脚 + PLL3 像素时钟 + 时序 + 层）

	XART1.OutFormat("LTDC OK, PixClk=%dHz\r\n", LTDC.getFrequency());

	// ---- DMA2D 初始化（主模式 R2M，输出 RGB565 匹配帧缓冲）----
	DMA2D.setMode(DMA2DMode::R2M, PixelFormat::RGB565);
	dma2d_prepare_gradient();
	dma2d_prepare_block();
	XART1.OutFormat("DMA2D OK\r\n");

	byte x = 0;
	byte phase = 0;      // 0: R2M 填充, 1: M2M 拷贝, 2: PFC 转换
	byte phase_cnt = 0;  // 当前演示已执行帧数（每 40 帧 = 2s 切换）
	stdsint blit_x = 0, blit_y = 0, blit_vx = 4, blit_vy = 3;// M2M 色块位置与速度
	while (true) {
		switch (phase) {
		case 0: {
			// ① R2M：DMA2D 硬件填充整屏背景（替代 CPU 逐像素清屏），每 10 帧换色
			DMA2D.setMode(DMA2DMode::R2M, PixelFormat::RGB565);
			DMA2D.Transfer((pureptr_t)uint32(bg_colors[x]), (pureptr_t)_fb_back, 800, 480);
			ltdc_texts("DMA2D R2M FILL");
			if (phase_cnt % 10 == 0) { x++; if (x >= 12) x = 0; }
		} break;
		case 1: {
			// ② M2M：DMA2D 硬件拷贝移动色块（先 R2M 清深灰背景，再 M2M 拷贝色块）
			DMA2D.setMode(DMA2DMode::R2M, PixelFormat::RGB565);
			DMA2D.Transfer((pureptr_t)0xFF202020U, (pureptr_t)_fb_back, 800, 480);
			blit_x += blit_vx; blit_y += blit_vy;
			if (blit_x <= 0 || blit_x + BLOCK_SIZE >= 800) { blit_vx = -blit_vx; blit_x += blit_vx; }
			if (blit_y <= 0 || blit_y + BLOCK_SIZE >= 480) { blit_vy = -blit_vy; blit_y += blit_vy; }
			DMA2D.setMode(DMA2DMode::M2M, PixelFormat::RGB565);
			DMA2D[DMA2DReg::FGOR] = 0;// 源行偏移 0：连续 64×64 源，每行 64 像素后连续读下一行
			DMA2D.Transfer((pureptr_t)_buf_block,
				(pureptr_t)(_IMM(_fb_back) + (blit_y * LCD_FB_STRIDE + blit_x) * 2), BLOCK_SIZE, BLOCK_SIZE, LCD_FB_STRIDE);
				// 目标行宽 800：Transfer 自动设 OOR=800-64=736，64 行铺成 64×64 方块
			ltdc_texts("DMA2D M2M BLIT");
		} break;
		case 2: {
			// ③ M2M PFC：ARGB8888 渐变 → RGB565 帧缓冲（硬件格式转换）
			DMA2D.setMode(DMA2DMode::M2MPFC, PixelFormat::RGB565);
			DMA2D_LAYER_t::LayerPara lp{};
			lp.pixel_format = PixelFormat::ARGB8888;
			lp.input_offset = 0;
			DMA2D[1].setMode(lp);
			DMA2D.Transfer((pureptr_t)_buf_argb, (pureptr_t)_fb_back, 800, 480);
			ltdc_texts("DMA2D M2M PFC");
		} break;
		}
		lcd_swap();// 双缓冲：垂直消隐期切换层地址；单缓冲：空操作
		if (++phase_cnt >= 40) { phase_cnt = 0; phase = (phase + 1) % 3; }// 每 40 帧（2s）切换演示
		if (phase_cnt % 20 == 0) LEDB.Toggle();// DS0 每 1s 翻转
		SysDelay_ms(50);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
