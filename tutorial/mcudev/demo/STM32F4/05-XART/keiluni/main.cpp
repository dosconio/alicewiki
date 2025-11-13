// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "cpp/MCU/ST/STM32F4"
using namespace uni;

GPIO_Pin& LED = GPIOC[0];
GPIO_Pin& KEY = GPIOC[1];
void hand_uart() {
	int data;
	XART1 >> data;
	XART1 << data;
}

int main() {
	LED.setMode(GPIOMode::OUT_PushPull);
	if (!RCC.setClock(SysclkSource::HSE)) while (true);
	XART1.setMode();
	XART1.RuptTrait::enInterrupt(hand_uart);
	SysDelay(10);
	XART1 << "Hello~\n";
	while (true) {
		LED.Toggle();
		XART1 << ">>>\n";
		SysDelay(5000);
	}
}
