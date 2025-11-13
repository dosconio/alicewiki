// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "../../board.h"
#include <stdio.h>

bool init(void) {
	if (!RCC.setClock(SysclkSource::HSE))
		return false;
	LEDR.setMode(GPIOMode::OUT_PushPull);
	LEDG.setMode(GPIOMode::OUT_PushPull);
	LEDB.setMode(GPIOMode::OUT_PushPull);
	LEDR = LEDG = LEDB = !false;
	return true;
}

word buf[128];

word wavegen_angle(word limit, word range, word index) {
	if (range >>= 1); else return 0;
	if (index < range)
		return limit / range * index;
	else return limit - limit / range * (index-range+1);
}

int main() {
	if (!init()) while (true);
	DAC.enDMA(GPIOA[5], DACTrigger::T6, false, buf, numsof(buf));
	for0a(i, buf) {
		buf[i] = wavegen_angle(0x1000, numsof(buf), i);
	}
	while (true);
}
