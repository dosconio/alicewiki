// #include "driver/IIC.h"
static void inline_HMC624A_delay(void) {
	for0(i, 0x100);
}

class HMC624A_t {
protected:
	// IIC_t IIC;
	GPIO_Pin& LE, & SDA, & SCL;
	byte ADDR;
public:
	HMC624A_t(GPIO_Pin& SDA, GPIO_Pin& SCL, GPIO_Pin& LE) :
		// IIC(SDA, SCL),
		LE(LE), SDA(SDA), SCL(SCL)
	{
		// IIC.func_delay = inline_HMC624A_delay;
		LE.setMode(GPIOMode::OUT_PushPull);
		SDA.setMode(GPIOMode::OUT_PushPull);
		SCL.setMode(GPIOMode::OUT_PushPull);
	}
	void setOutput(byte dB) {
		dB &= 0x3F;
		LE = !1;
		inline_HMC624A_delay();
		SCL = 0;
		for0(i, 6) {
			SDA = (dB & 0x20);
			dB <<= 1;
			SCL = 1;
			inline_HMC624A_delay();
			SCL.Toggle();
			inline_HMC624A_delay();
		}
		LE = !0;
		SDA = 0;
	}
};
