// ASCII
// @dosconio 20241007
#include "../official_board_p401r.hpp"

_ESYM_C char _IDN_BOARD[16] = "01R";// MSP432-P401R


int main()
{
	WdogA.HoldTimer();
	LED0.setMode(GPIOMode::OUT_PushPull);
	LEDR.setMode(GPIOMode::OUT_PushPull);
	LEDG.setMode(GPIOMode::OUT_PushPull);
	LEDB.setMode(GPIOMode::OUT_PushPull);
	LED0 = LEDR = LEDG = LEDB = false;
	while (true) {
		static byte b = 0;
		if (b < 2) LED0.Toggle();
		else if (b < 4) LEDR.Toggle();
		else if (b < 6) LEDG.Toggle();
		else if (b < 8) LEDB.Toggle();
		else b = 0xFF;
		for0(i, 100000); b++;
	}
}

_ESYM_C void HardFault_Handler(void) {
	LED0.setMode(GPIOMode::OUT_PushPull);
	while (true) { LED0.Toggle(); for0(i, 100000){} }
}
