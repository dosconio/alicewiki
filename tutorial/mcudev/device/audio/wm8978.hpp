// ASCII CPP-ISO11 TAB4 CRLF
// Docutitle: (Device/Audio) WM8978 Audio Codec Driver
// Codifiers: @dosconio: 2026xxxx~;
// Attribute: Arn-Covenant Any-Architect Env-Freestanding Non-Dependence
// Copyright: UNISYM, under Apache License 2.0

#ifndef _INC_DEVICE_AUDIO_WM8978_HPP
#define _INC_DEVICE_AUDIO_WM8978_HPP

#ifndef __USYM__
#include <cpp/unisym>
#include <cpp/Device/IIC>
#include <cpp/System/Audiosys.hpp>
#endif

#define WM8978_ADDR 0x1A

class WM8978_t : public uni::SubACI {
public:
	enum class I2SFormat : byte {
		LSB = 0,
		MSB = 1,
		Standard = 2,// Philips I2S standard
		PCM = 3
	};
	enum class I2SLength : byte {
		Bit16 = 0,
		Bit20 = 1,
		Bit24 = 2,
		Bit32 = 3
	};
protected:
	uni::IIC_t& iic;
	byte dev_addr = WM8978_ADDR;
	uint16 reg_map[58];
public:
	WM8978_t(uni::IIC_t& _iic) : iic(_iic) {
		static const uint16 init_tbl[58] = {
			0x0000, 0x0000, 0x0000, 0x0000, 0x0050, 0x0000, 0x0140, 0x0000,
			0x0000, 0x0000, 0x0000, 0x00FF, 0x00FF, 0x0000, 0x0100, 0x00FF,
			0x00FF, 0x0000, 0x012C, 0x002C, 0x002C, 0x002C, 0x002C, 0x0000,
			0x0032, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x0038, 0x000B, 0x0032, 0x0000, 0x0008, 0x000C, 0x0093, 0x00E9,
			0x0000, 0x0000, 0x0000, 0x0000, 0x0003, 0x0010, 0x0010, 0x0100,
			0x0100, 0x0002, 0x0001, 0x0001, 0x0039, 0x0039, 0x0039, 0x0039,
			0x0001, 0x0001
		};
		for (byte i = 0; i < 58; i++) {
			reg_map[i] = init_tbl[i];
		}
	}

	bool WriteReg(byte reg, uint16 val) {
		if (reg >= 58) return false;
		iic.SendStart(dev_addr);
		iic.Send((byte)((reg << 1) | ((val >> 8) & 0x01)), true);
		iic.Send((byte)(val & 0xFF), true);
		iic.SendStop();
		reg_map[reg] = val;
		return (iic.getError() == ERR_IIC_NONE);
	}

	uint16 ReadReg(byte reg) const {
		if (reg >= 58) return 0;
		return reg_map[reg];
	}

	byte GetAddress() const { return dev_addr; }

	bool Initialize() {
		WriteReg(0, 0);// Soft reset
		SysDelay_ms(20);
		// Default general configuration
		if (!WriteReg(1, 0x001B)) return false;        // R1: BIASEN=1, BUFIOEN=1, VMIDSEL=11 (5K)
		if (!WriteReg(2, 0x01B0)) return false;        // R2: ROUT1, LOUT1 enable, BOOSTENR, BOOSTENL enable
		if (!WriteReg(3, 0x006F)) return false;        // R3: LOUT2, ROUT2 enable, RMIX, LMIX enable, DACENR, DACENL enable
		if (!WriteReg(4, 0x0010)) return false;        // R4: 16-bit standard I2S format
		if (!WriteReg(6, 0x0000)) return false;        // R6: MCLK supplied externally
		if (!WriteReg(10, 0x0008)) return false;       // R10: SOFTMUTE=0, DACOSR128=1
		if (!WriteReg(43, 0x0010)) return false;       // R43: INVROUT2 enable (BTL differential speaker output)
		if (!WriteReg(49, 0x0006)) return false;       // R49: TSDEN=1 | SPEAKER BOOST 1.5x (0x06)
		if (!WriteReg(50, 0x0001)) return false;       // R50: Left DAC to Left Mixer (DACL2LMIX=1)
		if (!WriteReg(51, 0x0001)) return false;       // R51: Right DAC to Right Mixer (DACR2RMIX=1)
		SetHPVol(63, 63);                              // Headphone maximum volume (+6dB)
		SetSPKVol(63);                                 // Speaker maximum volume (+6dB)
		return true;
	}

	void SetADDA(bool dacen, bool adcen) {
		uint16 regval = ReadReg(3);
		if (dacen) regval |= (3 << 0) | (3 << 2) | (3 << 5); else regval &= ~((3 << 0) | (3 << 2) | (3 << 5));
		WriteReg(3, regval);

		regval = ReadReg(2);
		if (adcen) regval |= (3 << 0); else regval &= ~(3 << 0);
		regval |= (3 << 7); // keep LOUT1/ROUT1 enabled
		WriteReg(2, regval);
	}

