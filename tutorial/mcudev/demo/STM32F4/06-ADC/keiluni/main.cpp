// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio (YangXuecong-20211492316)

#define  __stdlib_h
#define  _STYLE_RUST
#define BOARD_DETAIL "07VET6"
#include <stdio.h>//{TEMP}
#include "cpp/MCU/ST/STM32F4"
#include "cpp/string"
#include "c/consio.h"
using namespace uni;

// Board: Mo-nv-board (http://www.stm32er.com/)
GPIO_Pin& LEDR = GPIOC[5];
GPIO_Pin& LEDB = GPIOB[2];
GPIO_Pin& KEY1 = GPIOA[0];
GPIO_Pin& KEY2 = GPIOA[1];
GPIO_Pin& KEY3 = GPIOA[4];

// * Interrupt Single Sample

#define v (void)

symbol_t pro01, pro02, pro03, pro04, pro05;
char* StrHeap(const char* a){v a; return nullptr;}
char* StrHeapAppendChars(char* dest, char chr, size_t n) {v dest; v chr; v n; return nullptr;}
extern "C" void outtxt(const char* str, stduint len) {
	for0(i, len) {
		XART1 << stduint(str[i]);
		for0(i, _TEMP 1000);
	}
	_crt_out_cnt += len;
}


float adc_val;

void hand_adc() {
	ADC1 >> adc_val;
}

void init() {
	StrAppend(_IDN_BOARD, BOARD_DETAIL);
	LEDR.setMode(GPIOMode::OUT_PushPull);
	if (!RCC.setClock(SysclkSource::HSE)) while (true);
	XART1.setMode();// A9 and A10
	
	LEDR = !false;
	XART1 << "\nDOSCONIO HOMEWORK SYSTEM\n";
}

int main() {
	init();
	ADC1.setMode();
	ADC1.setChannel(GPIOB[0]);// Chan 8
	ADC1.RuptTrait::enInterrupt(hand_adc);
	loop {
		LEDR.Toggle(); 
		SysDelay(500); 
		outsfmt("%f\n", adc_val * 3.3);
	}
}

