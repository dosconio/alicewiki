// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "../../board.h"

bool init(void) {
	if (!RCC.setClock(SysclkSource::HSE))
		return false;
	LEDR.setMode(GPIOMode::OUT_PushPull);
	LEDG.setMode(GPIOMode::OUT_PushPull);
	LEDB.setMode(GPIOMode::OUT_PushPull);
	LEDR = LEDG = LEDB = !false;
	return true;
}

void my_uart() {
	int data;
	XART1 >> data;
	XART1 << data + 1;
}
int main() {
	if (!init()) while (true);
	XART1.setMode(9600);
	XART1.RuptTrait::enInterrupt(my_uart);
	SysDelay(1);
	XART1 << "Hello~\n";
	while (true) {
 		LEDR.Toggle();
		XART1 << ">>>\n";
		SysDelay(1000);
	}
}
