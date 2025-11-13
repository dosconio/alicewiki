
class AD9954 {
protected:
	void Send(byte cmd, byte* databyte, byte lengh)
	{
		byte SPDR;
		for0(i, _BYTE_BITS_) {
			SCLK = 0;
			SDIO = (cmd & 0x80);
			SCLK.Toggle();
			cmd <<= 1;
		}
		for0r(j, lengh) {
			SPDR = databyte[j];	    //取出高 byte
			for0(i, 8)
			{
				SCLK = 0;
				SDIO = SPDR & 0x80;
				SCLK.Toggle();
				SPDR <<= 1;
			}
		}
	}

public:
	GPIO_Pin& PWRDWNCTL, & RST, & IOSYNC, & SDO, & CS, & SCLK, & SDIO, & OSK, & UPDATA;
	byte words0[4];// 第0组：功能控制字
	byte words1[3];// 第1组：模拟部分控制字  采用20M有源输入，20倍频，VCO范围设置为250M到400MHz，锁相环泵电流为150UA
	byte ASF[2];// 第2组
	byte ARR[1];// 第3组
	byte FREQ[4];// 第4组：频率控制字
	byte PHASE[2];// 第5组：相位控制字 相位偏移字
	AD9954(GPIO_Pin& PWRDWNCTL, GPIO_Pin& RST, GPIO_Pin& IOSYNC, GPIO_Pin& SDO, GPIO_Pin& CS, GPIO_Pin& SCLK, GPIO_Pin& SDIO, GPIO_Pin& OSK, GPIO_Pin& UPDATA) :
		PWRDWNCTL(PWRDWNCTL), RST(RST), IOSYNC(IOSYNC), SDO(SDO), CS(CS), SCLK(SCLK), SDIO(SDIO), OSK(OSK), UPDATA(UPDATA)
	{
		PWRDWNCTL = 0;
		RST = 0;
		IOSYNC = 0;
		SDO = 0;
		CS = 0;
		SCLK = 0;
		SDIO = 0;
		OSK = 0;
		UPDATA = 0;
		//
		words0[0] = 0x00; words0[1] = 0x10; words0[2] = 0x00; words0[3] = 0x00;
		words1[0] = 0xA7; words1[1] = 0x00; words1[2] = 0x00;
		ASF[0] = 0x00; ASF[1] = 0x00;
		ARR[0] = 0x00;
		FREQ[0] = 0x0A; FREQ[1] = 0xD7; FREQ[2] = 0xA3; FREQ[3] = 0x00;
		PHASE[0] = 0x00; PHASE[1] = 0x00;
	}

	void setMode(void) {
		GPIO_Pin* sbits[] = {
			&PWRDWNCTL,&RST,&IOSYNC,&SDO,&CS,&SCLK,&SDIO,&OSK,&UPDATA
		};
		for0(i, numsof(sbits))
			sbits[i]->setMode(GPIOMode::OUT_PushPull);
		//初始化AD9952使输出为1MHZ
		PWRDWNCTL = 0;
		CS = 0;
		UPDATA = 0;
		IOSYNC = 0;
		RST = 1;
		for0(_i, 50000);
		RST = 0;
		Send(0X00, words0, 4);
		Send(0X01, words1, 3);
		Send(0X02, ASF, 2);
		Send(0X03, ARR, 1);
		Send(0X04, FREQ, 4);
		Send(0X05, PHASE, 2);
		UPDATA = 1;
		UPDATA.Toggle();
	}

	void setAmplitude(stduint Ampli)
	{
		unsigned char buf[] = { 0x40,0x20,0x00,0x02 }; // {0x02,0x00,0x20,0x40} マ
		// 2000 for 75, 4000 for 145mV 40000 for 250mV 0xFFFF for 570mV
		CS = 0;
		Send(0x00, buf, 4);
		ASF[0] = (byte)Ampli;
		ASF[1] = (byte)(Ampli >> 8) & 0x3f;
		Send(0x02, ASF, 2);// (0x03,ControlWord+7,1)マ
		CS.Toggle();
		UPDATA = 1;
		UPDATA.Toggle();
	}

	void setFrequency(uint32 Freq)
	{
		uint32 Temp;
		Temp = (uint32)Freq * 10.73741824;	   //将输入频率因子分为四个字节  10.73741824=(2^32)/400000000
		FREQ[0] = (byte)Temp;
		FREQ[1] = (byte)(Temp >> 8);
		FREQ[2] = (byte)(Temp >> 16);
		FREQ[3] = (byte)(Temp >> 24);
		Send(0X04, FREQ, 4);
		Send(0X05, PHASE, 2);
		UPDATA = 1;
		UPDATA.Toggle();
	}

	// AD相位是初始相位，基本都是 180-deg diff
	void setPhase(stduint ph)
	{
		CS = 0;
		PHASE[0] = (byte)ph;
		PHASE[1] = (byte)(ph >> 8) & 0x3f;
		Send(0x05, PHASE, 2);
		CS.Toggle();
		UPDATA = 1;
		UPDATA.Toggle();
	}

};

#include "driver/Generator/AD9959.h"
