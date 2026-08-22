// UTF-8 C++(ARMCLANG) TAB4 CRLF
// DAC1 ch1 (PA4) output changes once per second;
// ADC1 (PA5) reads the DAC output back (wire PA4 -> PA5) for verification.
// Both results are sent via XART1.

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
	XART1.OutFormat("Hello, %s %s on %s\n", "Example", "DAC1 PA4 <-> ADC1 PA5", _IDN_BOARD);
	
	// ---- DAC1 channel 1: PA4 ----
	DAC.enClock();
	DAC.enAble(GPIOA[4]);// ch1, output buffer disabled, connected to PA4
	DAC.setOutput(GPIOA[4], 0);
	
	// ---- ADC1: PA5 loopback ----
	ADC1.setMode(ADCRes::B16, 1);                       // 16-bit, 1 regular conversion, software trigger
	ADC1.setChannel(GPIOA[5], 0);                       // PA5 = ADC1 channel 19 (setChannel auto-maps)
	ADC1.enClock(true, 2);                              // CKPER default: ensures HSI, selects per_ck, presc /4
	ADC1.enDeepPowerDown(false);
	ADC1.enVoltageRegulator(true);
	ADC1.Calibrate();
	
	uint16 dacval = 0;
	while (1) {
		// DAC value changes once per second (0..3800, step 200)
		dacval = (dacval + 200) % 4000;
		DAC.setOutput(GPIOA[4], dacval);
		SysDelay_ms(10);// settle
		
		// read back the DAC output through ADC1 (average 10 samples)
		uint32 sum = 0;
		for0(i, 10) {
			ADC1.Start();
			if (!ADC1.Poll(0xFFFF)) break;
			sum += (uint32)ADC1.inn();
			SysDelay_ms(5);
		}
		uint32 adc_raw = sum / 10;
		uint32 dac_mv = (uint32)dacval * 3300 / 4095;   // 12-bit DAC, VREF = 3.3V
		uint32 adc_mv = adc_raw * 3300 / 65535;         // 16-bit ADC, VREF = 3.3V
		
		XART1.OutFormat("DAC set: %u (%u.%03u V) | ADC read: %u (%u.%03u V)\n",
			(unsigned)dacval, (unsigned)(dac_mv / 1000), (unsigned)(dac_mv % 1000),
			(unsigned)adc_raw, (unsigned)(adc_mv / 1000), (unsigned)(adc_mv % 1000));
		LEDB.Toggle();
		SysDelay_ms(1000);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
