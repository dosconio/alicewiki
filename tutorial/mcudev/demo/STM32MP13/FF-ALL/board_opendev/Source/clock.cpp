#include <cpp/MCU/ST/STM32MP13>
#include <c/driver/RCC/RCC-registers.hpp>

using namespace uni;
extern bool useDDR;

_ESYM_C void erro();

bool init_ddr() {
	RCC.CSI.enAble(true);
	RCC.enSyscfg(true);
	MCE.enClock();
	TZC.enClock();
	TZC[TZCReg::GATE_KEEPER] = 0;
	TZC[TZCReg::REG_ID_ACCESSO] = 0xFFFFFFFF;// Allow DDR Region0 R/W  non secure for all IDs
	TZC[TZCReg::REG_ATTRIBUTESO] = 0xC0000001;
	TZC[TZCReg::GATE_KEEPER] |= 1;// Enable the access in secure Mode. filter 0 request close
	// Enable ETZPC & BACKUP SRAM for security
	ETZPC.enClock();
	BKPSRAM.enClock();
	BSEC[BSECReg::BSEC_DENABLE] = 0x47f;
	if (!DDR.setMode(DDRType::DDR3, DDR_t::DDRSize_256M)) return false;
	// Check DDR Write/Read
	{
		Letvar(tmpp, uint32*, 0xC0000000);
		tmpp[0] = 0x55AAAA55;
		return *tmpp == 0x55AAAA55;
	}
}

bool init_clock() {
	using namespace RCCReg;
	RCC.Sysclock.getCoreFrequency();
	SysTick::enClock(1000);
	RCC.canMode();
	PWR.setDBP(true);
	// AKA __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_MEDIUMHIGH);
	RCC[BDCR].maset(_IMM(_BDCR::LSEDRV), 2, 0b10);
	// OSC PART:  Order: HSE HSI CSI LSI LSE PLL
	uni::RCC.HSE.setMode(false);// no-bypass
	uni::RCC.HSI.setMode(0, 0);// DIV1 and Cali(0x0)
	uni::RCC.CSI.setMode(0x10);
	uni::RCC.LSI.setMode();
	uni::RCC.LSE.setMode(false);// no-bypass

	if (!RCC.PLL1.setModePLL12(PLLMode::Fractional, PLL1Source::HSE,
		.m=3, .n=81, .p=1, .q=2, .r=2, .frac=0x800)) erro();

	if (!RCC.PLL2.setModePLL12(PLLMode::Fractional, PLL2Source::HSE,
		//.m=3, .n=66, .p=2, .q=2, .r=1, .frac=0x1400)) erro();
		//.m=3, .n=50, .p=2, .q=2, .r=1, .frac=0)) erro();// for DDR 400M
		//.m=3, .n=62, .p=2, .q=2, .r=1, .frac=4096)) erro();// for DDR 500M
		.m=3, .n=66, .p=2, .q=2, .r=1, .frac=5120)) erro();// for 533M

	bool if_range3 = true;
	if (!RCC.PLL3.setModePLL34(PLLMode::Fractional, PLL3Source::HSE, if_range3,
		.m=2, .n=34, .p=2, .q=17, .r=2, .frac=0x1a04)) erro();

	bool if_range4 = true;
	if (!RCC.PLL4.setModePLL34(PLLMode::Fractional, PLL4Source::HSE, if_range4,
		.m=2, .n=38, .p=12, .q=14, .r=6, .frac=4096)) erro();// 33M for LTDC (.Q)
		//.m=2, .n=35, .p=12, .q=14, .r=6, .frac=0)) erro();   // 30M for LTDC

	bool state = true;

	// BUS PART:  Order: MPU ACLK HCLK PCLK4 PCLK5 PCLK1 PCLK2 PCLK3 PCLK6
	asserv(state) = RCC.Sysclock.setMode(SysclkSource::PLL1, 1);// PLL1 DIV2
	else erro();
	asserv(state) = RCC.AXIS.setMode(AxisSource::PLL2, 1);// PLL2 DIV1
	else erro();
	asserv(state) = RCC.MLAHB.setMode(MLAHBSource::PLL3, 0);// PLL3 DIV1
	else erro();
	// APBs
	RCC.APB4.setMode(1);// PCLK4 RCC_APB4_DIV2
	RCC.APB5.setMode(2);// PCLK5 RCC_APB5_DIV4
	RCC.APB1.setMode(1);// PCLK1 RCC_APB1_DIV2
	RCC.APB2.setMode(1);// PCLK2 RCC_APB2_DIV2
	RCC.APB3.setMode(1);// PCLK3 RCC_APB3_DIV2
	RCC.APB6.setMode(1);// PCLK6 RCC_APB6_DIV2
	// {unchk}
	bool compensate = false;
	// Note : The activation of the I/O Compensation Cell is recommended with communication  interfaces
	// (GPIO, SPI, FMC, XSPI ...)  when  operating at [HIGH frequencies] (refer to product datasheet)
	// The I/O Compensation Cell activation  procedure requires :
	// - The activation of the CSI clock
	// - The activation of the SYSCFG clock
	// - Enabling the I/O Compensation Cell : setting bit[0] of register SYSCFG_CCCSR
	// To do this please enable the block
	if (compensate) {
		//{} __HAL_RCC_CSI_ENABLE() ;
		//{} __HAL_RCC_SYSCFG_CLK_ENABLE() ;
		//{} HAL_EnableCompensationCell();
	}
	return true;
}

extern "C" { void Error_Handler(void) { erro(); } }
extern "C" { void assert_failed(void) { erro(); } }
