// UTF-8 C++(ARMCC-5) TAB4 CRLF
// @dosconio
#include "../../board.h"

void loop() {
	static byte idx = 0;
	LEDB = idx & 0x01;
	LEDG = idx & 0x02;
	LEDR = idx & 0x04;
	Ranginc(idx, 2*2*2);
}

int main() {
	LEDR.setMode(GPIOMode::OUT_PushPull);
	LEDG.setMode(GPIOMode::OUT_PushPull);
	LEDB.setMode(GPIOMode::OUT_PushPull);
	for0(i,6) {
		loop();
		for0 (i, 2000000);// around 1s
	}
	RCC.setClock(SysclkSource::HSE);
	for0(i,6) {
		loop();
		for0 (i, 2000000);
	}
	while (true) {
		loop();
		SysDelay(1000);// 1s
	}
}
