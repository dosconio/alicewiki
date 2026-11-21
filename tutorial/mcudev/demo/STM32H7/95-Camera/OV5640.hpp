// ASCII CPP-ISO11 TAB4 LF
// Docutitle: [Device] OV5640 Image Sensor
// Codifiers: @ArinaMgk
// Attribute: Arn-Covenant Any-Architect Env-Freestanding Non-Dependence
// Copyright: UNISYM, under Apache License 2.0
/*
	Copyright 2023 ArinaMgk

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0
	http://unisym.org/license.html

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.
*/

#ifndef _INC_DEMO_OV5640
#define _INC_DEMO_OV5640

#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/IIC>

class OV5640_t {
public:
	OV5640_t();
	bool Initialize();
	bool WriteRegister(uint16 address, byte value);
	bool ReadRegister(uint16 address, byte& value);
	uint16 getIdentity() const { return identity; }
	bool enOutput(bool enable);
	bool enPower(bool enable);
	bool enBuzzer(bool enable);
	bool enFlash(bool enable);
	bool setRGB565();
	bool setJPEG();
	bool setOutput(const uni::Rectangle& area);
	bool setWindow(const uni::Rectangle& area);
	bool setLightMode(byte mode);
	bool setSaturation(byte level);
	bool setBrightness(byte level);
	bool setContrast(byte level);
	bool setSharpness(byte level);
	bool setEffect(byte effect);
	bool setTestPattern(byte mode);
	bool InitializeFocus();
	bool FocusSingle();
	bool FocusContinuous();

private:
	uni::IIC_SOFT control_bus, expander_bus;
	byte expander_latch = 0xFF;
	uint16 identity = 0;
	static void DelayBus();
	static bool SendByte(uni::IIC_SOFT& bus, byte value);
	bool UpdateExpander(byte mask, bool high);
	bool WriteRegisters(const uint16 (*registers)[2], stduint count);
	bool WriteGroup(const uint16 (*registers)[2], stduint count);
	bool WaitRegister(uint16 address, byte value, stduint timeout_ms);
};

#endif
