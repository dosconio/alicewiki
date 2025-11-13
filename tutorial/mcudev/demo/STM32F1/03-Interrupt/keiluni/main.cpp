// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "../../board.h"

static void hand_key1(void) { LEDB.Toggle(); }
static void hand_key2(void) { LEDG.Toggle(); }

int main() {
	if (!RCC.setClock(SysclkSource::HSE))
	{ while (true); }
	LEDR.setMode(GPIOMode::OUT_PushPull);
	LEDG.setMode(GPIOMode::OUT_PushPull);
	LEDB.setMode(GPIOMode::OUT_PushPull);
	KEY1.setMode(GPIORupt::Posedge, hand_key1);
	KEY2.setMode(GPIORupt::Anyedge, hand_key2);
	LEDR = LEDG = LEDB = !false;
	KEY1.enInterrupt();
	KEY2.enInterrupt();
	while (true);
}
