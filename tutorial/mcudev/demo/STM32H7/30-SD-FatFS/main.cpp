// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 30-SD-FatFS：SD 卡（SDMMC1）挂载 unisym FAT 文件系统，缓冲放外部 SDRAM
 *
 * 预期现象：
 *   1) 上电后串口打印 "SD-FatFS TEST"；SDRAM 初始化并探测 OK 后打印 "SDRAM OK"。
 *   2) SD 初始化 OK 后打印 "SD Card OK"。
 *   3) 以 SDCard1 为底层存储构造 FilesysFAT，loadfs() 挂载，成功打印 "FAT32 mounted OK"。
 *   4) 自动 删除->创建 "/TEST.TXT"，writfl 写入 18 字节，再 readfl 读回打印，验证读写链路。
 *
 * 使用说明：
 *   1) SDMMC1 引脚（PC8~PC12、PD2，AF12）由库硬编码，对应板载 SD 卡座。
 *   2) 卡需已按 FAT 格式化（FAT12/16/32 均可，fat_type 传 0 由 loadfs 自动识别）。
 *   3) FilesysFAT 直接以 SecureDigitalCard_t（StorageTrait）为存储，Block_Size 置 512。
 *   4) SDRAM 初始化用共享模块 _opendev/SDRAM.cpp（需作为源文件加入工程编译）；
 *      FilesysFAT 的扇区/FAT/读回缓冲全部位于 SDRAM Bank1（0xC0000000）。
 *   5) FAT 实现（FAT.cpp 等）由库提供，本工程仅调用其接口。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include "../_opendev/SDRAM.hpp"
#include <c/format/filesys/FAT.h>
#include <new>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYR = GPIOH[ 3];// KEY0 按下=0

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// 简单堆：启动文件堆只有 512B，而 FAT 库用 new 分配 512B 缓冲会失败。
// 直接复用已初始化好的外部 SDRAM 作堆池（避开上方 FAT 缓冲，偏移 0x1000 起，1MB）。
namespace {
	static byte* heap_pool  = (byte*)SDRAM_BANK1_BASE + 0x1000;
	static byte* heap_cursor = heap_pool;
	static byte* heap_limit  = heap_pool + (1024U * 1024U);
}
static void* heap_alloc(size_t size) {
	size = (size + 15U) & ~(size_t)15U;// 16 字节对齐
	if (heap_cursor + size > heap_limit) {
		while (1) { }// 堆耗尽，停机
	}
	void* p = heap_cursor;
	heap_cursor += size;
	return p;
}
void* operator new(size_t size)              { return heap_alloc(size); }
void* operator new[](size_t size)            { return heap_alloc(size); }
void  operator delete(void* p)        noexcept { (void)p; }
void  operator delete[](void* p)      noexcept { (void)p; }
void  operator delete(void* p, size_t) noexcept { (void)p; }
void  operator delete[](void* p, size_t) noexcept { (void)p; }

// FilesysFAT 的缓冲全部放到外部 SDRAM（SDRAM_BANK1_BASE，0xC0000000）
static byte* sector_buf = (byte*)SDRAM_BANK1_BASE;      // 扇区缓冲（512B）
static byte* fat_buf    = sector_buf + 512;             // FAT 表缓冲（512B）
static byte* file_buf   = sector_buf + 1024;            // 读回文件数据缓冲

int main() {
	L1C.enAbleICacheAll();// 只开 I-cache；SDMMC 内部 IDMA 不经 D-cache，开 D-cache 会数据不一致
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true);

	XART1.setMode(115200);
	XART1.OutFormat("SD-FatFS TEST\r\n");

	// 初始化 SDRAM 并快速探测读写
	sdram_init();
	*(volatile uint32*)SDRAM_BANK1_BASE = 0x55AA55AA;
	if (*(volatile uint32*)SDRAM_BANK1_BASE != 0x55AA55AA) {
		XART1.OutFormat("SDRAM fail\r\n");
		while (1) { SysDelay_ms(1000); }
	}
	XART1.OutFormat("SDRAM OK\r\n");

	// SDMMC1 init
	if (!SDCard1.setMode()) {
		XART1.OutFormat("SD Card Error!\r\n");
		while (1) { SysDelay_ms(1000); }
	}
	XART1.OutFormat("SD Card OK\r\n");

	auto& ci = SDCard1.CardInfo;
	uint64 cap_bytes = (uint64)ci.BlockNbr * ci.BlockSize;
	uint32 cap_mb = (uint32)(cap_bytes / (1024U * 1024U));
	XART1.OutFormat("CardType=%d, BlockNbr=%u, Capacity %u MB\r\n",
		(int)SDCard1.CardType, (unsigned)ci.BlockNbr, (unsigned)cap_mb);

	// SD 作为 FAT 的底层存储，块大小固定 512
	SDCard1.Block_Size = 512;

	// 构造 FAT 文件系统并挂载（fat_type=0 自动识别 12/16/32；缓冲位于 SDRAM）
	FilesysFAT fs(0, SDCard1, sector_buf, fat_buf);
	if (!fs.loadfs()) {
		XART1.OutFormat("FAT mount FAIL, error=%u\r\n", (unsigned)fs.error_number);
		while (1) { SysDelay_ms(1000); }
	}
	XART1.OutFormat("FAT%u mounted OK\r\n", (unsigned)fs.fat_type);

	// 删除旧文件（忽略结果），再创建新文件
	fs.remove("/TEST.TXT");
	if (!fs.create("/TEST.TXT", 0, nullptr)) {
		XART1.OutFormat("create FAIL\r\n");
		while (1) { SysDelay_ms(1000); }
	}

	// 通过 search 拿到文件句柄（含 dir_sector/dir_index，供写入回写目录项）
	FAT_FileHandle h;
	FilesysSearchArgs sargs{};
	sargs.handle_buffer = &h;
	void* fh = fs.search("/TEST.TXT", &sargs);
	if (!fh) {
		XART1.OutFormat("search FAIL\r\n");
		while (1) { SysDelay_ms(1000); }
	}
	XART1.OutFormat("file size before=%u\r\n", (unsigned)h.size);

	// 写入 18 字节，再读回验证
	const char* msg = "Hello, unisym FAT!";
	stduint wlen = fs.writfl(fh, Slice{0, 18}, (const byte*)msg);
	XART1.OutFormat("writfl=%u, file size after=%u\r\n", (unsigned)wlen, (unsigned)h.size);

	stduint rlen = fs.readfl(fh, Slice{0, 18}, file_buf);
	file_buf[18] = 0;
	XART1.OutFormat("readfl=%u -> %s\r\n", (unsigned)rlen, file_buf);

	while (1) { SysDelay_ms(1000); }
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
