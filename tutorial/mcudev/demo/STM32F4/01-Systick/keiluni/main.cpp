// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "cpp/MCU/ST/STM32F4"
using namespace uni;

GPIO_Pin& LED = GPIOC[0];

int main() {
	LED.setMode(GPIOMode::OUT_PushPull);
	if (!RCC.setClock(SysclkSource::HSE)) while (true);
	
	while (true) {
		LED.Toggle();
		SysDelay(500);// ms
	}
}

