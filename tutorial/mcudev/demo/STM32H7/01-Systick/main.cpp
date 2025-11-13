#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[0];
GPIN& LEDR = GPIOB[1];

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(4); // 4 pre and 0 sub
	if (!RCC.setClock(SysclkSource::HSE)) erro();
	
	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	while (true) {
		LEDR.Toggle();
		LEDB.Toggle();
		SysDelay_ms(1000);
	}
}

void erro(char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
