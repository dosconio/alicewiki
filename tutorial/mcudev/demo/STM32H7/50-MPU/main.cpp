// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 50-MPU：内存保护单元（MPU）演示
 *
 * 预期现象：
 *   1) 上电串口打印 "UNISYM STM32H743 MPU demo"。
 *   2) 按 KEYU(PA0)：配置 MPU Region0 保护 0x2407F000 的 128 字节区域，
 *      （只读、不可共享、不可缓存、可缓冲），串口打印 "MPU open"。
 *   3) 按 KEYR(PH3)：向 0x2407F000 写入数据——
 *      - MPU 未开启：写成功，串口打印 "Write OK: 0x5A"。
 *      - MPU 已开启（只读）：触发 MemManage 内存访问错误，进入 MemManage_Handler，
 *        点亮 DS1、串口打印 "Mem Access Error!!"，延时后软复位重启。
 *   4) 按 KEYD(PH2)：从 0x2407F000 读一个字节，无论 MPU 是否开启都能读，
 *      串口打印 "Read: 0xXX"。
 *
 * 使用说明：
 *   1) 被保护区域位于 AXI SRAM D1 末尾（0x2407F000），128 字节，MPU Region0。
 *      AXI SRAM 经过 MPU；DTCM（0x20000000）走 TCM 接口、绕过 MPU，故不用它。
 *      程序数据/栈在 AXI SRAM 低地址（≤0x24002830），末尾区域空闲，不会被占用。
 *   2) MPU 寄存器：CTRL=0xE000ED94 / RNR=0xE000ED98 / RBAR=0xE000ED9C / RASR=0xE000EDA0。
 *   3) 软复位：写 SCB AIRCR（0xE000ED0C）VECTKEY|SYSRESETREQ。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <c/prochip/CortexM7.h>// __DSB / __ISB
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// KEY_UP (PA0, WKUP) 按下=1
GPIN& KEYD = GPIOH[ 2];// KEY1 按下=0
GPIN& KEYR = GPIOH[ 3];// KEY0 按下=0

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// Cortex-M7 MPU registers
#define _MPU_CTRL_ADDR 0xE000ED94// MPU_CTRL (0xE000ED90 is MPU_TYPE, read-only)
#define _MPU_RNR_ADDR  0xE000ED98
#define _MPU_RBAR_ADDR 0xE000ED9C
#define _MPU_RASR_ADDR 0xE000EDA0

// protected region: AXI SRAM D1 end 0x2407F000, 128 bytes, read-only.
// AXI SRAM passes through the MPU; DTCM (0x20000000) would bypass it (TCM interface).
// Program data/stack live at low AXI SRAM (<=0x24002830), so this end region is free.
#define _MPU_PROTECT_BASE 0x2407F000

// AKA MPU_Set_Protection: Region0, 128B, PRIV_RO_URO, not shareable, not cacheable, bufferable
static void mpu_open() {
	Reference(0xE000ED24) |= (1u << 16);         // SHCSR.MEMFAULTENA: route MPU fault to MemManage_Handler
	Reference(_MPU_CTRL_ADDR).rstof(0);          // HAL_MPU_Disable
	Reference(_MPU_RNR_ADDR)  = 0;               // Region number 0
	Reference(_MPU_RBAR_ADDR) = _MPU_PROTECT_BASE; // base address
	// RASR: ENABLE(0) | SIZE=6(1,128B) | B(16) | AP=0x06(24, read-only) | TEX=0 | S/C=0 | XN=0
	Reference(_MPU_RASR_ADDR) = (1u << 0) | (6u << 1) | (1u << 16) | (0x06u << 24);
	Reference(_MPU_CTRL_ADDR) = (1u << 0) | (1u << 2); // HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT): ENABLE(0)|PRIVDEFENA(2)
	__DSB();
	__ISB();
}

// MemManage fault handler (MPU region violation); vector table already references it
extern "C" void MemManage_Handler() {
	XART1.OutFormat("Mem Access Error!!\r\n");
	LEDR.setMode(GPIOMode::OUT) = false;// light DS1
	for (volatile unsigned i = 0; i < 1000000; i++) {}// busy-wait (SysTick masked in handler mode)
	XART1.OutFormat("Soft Reseting...\r\n");
	for (volatile unsigned i = 0; i < 1000000; i++) {}
	__DSB();                                    // ensure pending writes complete (AKA NVIC_SystemReset)
	Reference(0xE000ED0C) = (0x5FAu << 16) | (1u << 2);// AIRCR: VECTKEY + SYSRESETREQ
	__DSB();
	while (true) {}
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true);

	XART1.setMode(115200);
	SysDelay_ms(500);
	XART1.OutFormat("UNISYM STM32H743 MPU demo\r\n");

	volatile byte* mpudata = (volatile byte*)_MPU_PROTECT_BASE;
	bool mpu_on = false;
	bool keyu_prev = true, keyd_prev = true, keyr_prev = true;

	while (1) {
		bool ku = (bool)KEYU, kd = (bool)KEYD, kr = (bool)KEYR;
		if (ku && !keyu_prev) {            // KEY_UP: enable MPU protection
			mpu_open();
			mpu_on = true;
			XART1.OutFormat("MPU open\r\n");
		}
		if (!kr && keyr_prev) {            // KEY0: write to protected region
			*mpudata = 0x5A;               // faults when MPU read-only is on
			XART1.OutFormat("Write OK: 0x5A\r\n");
		}
		if (!kd && keyd_prev) {            // KEY1: read from protected region
			XART1.OutFormat("Read: 0x%02X\r\n", (unsigned)*mpudata);
		}
		keyu_prev = ku;
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
