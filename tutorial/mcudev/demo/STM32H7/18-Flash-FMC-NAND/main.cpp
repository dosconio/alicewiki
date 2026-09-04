// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 18-Flash-FMC-NAND：外挂 NAND Flash（FMC Bank3）读写演示
 *
 * 预期现象：
 *   1) 上电串口(115200)打印 "UNISYM STM32H743 NAND demo"，随后打印
 *      "NAND ID: 0x2C 0xDC 0x90 0x95"（MT29F4G08 的 4 字节电子签名，
 *      首字节 0x2C 为镁光厂商码）。
 *   2) 按 KEYU(PA0)：向测试页(page 0)写入 2048 字节递增 pattern，
 *      串口打印 "Write page OK"（或 failed）。
 *   3) 按 KEYL(PC13)：读回测试页并十六进制打印前 32 字节——
 *      刚写完时显示 0x00..0x1F 的递增序列；擦除后显示全 0xFF。
 *   4) 按 KEYD(PH2)：擦除测试页所在块(block 0)，串口打印 "Erase block OK"；
 *      之后按 KEYL 读回应为全 0xFF。
 *   5) 按 KEYR(PH3)：打印测试页信息与总页数(getUnits)。
 *
 * 使用说明：
 *   1) NAND 挂在 FMC Bank3，地址窗 0x80000000（256MB），需经 MPU 配成
 *      非缓存+可缓冲（见 nand_mpu_open()），否则写后读会命中 L1 缓存、
 *      无法真实校验 NAND。
 *   2) 本演示为裸 NAND 驱动（无坏块管理/磨损均衡/ECC 纠错）；测试块 block 0
 *      会被擦除，属破坏性操作，仅供演示。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/Storage/FMC/FMC-Flash-NAND.hpp>
#include <c/prochip/CortexM7.h>// __DSB / __ISB
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

