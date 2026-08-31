// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 91-ImageShow-BaseOn20：SD 卡 FAT 文件系统枚举 JPG 图片，硬件 JPEG 解码后显示到 RGB-LCD，
 * 通过方向键在图片之间切换。
 *
 * 依赖：
 *   - SDMMC1（板载 SD 卡座）：SecureDigitalCard_t SDCard1
 *   - FAT 文件系统：FilesysFAT
 *   - 硬件 JPEG 解码：JPEGCodecHard（内部 JPEG_HARD + DMA2D YCbCr->RGB）
 *   - LTDC + SDRAM：_opendev/RGB-LCD.hpp（RGB-LCD.cpp / SDRAM.cpp 加入工程）
 *
 * 使用说明：
 *   1) SD 卡根目录放置若干 .jpg 图片（建议宽高不超过屏幕 800x480，宽高为 16 的倍数）。
 *   2) 上电初始化 SD + FAT + LCD，枚举根目录 JPG，显示第一张。
 *   3) 按键 KEY0(右)/KEY2(左) 切换上一张/下一张；KEY_UP 重新枚举。
 *   4) LTDC 层为 RGB565，帧缓冲位于 SDRAM（0xC0000000），解码输出 RGB565 直接写入。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include <cpp/Device/GPU>
#include <c/format/filesys/FAT.h>
#include <c/format/picture/JPEG.h>
#include <c/format/picture/PNG.h>
#include <c/format/picture/BMP.h>
#include <c/mempool.h>
#include <cpp/Device/DBG>
#include "../_opendev/RGB-LCD.hpp"
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;


extern uni::Mempool mempool;

// 板载外设
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// Up   (WK_UP)
GPIN& KEYL = GPIOC[13];// Left (KEY2)
GPIN& KEYD = GPIOH[ 2];// Down (KEY1)
GPIN& KEYR = GPIOH[ 3];// Right(KEY0)

void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// 内存管理：使用成熟 Mempool（c/mempool.h），管理外部 SDRAM 切片。
// 注意：LTDC 帧缓冲占用 SDRAM_BANK1_BASE 起的 ~0xC0000 字节，Mempool 切片避开。
// Mempool 本身是 trait::Malloc 的子类，可直接作为 JPEGCodecHard::Decode 的分配器。
// operator new 必须返回最大对齐地址（8 字节），否则对象被非对齐访问会 bus fault。
// Mempool 的 alignment 是 2 的指数：3 -> 2^3 = 8 字节对齐。


// Mempool 管理的内存切片（照 80-Memman：SDRAM + SRAM12 + SRAM4，不用 AXI/ITCM）。
// 避开 LTDC 帧缓冲（SDRAM_BANK1_BASE 起 ~0xC0000）与下方 FAT 缓冲（+0x100000 起 1KB）。
#define SDRAM_POOL_BASE (SDRAM_BANK1_BASE + 0x100000 + 0x1000)
#define SDRAM_POOL_SIZE (4U * 1024U * 1024U)
#define SRAM12_ADDR ((stduint)0x30000000)
#define SRAM4_ADDR  ((stduint)0x38000000)
// FAT / JPEG 缓冲（外部 SDRAM，避开 LTDC 帧缓冲与 MEMPOOL 切片）
static byte* sector_buf = (byte*)(SDRAM_BANK1_BASE + 0x100000); // 扇区缓冲 512B（1MB 处）
static byte* fat_buf    = sector_buf + 512;                     // FAT 表缓冲 512B
#define MAX_PIC 64
static const char* pic_name[MAX_PIC];                   // 图片路径数组
static stduint pic_count = 0;                           // 图片数量

// LTDC 帧缓冲：RGB565，800x480，位于 SDRAM
#define LCD_W 800
#define LCD_H 480
#define LCD_FB ((uint16*)FMC_SDRAM_BANK1_BASE)

