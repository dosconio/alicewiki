#include "driver/IIC.h"
static void inline_MCP4725_delay(void) {
	for0(i, 0x100);
}

class MCP4725_t {
protected:
	IIC_t IIC;
	byte ADDR;
public:
	MCP4725_t(GPIO_Pin& SDA, GPIO_Pin& SCL, byte ADDR = (0xC0)) :// 0xC0 for GND short
		IIC(SDA, SCL), ADDR(ADDR)//, delay_ms(delay_ms) 
	{
		IIC.func_delay = inline_MCP4725_delay;
		SCL.setMode(GPIOMode::OUT_PushPull);
	}
	void setOutput(word percn) {
		IIC.SendStart();
		IIC << ADDR;
		IIC << 64;// Update Output
		IIC << byte(percn >> 4);// High
		IIC << byte(percn << 4);// Low
		IIC.SendStop();
	}
};
