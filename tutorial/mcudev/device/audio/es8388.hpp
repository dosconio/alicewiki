// ASCII CPP-ISO11 TAB4 CRLF
// Docutitle: (Device/Audio) ES8388 Audio Codec Driver
// Codifiers: @dosconio: 2026xxxx~;
// Attribute: Arn-Covenant Any-Architect Env-Freestanding Non-Dependence
// Copyright: UNISYM, under Apache License 2.0

#ifndef _INC_DEVICE_AUDIO_ES8388_HPP
#define _INC_DEVICE_AUDIO_ES8388_HPP

#ifndef __USYM__
#include <cpp/unisym>
#include <cpp/Device/IIC>
#include <cpp/Device/SysTick>
#include <cpp/System/Audiosys.hpp>
#endif

#define ES8388_ADDR 0x10

class ES8388_t : public uni::SubACI {
public:
	enum class I2SFormat : byte {
		Standard = 0,// Philips I2S
		MSB = 1,     // Left Justified
		LSB = 2,     // Right Justified
		PCM = 3      // DSP / PCM
	};
	enum class I2SLength : byte {
		Bit24 = 0,
		Bit20 = 1,
		Bit18 = 2,
		Bit16 = 3,
		Bit32 = 4
	};
protected:
	uni::IIC_t& iic;
	byte dev_addr = ES8388_ADDR;
public:
	ES8388_t(uni::IIC_t& _iic) : iic(_iic) {}

	bool Send(byte reg, byte val) {
		iic.SendStart(dev_addr);
		iic.Send(reg, true);
		iic.Send(val, true);
		iic.SendStop();
		return (iic.getError() == ERR_IIC_NONE);
	}

	byte Read(byte reg) {
		iic.SendStart(dev_addr);
		iic.Send(reg, true);
		iic.SendStart();
		iic.Send((byte)((dev_addr << 1) | 1), true);
		byte temp = iic.ReadByte(true, false);
		iic.SendStop();
		return temp;
	}

	bool Initialize() {
		// 与正点原子 阿波罗 H743 官方 es8388.c (es8388_init) 逐寄存器一致
		Send(0x00, 0x80);      /* 软复位ES8388 */
		Send(0x00, 0x00);
		SysDelay_ms(100);      /* 等待复位 */

		Send(0x01, 0x58);
		Send(0x01, 0x50);
		Send(0x02, 0xF3);
		Send(0x02, 0xF0);

		Send(0x03, 0x09);      /* 麦克风偏置电源关闭 */
		Send(0x00, 0x06);      /* 使能参考 500K驱动使能 */
		Send(0x04, 0x00);      /* DAC电源管理，不打开任何通道 */
		Send(0x08, 0x00);      /* MCLK不分频 */
		Send(0x2B, 0x80);      /* DAC控制 DACLRC与ADCLRC相同 */

		Send(0x09, 0x88);      /* ADC L/R PGA增益配置为+24dB */
		Send(0x0C, 0x4C);      /* ADC 数据选择 音频数据为16bit */
		Send(0x0D, 0x02);      /* ADC配置 MCLK/采样率=256 */
		Send(0x10, 0x00);      /* ADC数字音量 L */
		Send(0x11, 0x00);      /* ADC数字音量 R */

		Send(0x17, 0x18);      /* DAC 音频数据为16bit */
		Send(0x18, 0x02);      /* DAC 配置 MCLK/采样率=256 */
		Send(0x1A, 0x00);      /* DAC数字音量 L */
		Send(0x1B, 0x00);      /* DAC数字音量 R */
		Send(0x27, 0xB8);      /* L混频器 */
		Send(0x2A, 0xB8);      /* R混频器 */
		SysDelay_ms(100);
		return (iic.getError() == ERR_IIC_NONE);
	}

	void SetI2S(I2SFormat fmt, I2SLength len) {
		byte f = (byte)fmt & 0x03;
		byte l = (byte)len & 0x07;
		Send(0x17, (f << 1) | (l << 3)); /* R23: DAC Format */
	}

	void SetI2S(byte fmt, byte len) {
		fmt &= 0x03;
		len &= 0x07;
		Send(0x17, (fmt << 1) | (len << 3));
	}

	void SetHPVol(byte volume) {
		if (volume > 33) volume = 33;
		Send(0x2E, volume);
		Send(0x2F, volume);
	}

	void SetSPKVol(byte volume) {
		if (volume > 33) volume = 33;
		Send(0x30, volume);
		Send(0x31, volume);
	}

	void SetADDA(bool dacen, bool adcen) {
		// 与官方 es8388_adda_cfg 一致: bit0/2 = DAC, bit1/3 = ADC (1=关闭)
		byte tempreg = 0;
		tempreg |= ((!dacen) ? 1u : 0u) << 0;
		tempreg |= ((!adcen) ? 1u : 0u) << 1;
		tempreg |= ((!dacen) ? 1u : 0u) << 2;
		tempreg |= ((!adcen) ? 1u : 0u) << 3;
		Send(0x02, tempreg);
	}

	void SetOutput(bool o1en, bool o2en) {
		byte tempreg = 0;
		if (o1en) tempreg |= (3 << 2); /* LOUT1 / ROUT1 power up (0x0C) */
		if (o2en) tempreg |= (3 << 4); /* LOUT2 / ROUT2 power up (0x30) */
		Send(0x04, tempreg);
	}

	void SetMicGain(byte gain) {
		gain &= 0x0F;
		gain |= (gain << 4);
		Send(0x09, gain);
	}

public: // ---- uni::SubACI ----
	bool isReady() const {
		return true;
	}

	bool setFormat(const uni::AudioFormat& format) {
		format_cache = format;
		return true;
	}

	bool ConfigI2S(uint32 fmt, uint32 bits) {
		I2SLength len;
		switch (bits) {
		case 16: len = I2SLength::Bit16; break;
		case 18: len = I2SLength::Bit18; break;
		case 20: len = I2SLength::Bit20; break;
		case 24: len = I2SLength::Bit24; break;
		case 32: len = I2SLength::Bit32; break;
		default: return false;
		}
		if (fmt > (uint32)I2SFormat::PCM) return false;
		SetI2S((I2SFormat)fmt, len);
		return true;
	}

	stduint getChannelCount() const {
		return 2;
	}

	const char* getChannelName(stduint ch) const {
		switch (ch) {
		case 0: return "Headphone";
		case 1: return "Speaker";
		default: return nullptr;
		}
	}

	bool getMainChannel(stduint& out) const {
		out = 0;
		return true;
	}

	bool setVolume(stduint ch, uint32 left, uint32 right) {
		if (ch > 1) return false;
		uint32 percent = (left + right) / 2;
		if (percent > 100) percent = 100;
		volume_cache[ch] = percent;
		byte vol = (byte)(percent * 33 / 100);
		if (ch == 0) SetHPVol(vol); else SetSPKVol(vol);
		return true;
	}

	bool getVolume(stduint ch, uint32& left, uint32& right) const {
		if (ch > 1) return false;
		left = right = volume_cache[ch];
		return true;
	}

	bool setMute(stduint ch, bool mute = true) {
		if (ch > 1) return false;
		byte vol = mute ? 0 : (byte)(volume_cache[ch] * 33 / 100);
		if (ch == 0) SetHPVol(vol); else SetSPKVol(vol);
		return true;
	}

protected:
	uni::AudioFormat format_cache;
	uint32 volume_cache[2] = { 100, 100 };
};

#endif
