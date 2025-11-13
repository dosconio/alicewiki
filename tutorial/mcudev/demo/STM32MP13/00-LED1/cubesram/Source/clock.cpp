#include <cpp/MCU/ST/STM32MP13>
#include <c/driver/RCC/RCC-registers.hpp>

using namespace uni;
extern bool useDDR;

_ESYM_C void erro();

static bool init_clock_no_ddr() {
	using namespace RCCReg;
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
		.m=3, .n=66, .p=2, .q=2, .r=1, .frac=0x1400)) erro();

	bool if_range3 = true;
	if (!RCC.PLL3.setModePLL34(PLLMode::Fractional, PLL3Source::HSE, if_range3,
		.m=2, .n=34, .p=2, .q=17, .r=2, .frac=0x1a04)) erro();

	bool if_range4 = true;
	if (!RCC.PLL4.setModePLL34(PLLMode::Integer, PLL4Source::HSE, if_range4,
		.m=2, .n=50, .p=12, .q=60, .r=6, .frac=0)) erro();

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
	//
	// Note : The activation of the I/O Compensation Cell is recommended with communication  interfaces
	// (GPIO, SPI, FMC, XSPI ...)  when  operating at  high frequencies(please refer to product datasheet)
	// The I/O Compensation Cell activation  procedure requires :
	// - The activation of the CSI clock
	// - The activation of the SYSCFG clock
	// - Enabling the I/O Compensation Cell : setting bit[0] of register SYSCFG_CCCSR
	// To do this please enable the block
	if (false) {
		//{} __HAL_RCC_CSI_ENABLE() ;
		//{} __HAL_RCC_SYSCFG_CLK_ENABLE() ;
		//{} HAL_EnableCompensationCell();
	}
	return true;
}

bool init_clock(void) {
	RCC.Sysclock.getCoreFrequency();
	SysTick::enClock(1000);//SystemClock_Config_NoDDR();//{} if (!RCC.setClock(SysclkSource::HSE)) while (true);
	if (useDDR) _TODO return false;
	return init_clock_no_ddr();
	return true;
}

extern "C" { void erro(void) { loop; } }
extern "C" { void Error_Handler(void) { erro(); } }