// 枚举回调：_tocall_ft 是变参函数指针，FAT 按 (is_dir, name) 两参调用（见 FAT.cpp:531）
// C++ 中 lambda/普通函数不能隐式转变参函数指针，故用真正的变参函数。
static void on_seg(void* is_dir, ...) {
	if (is_dir) return;
	va_list ap;
	va_start(ap, is_dir);
	const char* nm = va_arg(ap, const char*);
	va_end(ap);
	if (!nm) return;
	stduint len = StrLength(nm);
	XART1.OutFormat("  file: '%s' len=%u\r\n", nm, (unsigned)len);
	if (len < 4) return;
	const char* ext = nm + len - 4;
	// XART1.OutFormat("  ext: '%s'\r\n", ext);
	// 只收集 .jpg/.jpeg/.png/.bmp（不区分大小写，FAT 短名为大写如 .JPG）
	if (StrCompareInsensitive(ext, ".jpg") && StrCompareInsensitive(ext, ".jpeg") &&
		StrCompareInsensitive(ext, ".png") && StrCompareInsensitive(ext, ".bmp")) return;
	// XART1.OutFormat("  -> jpg\r\n");
	if (pic_count >= MAX_PIC) return;
	char* full = (char*)mempool.allocate(len + 2);
	full[0] = '/';
	MemCopyN(full + 1, nm, len);
	full[len + 1] = 0;
	pic_name[pic_count++] = full;
}

// 枚举根目录下所有 .jpg 文件，收集到 pic_name
static stduint collect_jpg(FilesysFAT& fs) {
	pic_count = 0;
	FAT_FileHandle h;
	FilesysSearchArgs sargs{};
	sargs.handle_buffer = &h;
	if (!fs.search("/", &sargs)) return 0;
	fs.enumer(&h, on_seg);
	return pic_count;
}

// ---- 按键中断（EXTI）回调：只置事件标志，处理放主循环 ----
static volatile byte key_event = 0;
static void on_key_next() { key_event = 1; }// KEYR：下一张
static void on_key_prev() { key_event = 2; }// KEYL：上一张
static void on_key_enum(){ key_event = 3; }// KEYU：重新枚举

