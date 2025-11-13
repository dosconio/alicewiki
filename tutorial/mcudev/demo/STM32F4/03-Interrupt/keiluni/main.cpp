// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "cpp/MCU/ST/STM32F4"
using namespace uni;

GPIO_Pin& LED = GPIOC[0];
GPIO_Pin& KEY = GPIOC[1];

void hand_key() { LED.Toggle(); }

int main() {
	if (!RCC.setClock(SysclkSource::HSE))
	{ while (true); }
	LED.setMode(GPIOMode::OUT_PushPull);
	KEY.setMode(GPIORupt::Anyedge, hand_key);
	LED = !false;
	KEY.enInterrupt();
	while (true);
}
