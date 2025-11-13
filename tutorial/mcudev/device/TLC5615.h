#include "driver/SPI.h"
static void inline_TLC5615_delay(void) {
	for0(i, 0x100);
}

class TLC5615_t {
protected:
	SPI_t SPI;
public:
	TLC5615_t(GPIO_Pin& SCLK, GPIO_Pin& MOSI, GPIO_Pin& MISO, GPIO_Pin& CSEL) :
		SPI(SCLK, MOSI, MISO, CSEL)
	{
		SPI.func_delay = inline_TLC5615_delay;
	}
	void setOutput(word inp10b) {
		word VData = (inp10b * (1.024 / 2 / 2.5) + 0.5);//由目标电压mV，计算十位数值
		inp10b <<= 2;
		SPI.SendStart();
		SPI.Transceivex(inp10b, 12);
		SPI.SendStop();
	}
};