int main() {
	L1C.enAble();// I+D cache 全开（照 20-LTDC-DMA2D：SDRAM + LTDC + DMA2D 场景）
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	// 初始化 Mempool：登记 SDRAM + SRAM12 + SRAM4 切片，关闭自动扩展
	mempool.enable_auto_expand = false;
	mempool.Append(Slice{ SDRAM_POOL_BASE, SDRAM_POOL_SIZE });
	mempool.Append(Slice{ SRAM12_ADDR, 64U * 1024U });
	mempool.Append(Slice{ SRAM4_ADDR,  32U * 1024U });

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);

	// 按键改中断方式（照 doc-qrs/gpio.md）：KEYR/KEYL 按下为低(下降沿)，KEYU 按下为高(上升沿)
	KEYR.setMode(GPIORupt::Negedge, on_key_next);
	KEYR.setInterruptPriority(2, 0);
	KEYR.enInterrupt();
	KEYL.setMode(GPIORupt::Negedge, on_key_prev);
	KEYL.setInterruptPriority(2, 0);
	KEYL.enInterrupt();
	KEYU.setMode(GPIORupt::Posedge, on_key_enum);
	KEYU.setInterruptPriority(2, 0);
	KEYU.enInterrupt();

	XART1.setMode(115200);
	XART1.OutFormat("ImageShow TEST\r\n");

	sdram_init();
	ltdc_init();
	XART1.OutFormat("LTDC OK\r\n");

	DBGMCU.enDBGStandby(true);         // Domain1 待机调试（DBGMCU_CR.STANDBYD1）
	DBGMCU.enDBGStandbyDomain3(true);  // Domain3 待机调试（保持 SWD 口）

	// 启动时清屏（帧缓冲填黑，清掉背景残留乱码）
	LTDC[1].DrawRectangle(GrafRect(0, 0, LCD_W, LCD_H, Color::Black));

	// SD 卡初始化
	if (!SDCard1.setMode()) {
		XART1.OutFormat("SD Error!\r\n");
		while (1) { SysDelay_ms(1000); }
	}
	SDCard1.Block_Size = 512;

	// 挂载 FAT
	FilesysFAT fs(0, SDCard1, sector_buf, fat_buf);
	if (!fs.loadfs()) {
		XART1.OutFormat("FAT mount FAIL %u\r\n", (unsigned)fs.error_number);
		while (1) { SysDelay_ms(1000); }
	}
	XART1.OutFormat("FAT%u OK\r\n", (unsigned)fs.fat_type);

	// 枚举图片
	collect_jpg(fs);
	XART1.OutFormat("%u JPG found\r\n", (unsigned)pic_count);
	if (!pic_count) {
		XART1.OutFormat("No JPG\r\n");
		while (1) { SysDelay_ms(1000); }
	}

	stduint cur = 0;
	JPEGCodecHard hw(JPEG);// 硬件 JPEG codec（用户注入 JPEG 对象）
	JPEGCodec soft;// 软件 JPEG codec（硬件失败时回退）
	PNGCodec png;// 软件 PNG codec
	BMPCodec bmp;// 软件 BMP codec
	ImageDecodeOptions dopt;
	ImageDecodeOptionsInit(dopt);
	dopt.preferredFormat = PixelFormat::RGB565;// 输出 RGB565（对齐 LTDC 层）

	while (true) {
		LTDC[1].DrawRectangle(GrafRect(0, 0, LCD_W, LCD_H, Color::Black));
		// 读取当前图片文件
		FAT_FileHandle fh;
		FilesysSearchArgs sargs{};
		sargs.handle_buffer = &fh;
		void* fh2 = fs.search(pic_name[cur], &sargs);
		if (!fh2 || fh.is_dir || !fh.size) {
			XART1.OutFormat("open %s FAIL\r\n", pic_name[cur]);
		} else {
			stduint fsize = (stduint)fh.size;
			if (fsize > 512 * 1024) fsize = 512 * 1024;// 上限 512KB
			byte* fdata = (byte*)mempool.allocate(fsize + 1);
			stduint rd = fs.readfl(fh2, Slice{0, fsize}, fdata);
			XART1.OutFormat("read %s %uB\r\n", pic_name[cur], (unsigned)rd);

			// 硬件解码，失败则回退软件 JPEG 解码
			ImageBuffer out;
			ImageBufferClear(out);
			MemoryBlockDevice mem(Slice{ (stduint)fdata, rd }, sector_buf, 1);
			ImageResult r;
			stduint nlen = StrLength(pic_name[cur]);
			const char* ext = pic_name[cur] + nlen - 4;
			if (StrCompareInsensitive(ext, ".png") == 0) {
				r = png.Decode(mem, out, mempool, dopt);
			} else if (StrCompareInsensitive(ext, ".bmp") == 0) {
				r = bmp.Decode(mem, out, mempool, dopt);
			} else {
				// 硬件 JPEG，失败则回退软件 JPEG
				r = hw.Decode(mem, out, mempool, dopt);
				if (r != ImageResult::OK || !out.pixels) {
					ImageBufferFree(out);
					XART1.OutFormat("hw FAIL %d -> soft\r\n", (int)r);
					r = soft.Decode(mem, out, mempool, dopt);
				}
			}
			if (r == ImageResult::OK && out.pixels) {
				// 超屏图片直接跳过不显示（暂不做缩放）
				if (out.width > LCD_W || out.height > LCD_H) {
					XART1.OutFormat("skip %ux%u (too large)\r\n", (unsigned)out.width, (unsigned)out.height);
					ImageBufferFree(out);
				} else {
					// 写 SDRAM 帧缓冲（居中显示），支持硬件 RGB565 或软件 ARGB8888
					stduint ox = (LCD_W - out.width) / 2, oy = (LCD_H - out.height) / 2;
					Color* px32 = (Color*)out.pixels;
					uint16* px16 = (uint16*)out.pixels;
					for (stduint y = 0; y < out.height; y++) {
						uint16* dst = LCD_FB + (oy + y) * LCD_W + ox;
						for (stduint x = 0; x < out.width; x++) {
							if (out.format == PixelFormat::RGB565)
								dst[x] = px16[y * out.width + x];
							else
								dst[x] = px32[y * out.width + x].ToRGB565();
						}
					}
					XART1.OutFormat("show %ux%u\r\n", (unsigned)out.width, (unsigned)out.height);
					ImageBufferFree(out);
				}
			} else {
				XART1.OutFormat("decode FAIL %d\r\n", (int)r);
			}
			// 释放读入的 JPEG 文件缓冲（decode 内部会自己拷贝一份 stream）
			mempool.deallocate(fdata, fsize + 1);
		}

		// 等待按键中断事件（回调已置 key_event），空转等待
		LEDB.Toggle();
		while (key_event == 0) {
			SysDelay_ms(10, true);
		}
		stduint key = key_event;
		key_event = 0;
		if (key == 1) { cur = (cur + 1 < pic_count) ? cur + 1 : 0; }
		else if (key == 2) { cur = (cur > 0) ? cur - 1 : pic_count - 1; }
		else if (key == 3) { collect_jpg(fs); XART1.OutFormat("%u JPG\r\n", (unsigned)pic_count); if (pic_count) cur = 0; }
	}
}
void printlog(loglevel_t level, const char* fmt, ...) {}
void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
