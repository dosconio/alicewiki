// UTF-8 C++(ARMCLANG) TAB4 CRLF
// ADC1 samples PA5 (pot / external input), result sent via XART1

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

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();
	
	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);
	
	XART1.setMode(115200);
	XART1.OutFormat("Hello, %s %s on %s\n", "Example", "ADC1 PA5", _IDN_BOARD);
	
	// ---- ADC1: PA5, 16-bit, kernel clock = per_ck / 4 ----
	ADC1.setMode(ADCRes::B16, 1);                       // 16-bit, 1 regular conversion, software trigger
	ADC1.setChannel(GPIOA[5], 0);                       // PA5 = ADC1 channel 5
	ADC1.enClock(true, 2);                              // CKPER default: ensures HSI, selects per_ck, presc /4 (64MHz->16MHz)
	ADC1.enDeepPowerDown(false);                        // leave deep power-down
	ADC1.enVoltageRegulator(true);                      // enable ADC voltage regulator
	ADC1.Calibrate();                                   // offset calibration (enables ADC)
	
	while (1) {
		uint32 sum = 0;
		for0(i, 20) {                                   // 20 samples, average
			ADC1.Start();
			if (!ADC1.Poll(0xFFFF)) break;
			sum += (uint32)ADC1.inn();
			SysDelay_ms(5);
		}
		uint32 raw = sum / 20;
		uint32 mv = raw * 3300 / 65535;                 // VREF = 3.3V, 16-bit full scale
		XART1.OutFormat("ADC RAW: %u, VOL: %u.%03u V\n",
			(unsigned)raw, (unsigned)(mv / 1000), (unsigned)(mv % 1000));
		LEDB.Toggle();
		SysDelay_ms(500);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
