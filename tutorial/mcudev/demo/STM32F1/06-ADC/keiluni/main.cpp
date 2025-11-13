// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "../../board.h"
#include <stdio.h>

bool init(void) {
	if (!RCC.setClock(SysclkSource::HSE))
		return false;
	LEDR.setMode(GPIOMode::OUT_PushPull);
	LEDG.setMode(GPIOMode::OUT_PushPull);
	LEDB.setMode(GPIOMode::OUT_PushPull);
	LEDR = LEDG = LEDB = !false;
	return true;
}

dword adc_res = 0;
char buf[33];
void hand_adc(void) { 
	ADC1 >> adc_res;
}

void ADC_by_interrupt() {
	GPIOC[1].setMode(GPIOMode::IN_Analog);
	ADC1.setMode();
	ADC1.setChannel(GPIOC[1]);
	ADC1.RuptTrait::enInterrupt(hand_adc);
	while (true) {
		SysDelay(500);
		sprintf(buf, "0x%04X \r\n", adc_res);
		XART1 << buf;
		LEDR.Toggle();
	}
}

word dma_buf[4];
void ADC_by_dma() {
	GPIOC[1].setMode(GPIOMode::IN_Analog);
	ADC1.setDMA(false, true /*timebase array*/);
	ADC1.setMode(0, false, 0, false, 8); // 72M / 8 = 9MHz, 55.5 cycles + 12.5 = 68 cycles,
	ADC1.setChannel(GPIOC[1]);
	ADC1.enDMA(dma_buf, numsof(dma_buf) - 1 /*debug len*/);// run this multi-times for once mode
	while (true) {
		SysDelay(500);
		sprintf(buf, "%04X %04X %04X %04X\r\n", dma_buf[0], dma_buf[1], dma_buf[2], dma_buf[3]);
		XART1 << rostr(buf); 
		LEDB.Toggle();
	}
}

int main() {
	if (!init()) while (true);
	XART1.setMode();
	const int method = 1; // 1 for INT, 0 for DMA
	
	if (method)
		ADC_by_interrupt();
	else ADC_by_dma();
	while (true);
}
