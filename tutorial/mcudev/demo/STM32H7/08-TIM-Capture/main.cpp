#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SysTick>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// ============================================================
// 08-TIM-Capture 预期现象 (HAL 实验9 输入捕获 + 实验10 电容触摸按键, 合一)
// 硬件: 阿波罗 STM32H7; 串口 115200; 需 P11 跳线帽连接 ADC&TPAD
// 1. 上电校准: 串口打印 "tpad_default_val:xxx" (无触摸充电脉宽, 25MHz 计数)
// 2. DS0 (PB1): TIM3_CH4 PWM 呼吸, CCR4 0->300->0 循环, 亮度周期变化 (实验9)
// 3. 串口持续打印 "HIGH:xxx us": 每次 TPAD 充电脉宽的输入捕获结果 (实验9)
// 4. DS1 (PB0): 触摸 TPAD (PA5) 翻转一次 (实验10, 3 周期防抖)
//    触摸 -> 电容变大 -> 充电脉宽变长 -> HIGH 值明显大于 tpad_default_val
// ============================================================

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];// DS1: touch indicator (HAL 实验10 LED1)
GPIN& LEDR = GPIOB[ 1];// DS0: TIM3_CH4 PWM output, breathing (HAL 实验9 keeps 实验8 PWM)

char _buf[64]; String buf(_buf, byteof(_buf));

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// ---- 实验9 input capture state machine on TIM2_CH1 (falling-first) ----
// [7]:0 no capture done; 1 done. [6]:0 no first edge yet; 1 got first edge.
// [5:0]: overflow count after the first edge (HAL TIM5CH1_CAPTURE_STA semantics)
volatile byte CAPTURE_STA = 0;
volatile stduint CAPTURE_VAL = 0;

// TIM2 update interrupt: overflow counting (HAL HAL_TIM_PeriodElapsedCallback)
void hand_tim2_update() {
	if ((CAPTURE_STA & 0x80) == 0) {
		if (CAPTURE_STA & 0x40) {
			if ((CAPTURE_STA & 0x3F) == 0x3F) {// pulse too long
				CAPTURE_STA |= 0x80;
				CAPTURE_VAL = 0xFFFFFFFF;
			}
			else CAPTURE_STA++;
		}
	}
}

// TIM2 capture interrupt (CC1IF): first edge = discharge falling (re-arm, polarity->rising),
// second edge = charge threshold rising (ends timing) -> VAL = charge time (ticks)
void hand_tim2_capture() {
	if ((CAPTURE_STA & 0x80) == 0) {
		if (CAPTURE_STA & 0x40) {// second edge (rising): complete
			CAPTURE_STA |= 0x80;
			CAPTURE_VAL = TIM2.ReadCapture(1);
			TIM2[TimReg::CCER].setof(1, true);// CC1P back to falling
		}
		else {// first edge (falling): start timing
			CAPTURE_STA = 0;
			CAPTURE_VAL = 0;
			CAPTURE_STA |= 0x40;
			TIM2.enAble(false);
			TIM2[TimReg::CNT] = 0;
			TIM2[TimReg::CCER].setof(1, false);// CC1P switch to rising
			TIM2.enAble();
		}
	}
}

// ---- 实验10 TPAD driver: PA5 = TIM2_CH1, charge/discharge the touch pad ----
stduint tpad_default_val = 0;// no-touch charge time (ticks)

void TPAD_Discharge() {
	GPIOA[5].setMode(GPIOMode::OUT_PushPull, GPIOSpeed::Veryhigh);
	GPIOA[5] = false;// drive low: falling edge re-arms the capture state machine
}
void TPAD_Release() {
	GPIOA[5].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh);// TIM2_CH1 input
}

