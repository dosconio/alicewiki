// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio (YangXuecong-20211492316)

#define  __stdlib_h
#define  _STYLE_RUST
#define BOARD_DETAIL "07VET6"
#include <stdio.h>//{TEMP}
#include "cpp/MCU/ST/STM32F4"
#include "cpp/string"
using namespace uni;

char buf[32];

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
void  memf(pureptr_t p){v p;}
char* StrHeapAppendChars(char* dest, char chr, size_t n) {v dest; v chr; v n; return nullptr;}

float adc_val;



void hand_adc() {
	ADC1 >> adc_val;
}

void init() {
	StrAppend(_IDN_BOARD, BOARD_DETAIL);
	LEDR.setMode(GPIOMode::OUT_PushPull);
	if (!RCC.setClock(SysclkSource::HSE)) while (true);
	XART1.setMode();// A9 and A10
	
	DAC.enAble(GPIOA[4]);
	DAC.enAble(GPIOA[5]);
	
	LEDR = !false;
	XART1 << "\nDOSCONIO HOMEWORK SYSTEM\n";
}

int main() {
	init();
	
	loop {
		static word w = 0;
		//SysDelay(1); 
		DAC.setOutput(GPIOA[4], (w+=1) % 0x1000);
		DAC.setOutput(GPIOA[5], (w+=1) % 0x1000);
		for0(i, 100);
	}
}

