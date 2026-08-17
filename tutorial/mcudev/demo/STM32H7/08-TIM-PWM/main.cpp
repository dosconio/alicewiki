#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];// DS1
GPIN& LEDR = GPIOB[ 1];// DS0, TIM3_CH4 PWM output

// HAL 实验8: TIM3_PWM_Init(500-1, 200-1) -> PSC=199, ARR=499
// 200MHz / 200 / 500 = 2kHz PWM on TIM3_CH4(PB1 = DS0); initial pulse arr/2 (50%)
void PWMMode_Init() {
	TIM3.setMode(200, 500);// unisym: pass actual divisors (writes PSC=199, ARR=499)
	TIM3.setChannel(4, 499 / 2, &GPIOB[1]);// PWM1, pulse=249
	// ConfigChannel defaults to active-high; match HAL 实验8 OCPolarity=LOW
	TIM3.setOCPolarity(4, true);// CC4P=1 -> inverted output: CCR4=0 => LED off
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	PWMMode_Init();

	// breathing: DS0 brightness sweeps 0 -> 300 -> 0 (HAL 实验8 main loop)
	bool dir = true;// 1: increasing, 0: decreasing
	stduint led0pwmval = 0;
	while (1) {
		SysDelay_ms(5);
		if (dir) led0pwmval++; else led0pwmval--;
		if (led0pwmval > 300) dir = false;
		if (led0pwmval == 0) dir = true;
		TIM3[TimReg::CCR4] = led0pwmval;// HAL: TIM_SetTIM3Compare4
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
