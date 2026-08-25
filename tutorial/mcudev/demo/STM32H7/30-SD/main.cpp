// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 30-SD：SD 卡（SDMMC1）读取
 *
 * 预期现象：
 *   1) 上电后串口打印 "SD Card TEST"；检测到卡后打印卡容量（MB）与块大小。
 *   2) 检测不到卡时串口打印 "SD Card Error!"，DS0（LEDB）每 500ms 翻转，反复重试。
 *   3) 按 KEY0（PH3）：读 0 扇区（MBR/引导扇区），串口打印前 64 字节（十六进制）+ "DATA ENDED"。
 *
 * 使用说明：
 *   1) SDMMC1 引脚（PC8~PC12、PD2，AF12）由库硬编码，对应板载 SD 卡座。
 *   2) 块号以 512 字节为单位；Read(0, buf) 即读第 0 块。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYR = GPIOH[ 3];// KEY0 按下=0

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

static byte buf[512];

int main() {
	L1C.enAbleICacheAll();// 只开 I-cache；SDMMC 内部 IDMA 不经 D-cache，开 D-cache 会数据不一致导致 init 失败
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true);

	XART1.setMode(115200);
	XART1.OutFormat("SD Card TEST\r\n");

	// 定位：先确认 SDMMC1 内核时钟是否有效
	XART1.OutFormat("SDMMC1 freq=%uHz, clksrc=%d\r\n",
		(unsigned)SDCard1.getFrequency(), (int)SDCard1.getClockSource());

	// SDMMC1 init
	if (!SDCard1.setMode()) {
		XART1.OutFormat("FAIL! STA=0x%08X CMD=0x%08X RESPCMD=0x%02X RESP1=0x%08X DCTRL=0x%08X\r\n",
			(unsigned)SDCard1[SDReg::STA], (unsigned)SDCard1[SDReg::CMD],
			(unsigned)SDCard1[SDReg::RESPCMD], (unsigned)SDCard1[SDReg::RESP1],
			(unsigned)SDCard1[SDReg::DCTRL]);
		while (1) { SysDelay_ms(1000); }
	}
	XART1.OutFormat("SD Card OK\r\n");

	auto& ci = SDCard1.CardInfo;
	uint64 cap_bytes = (uint64)ci.BlockNbr * ci.BlockSize;
	uint32 cap_mb = (uint32)(cap_bytes / (1024U * 1024U));
	XART1.OutFormat("CardType=%d, BlockNbr=%u, Capacity %u MB, BlockSize %u\r\n",
		(int)SDCard1.CardType, (unsigned)ci.BlockNbr, (unsigned)cap_mb, (unsigned)ci.BlockSize);
	XART1.OutFormat("CSD=%08X %08X %08X %08X\r\n",
		(unsigned)SDCard1.CSD[0], (unsigned)SDCard1.CSD[1],
		(unsigned)SDCard1.CSD[2], (unsigned)SDCard1.CSD[3]);

	bool key0_prev = true;
	while (1) {
		bool k0 = (bool)KEYR;
		if (!k0 && key0_prev) { // KEY0: read sector 0
			if (SDCard1.Read(0, buf)) {
				XART1.OutFormat("SECTOR 0 DATA:\r\n");
				for0(i, 64) XART1.OutFormat("%02X ", (unsigned)buf[i]);
				XART1.OutFormat("\r\nDATA ENDED\r\n");
			} else {
				XART1.OutFormat("Read error\r\n");
			}
		}
		key0_prev = k0;
		SysDelay_ms(10);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