// one charge-pulse measurement: returns the captured charge time in ticks
stduint TPAD_Get_Val() {
	if (CAPTURE_STA & 0x80) CAPTURE_STA = 0;// consume stale result first
	TPAD_Discharge();// falling edge re-arms the state machine (polarity -> rising)
	SysDelay_ms(5);// discharge settle (HAL TPAD_Reset)
	// HAL TPAD_Reset order: start timing AFTER the discharge settle, right at release
	TIM2[TimReg::CNT] = 0;// exclude the 5ms discharge from the measurement
	TIM2[TimReg::SR] = 0;// clear any latched flags during discharge
	TPAD_Release();// pad charges -> rising edge completes the capture
	uint64 tickstart = SysTick::getTick();
	while (!(CAPTURE_STA & 0x80)) {
		if (SysTick::getTick() - tickstart > 2) break;// ~2ms timeout
	}
	if (CAPTURE_STA & 0x80) {
		stduint val = CAPTURE_VAL;
		CAPTURE_STA = 0;
		return val;
	}
	return (stduint)TIM2[TimReg::CNT];// timeout: return counter (HAL TPAD_Get_Val same)
}

// HAL 实验10 TPAD_Init: TIM2_CH1 at 200MHz/8 = 25MHz + 10-sample calibration
void TPAD_Init(byte psc) {
	TIM2.setMode(psc, 0xFFFFFFFF);// PSC = psc-1
	TIM2[TimReg::ARR] = 0xFFFFFFFF;// match HAL Period exactly
	TIM2.ConfigChannelInn(1, TimChinSel::Direct, TimIcPol::Falling, 0, 0);
	GPIOA[5].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh);
	GPIOA[5]._set_alternate(1);// TIM2_CH1 (AF1)
	// normalize: force the pad low, then let it charge high, so the first
	// discharge of TPAD_Get_Val always produces a clean falling edge
	GPIOA[5].setMode(GPIOMode::OUT_PushPull, GPIOSpeed::Veryhigh) = false;
	SysDelay_ms(5);
	GPIOA[5].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh);
	SysDelay_ms(1);
	TIM2[TimReg::SR] = 0;// clear latched flags
	TIM2[TimReg::CNT] = 0;
	// capture + update interrupts on the TIM2 NVIC line
	TIM2.FUNC_IC_Capture = hand_tim2_capture;
	TIM2.setInterrupt(hand_tim2_update);
	TIM2.setInterruptPriority(2, 0);
	TIM2.enCCInterrupt(1, true);// DIER.CC1IE
	TIM2.enInterrupt();// DIER.UIE + NVIC + CEN
	// calibration: 10 samples, sort, average the middle 6 (HAL TPAD_Init)
	stduint buf[10];
	for (byte i = 0; i < 10; i++) { buf[i] = TPAD_Get_Val(); SysDelay_ms(10); }
	for (byte i = 0; i < 9; i++)
		for (byte j = i + 1; j < 10; j++)
			if (buf[i] > buf[j]) { stduint tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp; }
	uint64 sum = 0;
	for (byte i = 2; i < 8; i++) sum += buf[i];
	tpad_default_val = (stduint)(sum / 6);
	XART1.OutFormat("tpad_default_val:%d\r\n", tpad_default_val);
}

// HAL 实验8/9 PWM: TIM3_CH4 -> PB1(DS0), 200MHz/200/500 = 2kHz
void PWMMode_Init() {
	TIM3.setMode(200, 500);// PSC=199, ARR=499
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
	XART1.setMode(115200);
	TPAD_Init(8);// TIM2_CH1 input capture + calibration

	static byte keyen = 0;// touch lockout (HAL TPAD_Scan mode=0)
	while (1) {
		SysDelay_ms(10);
		// 实验9: sawtooth PWM duty on DS0 (CCR4 cycles 0 -> 300 -> 0)
		stduint ccr = TIM3.ReadCapture(4);
		if (ccr == 300) ccr = 0; else ccr++;
		TIM3[TimReg::CCR4] = ccr;
		// 实验9 + 实验10: one TPAD charge-pulse capture (25MHz ticks -> us)
		stduint rval = TPAD_Get_Val();
		XART1.OutFormat("HIGH:%[64I] us\r\n", (sint64)(rval / 25));
		// 实验10: touch detection (HAL TPAD_Scan(0): threshold + lockout)
		if (rval > tpad_default_val * 4 / 3 && rval < 10 * tpad_default_val) {
			if (keyen == 0) LEDB.Toggle();// DS1 on touch
			keyen = 3;
		}
		if (keyen) keyen--;
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
