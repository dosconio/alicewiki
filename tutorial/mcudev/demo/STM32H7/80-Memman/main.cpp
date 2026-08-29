// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 80-Memman：内存管理实验（迁移自 HAL 实验39），使用 unisym Mempool
 *
 * 预期现象：
 *   1) 上电后串口打印 "MEMPOOL TEST"；随后直接写各内存区基址，
 *      打印 AXI/SDRAM/SRAM12/SRAM4/ITCM 是否可用。
 *   2) 单个 Mempool 管理这些内存切片。
 *   3) 方向键操作：
 *        KEY_RT(KEY0/PH3) 申请 2K 内存；
 *        KEY_DN(KEY1/PH2) 往申请到的内存写测试串并回读打印；
 *        KEY_LT(KEY2/PC13) 释放内存；
 *        KEY_UP(WK_UP/PA0) 重新测试各内存区。
 *   4) 每次操作后串口打印使用量/使用率。
 *
 * 说明：
 *   DTCM(0x20000000) 被链接器用作程序 RAM，本 demo 不纳入。
 */
 // 仅作演示。实际在参考时，请只使用 SDRAM、SRAM1/2/4 ；不要使用 AXI/ITCM
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include "../_opendev/SDRAM.hpp"
#include <c/mempool.h>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEY_UP = GPIOA[ 0];// Up   (WK_UP) 按下=1
GPIN& KEY_DN = GPIOH[ 2];// Down (KEY1)  按下=0
GPIN& KEY_LT = GPIOC[13];// Left (KEY2)  按下=0
GPIN& KEY_RT = GPIOH[ 3];// Right(KEY0)  按下=0

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// ---- 内存区配置（DTCM 被链接器占用，不纳入）----
// AXI：静态 BSS 数组（位于内部 AXI SRAM）
static byte axi_buf[64 * 1024];
// SDRAM：外部 SDRAM 1MB 段（避开 0x0 起的测试区）
#define SDRAM_POOL_OFF 0x100000
// SRAM12 / SRAM4 / ITCM：scatter 未占用，直接用固定地址
#define SRAM12_ADDR ((stduint)0x30000000)
#define SRAM4_ADDR  ((stduint)0x38000000)
#define ITCM_ADDR   ((stduint)0x00000000)

#define MEMREG_CNT 5
struct MemRegion {
	const char* name;
	stduint base;
	stduint size;
};
static const MemRegion memreg[MEMREG_CNT] = {
	{ "AXI",   (stduint)(pureptr_t)axi_buf,       byteof(axi_buf) },
	{ "SDRAM", SDRAM_BANK1_BASE + SDRAM_POOL_OFF, 1024U * 1024U },
	{ "SRAM12",SRAM12_ADDR,                       64U * 1024U },
	{ "SRAM4", SRAM4_ADDR,                        32U * 1024U },
	{ "ITCM",  ITCM_ADDR,                         32U * 1024U },
};

// 单个 Mempool，统一管理以上切片
Mempool mempool;

// 内存可用性测试：直接写读回基址（不经过 Mempool）
static bool mem_probe(stduint addr) {
	volatile byte* p = (volatile byte*)addr;
	p[0] = 0xAA; p[1] = 0x55;
	return p[0] == 0xAA && p[1] == 0x55;
}

// 往 buf 写入测试串 "MallocTest#NNN"（逐字节，避免非对齐访问）
static void mem_write(byte* p, unsigned idx) {
	const char* head = "MallocTest";
	stduint i = 0;
	while (head[i]) p[i] = head[i], i++;
	p[i++] = '#';
	char tmp[12]; unsigned n = 0;
	do { tmp[n++] = (char)('0' + idx % 10); idx /= 10; } while (idx);
	while (n) p[i++] = tmp[--n];
	p[i] = 0;
}

// 按键扫描（不支持连按）
byte key_scan() {
	static bool key_up = true;
	if (key_up && (!KEY_RT || !KEY_DN || !KEY_LT || KEY_UP)) {
		SysDelay_ms(10);
		key_up = false;
		if (!KEY_RT) return 1;// KEY_RT
		if (!KEY_DN) return 2;// KEY_DN
		if (!KEY_LT) return 3;// KEY_LT
		if (KEY_UP) return 4;// KEY_UP
	}
	else if (KEY_RT && KEY_DN && KEY_LT && !KEY_UP) key_up = true;
	return 0;
}

int main() {
	L1C.enAbleICacheAll();// 只开 I-cache，本实验不涉及 DMA
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEY_UP.setMode(GPIOMode::IN_Pull).setPull(false);// WK_UP 高有效
	KEY_DN.setMode(GPIOMode::IN_Pull).setPull(true);
	KEY_LT.setMode(GPIOMode::IN_Pull).setPull(true);
	KEY_RT.setMode(GPIOMode::IN_Pull).setPull(true);

	XART1.setMode(115200);
	XART1.OutFormat("MEMPOOL TEST\r\n");

	// 初始化 SDRAM（SDRAM 区用）
	sdram_init();
	XART1.OutFormat("SDRAM init done\r\n");

	// 直接写各基址，测试内存是否可用
	for0(i, MEMREG_CNT) {
		bool ok = mem_probe(memreg[i].base);
		XART1.OutFormat("[%s] %s\r\n", memreg[i].name, ok ? "OK" : "FAIL");
	}

	// 一个 Mempool，添加所有切片（关掉 auto-expand）
	mempool.enable_auto_expand = false;
	stduint pool_total = 0;
	for0(i, MEMREG_CNT) {
		mempool.Append(Slice{ memreg[i].base, memreg[i].size });
		pool_total += memreg[i].size;
	}

	stduint pool_used = 0;
	byte* p = 0;
	unsigned cnt = 0;

	XART1.OutFormat("KEY_RT:Malloc 2K  KEY_DN:Write  KEY_LT:Free  KEY_UP:Re-test\r\n");

	while (1) {
		byte key = key_scan();
		if (key == 1) {// 申请 2K
			byte* np = (byte*)mempool.allocate(2048, 0, 0);
			if (np) {
				if (p) { mempool.deallocate(p, 0); pool_used -= 2048; }
				p = np;
				pool_used += 2048;
				mem_write(p, cnt);
				XART1.OutFormat("alloc @0x%08X\r\n", (unsigned)(stduint)(pureptr_t)p);
			}
			else XART1.OutFormat("alloc FAIL\r\n");
		}
		else if (key == 2) {// 写测试串并回读
			if (p) {
				mem_write(p, cnt);
				XART1.OutFormat("data = %s\r\n", (const char*)p);
			}
			else XART1.OutFormat("nothing allocated\r\n");
		}
		else if (key == 3) {// 释放
			if (p) {
				mempool.deallocate(p, 0);
				pool_used -= 2048;
				p = 0;
				XART1.OutFormat("free done\r\n");
			}
			else XART1.OutFormat("nothing to free\r\n");
		}
		else if (key == 4) {// 重新测试各内存区
			for0(i, MEMREG_CNT) {
				bool ok = mem_probe(memreg[i].base);
				XART1.OutFormat("[%s] %s\r\n", memreg[i].name, ok ? "OK" : "FAIL");
			}
		}

		if (key) {
			XART1.OutFormat("used %u/%u (%u%%)\r\n",
				(unsigned)pool_used, (unsigned)pool_total,
				(unsigned)(pool_used * 100 / pool_total));
		}
		cnt++;
		SysDelay_ms(10);
	}
}
void printlog(loglevel_t level, const char* fmt, ...){}
void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
