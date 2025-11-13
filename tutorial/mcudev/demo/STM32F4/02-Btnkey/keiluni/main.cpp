// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "cpp/MCU/ST/STM32F4"
using namespace uni;

GPIO_Pin& LED = GPIOC[0];
GPIO_Pin& KEY = GPIOC[1];

int main() {
	if (!RCC.setClock(SysclkSource::HSE)) while (true);
	LED.setMode(GPIOMode::OUT_PushPull);
	KEY.setMode(GPIOMode::IN_Floating);
	while (true) {
		LED = !KEY;
	}
}

