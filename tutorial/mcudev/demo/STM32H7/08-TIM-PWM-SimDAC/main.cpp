// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 08-TIM-PWM-SimDAC：PWM 模拟 DAC（定时器 PWM + 板载 RC 低通滤波）
 *
 * 预期现象：
 *   1) 上电后串口打印标题；DS0（LEDB）每 200ms 翻转指示运行。
 *   2) TIM15_CH2（PA3）输出 390.625kHz PWM，经板载 RC 滤波得直流电压；
 *      ADC1（PA5）回读该电压。串口持续打印 DAC 设定值 / DAC 电压 / ADC 回读电压。
 *   3) 按 WK_UP：设定值 +10（电压升高）；按 KEY1：设定值 -10。
 *
 * 使用说明：
 *   1) PWM 频率 = 200MHz / 2 / 256 = 390.625kHz，8 位分辨率（0~255）。
 *   2) 设定值 pwmval 直接写 CCR2，范围 0~250；Vout = pwmval * 3.3 / 256 V。
 *   3) 需将 PA3（PWM 输出）经二阶 RC 低通接到 PA5（ADC 输入）。
 *   4) 硬件跳线（关键！否则 ADC 读不到 PWM DAC 值）：
 *      - 短接多功能接口 P11 的「ADC」与「PDC」两个跳线帽（把 PWM DAC 输出接入 ADC 自测试）；
 *      - 拔掉 P8 上 PA3(RX) 右侧的跳线帽，否则影响转换结果；
 *      - ADC/DAC 参考电压默认经 P5 连到 3.3V（VREF=3.3V）。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// WK_UP 按下=1
GPIN& KEYD = GPIOH[ 2];// KEY1  按下=0

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
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// WK_UP (PA0)
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);// KEY1 (PH2)

	XART1.setMode(115200);
	XART1.OutFormat("PWM DAC (TIM15_CH2=PA3) + ADC1 PA5\r\n");

	// ---- PWM DAC：TIM15 CH2 = PA3，200MHz/2/256 = 390.625kHz ----
	TIM15.setMode(2, 256);             // PSC=1, ARR=255
	TIM15.setChannel(2, 0, &GPIOA[3]); // CH2 = PA3（AF4，库内表自动配置），初始占空比 0

	// ---- ADC1：PA5，16 位，回读滤波后的直流电压 ----
	ADC1.setMode(ADCRes::B16, 1);
	ADC1.setChannel(GPIOA[5], 0);      // PA5 = ADC1 通道（引脚自动解析）
	ADC1.enClock(true, 2);
	ADC1.enDeepPowerDown(false);
	ADC1.enVoltageRegulator(true);
	ADC1.Calibrate();

	stduint pwmval = 0;
	TIM15[TimReg::CCR2] = pwmval;

	while (1) {
		// 按键调节：WK_UP +10，KEY1 -10（范围 0~250）
		if (KEYU) {
			if (pwmval < 250) pwmval += 10;
			TIM15[TimReg::CCR2] = pwmval;
		} else if (!KEYD) {
			pwmval = (pwmval > 10) ? pwmval - 10 : 0;
			TIM15[TimReg::CCR2] = pwmval;
		}

		// ADC 回读：20 次平均
		uint32 sum = 0;
		for0(i, 20) {
			ADC1.Start();
			if (!ADC1.Poll(0xFFFF)) break;
			sum += (uint32)ADC1.inn();
			SysDelay_ms(1);
		}
		uint32 raw = sum / 20;
		uint32 adcmv = raw * 3300 / 65535;   // 16 位满量程 3.3V
		uint32 dacmv = pwmval * 3300 / 256;  // 8 位 PWM 等效电压

		XART1.OutFormat("DAC=%u (%u.%03uV) | ADC=%u (%u.%03uV)\r\n",
			(unsigned)pwmval, (unsigned)(dacmv / 1000), (unsigned)(dacmv % 1000),
			(unsigned)raw, (unsigned)(adcmv / 1000), (unsigned)(adcmv % 1000));

		LEDB.Toggle();
		SysDelay_ms(200);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
