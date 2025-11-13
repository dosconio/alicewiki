#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];
GPIN& LEDR = GPIOB[ 1];
GPIN& KEYU = GPIOA[ 0];//   Up
GPIN& KEYL = GPIOC[13];// Left
GPIN& KEYD = GPIOH[ 2];// Down
GPIN& KEYR = GPIOH[ 3];//Right

void hand() {
	LEDR.Toggle();
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(4); // 4 pre and 0 sub
	if (!RCC.setClock(SysclkSource::HSE)) erro();
	
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIORupt::Anyedge);
	KEYU.setInterrupt(hand);
	KEYU.enInterrupt();
	while (true);
}

void erro(char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
