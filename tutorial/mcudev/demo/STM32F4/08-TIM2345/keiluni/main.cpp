// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "cpp/MCU/ST/STM32F4"
using namespace uni;

GPIO_Pin& LED = GPIOC[0];
GPIO_Pin& KEY = GPIOC[1];

void hand_timc() { 
	LED.Toggle();
}

int main() {
	LED.setMode(GPIOMode::OUT_PushPull);
	if (!RCC.setClock(SysclkSource::HSE)) while (true);
	bool output_mode = false;
	if (output_mode) {
		// PWM Output {unchk}
		TIM2.setMode(10000);
		TIM2[1].setMode(5000);// define the pin here
	}
	else {
		TIM2.setMode(42);// 500ms-period for 168M
		TIM2.RuptTrait::enInterrupt(hand_timc);
	}
	while (true);
}