// FMC NAND 引脚复用（照参考 HAL_NAND_MspInit：PD/PE/PG 全部 AF12）
static void nand_gpio_raw(stduint base, const byte* pins, byte n) {
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

static void nand_gpio_config() {
	// 使能 GPIO D/E/G 时钟（AHB4ENR bits 3/4/6）
	RCC[RCCReg::AHB4ENR].setof(3, true);  // GPIOD
	RCC[RCCReg::AHB4ENR].setof(4, true);  // GPIOE
	RCC[RCCReg::AHB4ENR].setof(6, true);  // GPIOG
	// FMC NAND 8 位：D0~D7 = PD14/15/0/1/PE7/8/9/10；NOE=PD4，NWE=PD5，CLE=PD11，ALE=PD12，NCE3=PG9
	static const byte pd[] = {0,1,4,5,11,12,14,15}, pe[] = {7,8,9,10}, pg[] = {9};
	nand_gpio_raw(0x58020C00, pd, byteof(pd));  // GPIOD
	nand_gpio_raw(0x58021000, pe, byteof(pe));  // GPIOE
	nand_gpio_raw(0x58021800, pg, byteof(pg));  // GPIOG
	// PD6 = FMC_NWAIT (R/B)：本驱动用 0x70 状态轮询、不用 R/B 引脚，故不配置。
}

// MPU region for the NAND window 0x80000000 (256MB, non-cacheable, bufferable, full access).
// AKA HAL_MPU_ConfigRegion in the reference NAND_MPU_Config (Region3, 256MB).
static void nand_mpu_open() {
	Reference(0xE000ED94).rstof(0);          // MPU_CTRL disable
	Reference(0xE000ED98) = 3;               // RNR: region 3
	Reference(0xE000ED9C) = 0x80000000;      // RBAR: base address
	// RASR: ENABLE(0) | SIZE=27(256MB,1) | B(16) | AP=0x03(24, full access)
	Reference(0xE000EDA0) = (1u << 0) | (27u << 1) | (1u << 16) | (0x03u << 24);
	Reference(0xE000ED94) = (1u << 0) | (1u << 2); // MPU_CTRL enable + PRIVDEFENA
	__DSB();
	__ISB();
}

// NAND timing mode set（照参考 NAND_ModeSet）：0xEF SET FEATURE, 地址 0x01, 写 mode
#define _NAND_DEVICE  0x80000000
#define _NAND_CLE     (1 << 16)
#define _NAND_ALE     (1 << 17)
#define _NAND_FEATURE 0xEF
#define _NAND_READY   0x40
static void nand_modeset(byte mode) {
	*(volatile uint8_t*)(_NAND_DEVICE | _NAND_CLE)  = _NAND_FEATURE; __DSB();
	*(volatile uint8_t*)(_NAND_DEVICE | _NAND_ALE)  = 0x01; __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = mode; __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = 0;    __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = 0;    __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = 0;    __DSB();
	while (FMC_NAND.ReadStatus() != _NAND_READY) {}
}

static byte page_buf[2048];
static byte read_buf[2048];

#define DEMO_PAGE 0 // test page: block 0, page 0

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
	XART1.OutFormat("UNISYM STM32H743 NAND demo\r\n");

	// NAND window is External-RAM (cacheable by default on CM7) -> mark non-cacheable
	nand_mpu_open();
	// FMC NAND 引脚复用（AF12）：数据 D0~D7 / CLE / ALE / NOE / NWE / NCE3
	nand_gpio_config();

	// MT29F4G08 (512MB): 2048B main / 64B spare, 64 pages/block, 4096 blocks, 2 planes
	NANDConfig cfg;
	cfg.data_bus = NANDBus::Bits8;
	cfg.wait_feature = false;
	cfg.ecc_computation = false;
	cfg.ecc_page_size = 0x20000; // FMC_NAND_ECC_PAGE_SIZE_512BYTE
	cfg.tclr_setup_time = 10;
	cfg.tar_setup_time = 10;
	cfg.common_space.setup_time = 10;
	cfg.common_space.wait_setup_time = 10;
	cfg.common_space.hold_setup_time = 10;
	cfg.common_space.hiz_setup_time = 10;
	cfg.attribute_space.setup_time = 10;
	cfg.attribute_space.wait_setup_time = 10;
	cfg.attribute_space.hold_setup_time = 10;
	cfg.attribute_space.hiz_setup_time = 10;
	cfg.page_size = 2048;
	cfg.spare_area_size = 64;
	cfg.block_size = 64;
	cfg.block_nbr = 4096;
	cfg.plane_nbr = 2;
	cfg.plane_size = 2048;
	cfg.extra_command_enable = false;

	if (!FMC_NAND.setMode(cfg)) { XART1.OutFormat("NAND init failed\r\n"); erro(); }
	FMC_NAND.Reset();
	SysDelay_ms(100);

	NANDID id;
	FMC_NAND.ReadID(id);
	XART1.OutFormat("NAND ID: 0x%02X 0x%02X 0x%02X 0x%02X\r\n",
		(unsigned)id.maker_id, (unsigned)id.device_id,
		(unsigned)id.third_id, (unsigned)id.fourth_id);
	nand_modeset(4); // 切到 timing mode 4（高速）；否则 mode 0 写数据 tADL=200ns 跟不上，写不进

	NANDAddress pa{};          // page 0, block 0, plane 0
	pa.page = DEMO_PAGE;
	byte fill = 0;
	bool keyu_prev = true, keyl_prev = true, keyd_prev = true, keyr_prev = true;

	while (1) {
		bool ku = (bool)KEYU, kl = (bool)KEYL, kd = (bool)KEYD, kr = (bool)KEYR;
		if (ku && !keyu_prev) {                         // KEY_UP: write pattern to test page
			for (unsigned i = 0; i < 2048; i++) page_buf[i] = (byte)(i + fill);
			bool ok = FMC_NAND.Write(pa, page_buf, 1, NANDArea::Main, NANDBus::Bits8);
			XART1.OutFormat(ok ? "Write page OK\r\n" : "Write page failed\r\n");
			fill += 32;
		}
		if (!kl && keyl_prev) {                         // KEY2: read test page, hex dump 32 bytes
			if (!FMC_NAND.Read(pa, read_buf, 1, NANDArea::Main, NANDBus::Bits8))
				XART1.OutFormat("Read page failed\r\n");
			else {
				XART1.OutFormat("Page %u first 32 bytes:\r\n", DEMO_PAGE);
				for (unsigned i = 0; i < 32; i++) {
					XART1.OutFormat("%02X ", read_buf[i]);
					if ((i & 15) == 15) XART1.OutFormat("\r\n");
				}
			}
		}
		if (!kd && keyd_prev) {                         // KEY1: erase block 0
			NANDAddress ba{};
			bool ok = FMC_NAND.EraseBlock(ba);
			XART1.OutFormat(ok ? "Erase block OK\r\n" : "Erase block failed\r\n");
		}
		if (!kr && keyr_prev) {                         // KEY0: show test page info
			XART1.OutFormat("page=%u block=0 plane=0 units=%u\r\n",
				DEMO_PAGE, (unsigned)FMC_NAND.getUnits());
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
