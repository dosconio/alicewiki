
// according to OpenEdv code1.0

#ifndef __USYM__
#include "M:/her/SVGN/unisym/inc/cpp/unisym"
#include "M:/her/SVGN/unisym/inc/cpp/Device/IIC"
#endif

#define ES8388_ADDR     0x10 

class ES8388_t {
protected:
	uni::IIC_t& iic;
public:
	ES8388_t(uni::IIC_t& _iic) : iic(_iic) {}
	// es8388_init
	void setOutput() {
		Send(0, 0x80);      /* 软复位ES8388 */
		Send(0, 0x00);
		SysDelay_ms(100);                  /* 等待复位 */
		//
		Send(0x01, 0x58);
		Send(0x01, 0x50);
		Send(0x02, 0xF3);
		Send(0x02, 0xF0);
		//
		Send(0x03, 0x09);   /* 麦克风偏置电源关闭 */
		Send(0x00, 0x06);   /* 使能参考 500K驱动使能 */
		Send(0x04, 0x00);   /* DAC电源管理，不打开任何通道 */
		Send(0x08, 0x00);   /* MCLK不分频 */
		Send(0x2B, 0x80);   /* DAC控制	DACLRC与ADCLRC相同 */
		//
		Send(0x09, 0x88);   /* ADC L/R PGA增益配置为+24dB */
		Send(0x0C, 0x4C);   /* ADC 数据选择为left data = left ADC, right data = left ADC  音频数据为16bit */
		Send(0x0D, 0x02);   /* ADC配置 MCLK/采样率=256 */
		Send(0x10, 0x00);   /* ADC数字音量控制将信号衰减 L  设置为最小！！！ */
		Send(0x11, 0x00);   /* ADC数字音量控制将信号衰减 R  设置为最小！！！ */
		//
		Send(0x17, 0x18);   /* DAC 音频数据为16bit */
		Send(0x18, 0x02);   /* DAC 配置 MCLK/采样率=256 */
		Send(0x1A, 0x00);   /* DAC数字音量控制将信号衰减 L  设置为最小！！！ */
		Send(0x1B, 0x00);   /* DAC数字音量控制将信号衰减 R  设置为最小！！！ */
		Send(0x27, 0xB8);   /* L混频器 */
		Send(0x2A, 0xB8);   /* R混频器 */
		SysDelay_ms(100);
	}

	// es8388 _write_reg
	bool Send(byte reg, byte val) {
		iic.SendStart();
		iic.Send((ES8388_ADDR << 1) | 0, true);  /* 发送器件地址+写命令 */
		iic.Send(reg, true);                     /* 写寄存器地址 */
		iic.Send(val & 0xFF, true);              /* 发送数据 */
		iic.SendStop();
		return true;
	}

	// es8388 _read_reg
	byte Read(byte reg)
	{
		iic.SendStart();
		iic.Send((ES8388_ADDR << 1) | 0, true);  /* 发送器件地址+写命令 */
		iic.Send(reg, true);                     /* 写寄存器地址 */
		//
		iic.SendStart();
		iic.Send((ES8388_ADDR << 1) | 1, true);  /* 发送器件地址+读命令 */
		byte temp = iic.ReadByte(0);
		//
		iic.SendStop();
		return temp;
	}

	/**
	 * @brief       设置ES8388工作模式
	 * @param       fmt : 工作模式
	 *    @arg      0, 飞利浦标准I2S;
	 *    @arg      1, MSB(左对齐);
	 *    @arg      2, LSB(右对齐);
	 *    @arg      3, PCM/DSP
	 * @param       len : 数据长度
	 *    @arg      0, 24bit
	 *    @arg      1, 20bit
	 *    @arg      2, 18bit
	 *    @arg      3, 16bit
	 *    @arg      4, 32bit
	 * @retval      无
	 */
	void sai_cfg(uint8_t fmt, uint8_t len)
	{
		fmt &= 0x03;
		len &= 0x07;                                    /* 限定范围 */
		Send(23, (fmt << 1) | (len << 3));  /* R23,ES8388工作模式设置 */
	}

	/**
	 * @brief       设置耳机音量
	 * @param       voluem : 音量大小(0 ~ 33)
	 * @retval      无
	 */
	void hpvol_set(uint8_t volume)
	{
		MIN(volume, 33);
		Send(0x2E, volume);
		Send(0x2F, volume);
	}
	/**
	 * @brief       设置喇叭音量
	 * @param       volume : 音量大小(0 ~ 33)
	 * @retval      无
	 */
	void spkvol_set(uint8_t volume)
	{
		MIN(volume, 33);
		Send(0x30, volume);
		Send(0x31, volume);
	}

	/**
	 * @brief       设置3D环绕声
	 * @param       depth : 0 ~ 7(3D强度,0关闭,7最强)
	 * @retval      无
	 */
	void setRounding3d(uint8_t depth)
	{
		depth &= 0x7;                           /* 限定范围 */
		Send(0x1D, depth << 2);     /* R7,3D环绕设置 */
	}

	/**
	 * @brief       ES8388 DAC/ADC配置
	 * @param       dacen : dac使能(1)/关闭(0)
	 * @param       adcen : adc使能(1)/关闭(0)
	 * @retval      无
	 */
	void adda_cfg(uint8_t dacen, uint8_t adcen)
	{
		uint8_t tempreg = 0;

		tempreg |= ((!dacen) << 0);
		tempreg |= ((!adcen) << 1);
		tempreg |= ((!dacen) << 2);
		tempreg |= ((!adcen) << 3);
		Send(0x02, tempreg);
	}

	/**
	 * @brief       ES8388 DAC输出通道配置
	 * @param       o1en : 通道1使能(1)/禁止(0)
	 * @param       o2en : 通道2使能(1)/禁止(0)
	 * @retval      无
	 */
	void output_cfg(uint8_t o1en, uint8_t o2en)
	{
		uint8_t tempreg = 0;
		tempreg |= o1en * (3 << 4);
		tempreg |= o2en * (3 << 2);
		Send(0x04, tempreg);
	}

	/**
	 * @brief       ES8388 MIC增益设置(MIC PGA增益)
	 * @param       gain : 0~8, 对应0~24dB  3dB/Step
	 * @retval      无
	 */
	void mic_gain(uint8_t gain)
	{
		gain &= 0x0F;
		gain |= gain << 4;
		Send(0x09, gain);       /* R9,左右通道PGA增益设置 */
	}

	/**
	 * @brief       ES8388 ALC设置
	 * @param       sel
	 *   @arg       0,关闭ALC
	 *   @arg       1,右通道ALC
	 *   @arg       2,左通道ALC
	 *   @arg       3,立体声ALC
	 * @param       maxgain : 0~7,对应-6.5~+35.5dB
	 * @param       mingain : 0~7,对应-12~+30dB 6dB/STEP
	 * @retval      无
	 */
	void alc_ctrl(uint8_t sel, uint8_t maxgain, uint8_t mingain)
	{
		uint8_t tempreg = 0;

		tempreg = sel << 6;
		tempreg |= (maxgain & 0x07) << 3;
		tempreg |= mingain & 0x07;
		Send(0x12, tempreg);     /* R18,ALC设置 */
	}

	/**
	 * @brief       ES8388 ADC输出通道配置
	 * @param       in : 输入通道
	 *    @arg      0, 通道1输入
	 *    @arg      1, 通道2输入
	 * @retval      无
	 */
	void input_cfg(uint8_t in)
	{
		Send(0x0A, (5 * in) << 4);   /* ADC1 输入通道选择L/R  INPUT1 */
	}

	
};