	void SetInput(bool micen, bool lineinen, bool auxen) {
		uint16 regval = ReadReg(2);
		if (micen) regval |= (3 << 2); else regval &= ~(3 << 2);
		WriteReg(2, regval);

		regval = ReadReg(44);
		if (micen) regval |= (3 << 4) | (3 << 0); else regval &= ~((3 << 4) | (3 << 0));
		WriteReg(44, regval);

		if (lineinen) SetLineInGain(5); else SetLineInGain(0);
		if (auxen) SetAuxGain(7); else SetAuxGain(0);
	}

	void SetOutput(bool dacen, bool bpsen) {
		uint16 regval = 0;
		if (dacen) regval |= (1 << 0);
		if (bpsen) regval |= (1 << 1) | (5 << 2);
		WriteReg(50, regval);
		WriteReg(51, regval);
	}

	void SetMICGain(byte gain) {
		gain &= 0x3F;
		WriteReg(45, gain);
		WriteReg(46, gain | (1 << 8));
	}

	void SetLineInGain(byte gain) {
		gain &= 0x07;
		uint16 regval = ReadReg(47) & ~(7 << 4);
		WriteReg(47, regval | (gain << 4));
		regval = ReadReg(48) & ~(7 << 4);
		WriteReg(48, regval | (gain << 4));
	}

	void SetAuxGain(byte gain) {
		gain &= 0x07;
		uint16 regval = ReadReg(47) & ~(7 << 0);
		WriteReg(47, regval | (gain << 0));
		regval = ReadReg(48) & ~(7 << 0);
		WriteReg(48, regval | (gain << 0));
	}

	void SetI2S(I2SFormat fmt, I2SLength len) {
		byte f = (byte)fmt & 0x03;
		byte l = (byte)len & 0x03;
		WriteReg(4, (f << 3) | (l << 5));
	}

	void SetI2S(byte fmt, byte len) {
		fmt &= 0x03;
		len &= 0x03;
		WriteReg(4, (fmt << 3) | (len << 5));
	}

	void SetHPVol(byte voll, byte volr) {
		voll &= 0x3F;
		volr &= 0x3F;
		if (voll == 0) voll |= (1 << 6);// Mute when 0
		if (volr == 0) volr |= (1 << 6);// Mute when 0
		WriteReg(52, voll | (1 << 8));
		WriteReg(53, volr | (1 << 8));  // Update volume synchronously (HPVU=1)
	}

	void SetSPKVol(byte volx) {
		volx &= 0x3F;
		if (volx == 0) volx |= (1 << 6);// Mute when 0
		WriteReg(54, volx | (1 << 8));
		WriteReg(55, volx | (1 << 8));  // Update volume synchronously (SPKVU=1)
	}

	void Set3D(byte depth) {
		depth &= 0x0F;
		WriteReg(41, depth);
	}

	void SetEQ3DDir(byte dir) {
		uint16 regval = ReadReg(18) & ~(1 << 8);
		regval |= ((dir & 1) << 8);
		WriteReg(18, regval);
	}

	void SetEQ1(byte cfreq, byte gain) {
		cfreq &= 0x03;
		gain &= 0x1F;
		uint16 regval = ((cfreq & 0x03) << 5) | (gain & 0x1F) | (1 << 8);
		WriteReg(18, regval);
	}

	void SetEQ2(byte cfreq, byte gain) {
		cfreq &= 0x03;
		gain &= 0x1F;
		uint16 regval = ((cfreq & 0x03) << 5) | (gain & 0x1F) | (1 << 8);
		WriteReg(19, regval);
	}

	void SetEQ3(byte cfreq, byte gain) {
		cfreq &= 0x03;
		gain &= 0x1F;
		uint16 regval = ((cfreq & 0x03) << 5) | (gain & 0x1F) | (1 << 8);
		WriteReg(20, regval);
	}

	void SetEQ4(byte cfreq, byte gain) {
		cfreq &= 0x03;
		gain &= 0x1F;
		uint16 regval = ((cfreq & 0x03) << 5) | (gain & 0x1F) | (1 << 8);
		WriteReg(21, regval);
	}

	void SetEQ5(byte cfreq, byte gain) {
		cfreq &= 0x03;
		gain &= 0x1F;
		uint16 regval = ((cfreq & 0x03) << 5) | (gain & 0x1F) | (1 << 8);
		WriteReg(22, regval);
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
		case 20: len = I2SLength::Bit20; break;
		case 24: len = I2SLength::Bit24; break;
		case 32: len = I2SLength::Bit32; break;
		default: return false;
		}
		if (fmt > 3) return false;
		// 0 表示本设备的标准 I2S(Philips): WM8978 的 Standard = 2
		SetI2S(fmt == 0 ? I2SFormat::Standard : (I2SFormat)fmt, len);
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
		byte vol = (byte)(percent * 63 / 100);
		if (ch == 0) SetHPVol(vol, vol); else SetSPKVol(vol);
		return true;
	}

	bool getVolume(stduint ch, uint32& left, uint32& right) const {
		if (ch > 1) return false;
		left = right = volume_cache[ch];
		return true;
	}

	bool setMute(stduint ch, bool mute = true) {
		if (ch > 1) return false;
		byte vol = mute ? 0 : (byte)(volume_cache[ch] * 63 / 100);
		if (ch == 0) SetHPVol(vol, vol); else SetSPKVol(vol);
		return true;
	}

protected:
	uni::AudioFormat format_cache;
	uint32 volume_cache[2] = { 100, 100 };
};

#endif
