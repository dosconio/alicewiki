// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 18-Flash-NOR：内部 NOR Flash 块读写演示
 *
 * 预期现象：
 *   1) 上电串口打印 "UNISYM STM32H743 Flash demo"，并打印容量信息
 *      （getUnits × 32 字节 = 2MB，双 bank）。
 *   2) 按 KEYU(PA0)：向测试块写入 32 字节 pattern（0x00~0x1F），
 *      串口打印 "Write OK"，再次按可刷新 pattern。
 *   3) 按 KEYL(PC13)：读取测试块并以十六进制打印 32 字节。
 *   4) 按 KEYD(PH2)：擦除测试块所在扇区（Bank2 Sector7），串口打印 "Erase OK"；
 *      之后按 KEYL 读回应为全 0xFF。
 *   5) 按 KEYR(PH3)：打印测试块信息（块号、所在 bank/扇区、绝对地址）。
 *
 * 使用说明：
 *   1) 测试块 DEMO_BLOCK=61440，位于 Bank2 末扇区（0x081E0000 之后），
 *      远离运行中的程序（程序通常 <1MB 只占 Bank1），擦写安全。
 *   2) 内部 Flash 写/擦前必须先 Flash.Unlock()，完成后 Flash.Lock()。
 *   3) Write 自动先擦所在扇区再编程 256-bit 行（32 字节粒度）。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/Flash>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// KEY_UP (PA0, WKUP) 按下=1
GPIN& KEYL = GPIOC[13];// KEY2 按下=0
GPIN& KEYD = GPIOH[ 2];// KEY1 按下=0
GPIN& KEYR = GPIOH[ 3];// KEY0 按下=0

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// test block: Bank2 last sector (sector 7, 0x081E0000..0x08200000), block 61440
#define DEMO_BLOCK 61440
#define DEMO_BANK  2
#define DEMO_SECTOR 7

static byte block_buf[32];

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true);

	XART1.setMode(115200);
	SysDelay_ms(500);
	XART1.OutFormat("UNISYM STM32H743 Flash demo\r\n");
	XART1.OutFormat("units=%u, size=%uMB\r\n", (unsigned)Flash.getUnits(),
		(unsigned)(Flash.getUnits() * 32 / 1024 / 1024));

	byte fill = 0;
	bool keyu_prev = true, keyl_prev = true, keyd_prev = true, keyr_prev = true;

	while (1) {
		bool ku = (bool)KEYU, kl = (bool)KEYL, kd = (bool)KEYD, kr = (bool)KEYR;
		if (ku && !keyu_prev) {                 // KEY_UP: write pattern to test block
			for (byte i = 0; i < 32; i++) block_buf[i] = (byte)(i + fill);
			if (!Flash.Unlock()) { XART1.OutFormat("Unlock failed\r\n"); }
			else {
				bool ok = Flash.Write(DEMO_BLOCK, block_buf);// auto-erase sector then program
				Flash.Lock();
				XART1.OutFormat(ok ? "Write OK\r\n" : "Write failed\r\n");
				fill += 32;
			}
		}
		if (!kl && keyl_prev) {                 // KEY2: read test block, hex dump
			Flash.Read(DEMO_BLOCK, block_buf);
			XART1.OutFormat("Block %u data:\r\n", DEMO_BLOCK);
			for (byte i = 0; i < 32; i++) {
				XART1.OutFormat("%02X ", block_buf[i]);
				if ((i & 15) == 15) XART1.OutFormat("\r\n");
			}
		}
		if (!kd && keyd_prev) {                 // KEY1: erase test block's sector
			if (!Flash.Unlock()) { XART1.OutFormat("Unlock failed\r\n"); }
			else {
				bool ok = Flash.Erase(DEMO_BANK, DEMO_SECTOR);
				Flash.Lock();
				XART1.OutFormat(ok ? "Erase OK\r\n" : "Erase failed\r\n");
			}
		}
		if (!kr && keyr_prev) {                 // KEY0: show test block info
			XART1.OutFormat("block=%u bank=%d sector=%d addr=0x%08X\r\n",
				DEMO_BLOCK, DEMO_BANK, DEMO_SECTOR,
				(unsigned)(0x081E0000 + (DEMO_BLOCK - 61440) * 32));
		}
		keyu_prev = ku;
		keyl_prev = kl;
		keyd_prev = kd;
		keyr_prev = kr;
		SysDelay_ms(10);
		LEDB.Toggle();
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
