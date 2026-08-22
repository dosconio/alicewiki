#include <cpp/unisym>
#include <cpp/Device/IIC>
#include <cpp/Device/GPIO>
#include <cpp/Device/SysTick>
#include <cpp/Device/_Touch.hpp>

#include "../device/GT9147.h"

using namespace uni;

// GT9147 I2C 命令与寄存器（照 HAL gt9147.h）
#define GT_CMD_WR  0x28
#define GT_CMD_RD  0x29
#define GT_REG_CTRL   0x8040// 控制寄存器（0x02=软复位，0x00=结束复位）
#define GT_REG_PID    0x8140// 产品 ID 寄存器（4 字节 ASCII，如 "9147"）
#define GT_REG_GSTID  0x814E// 触摸状态寄存器（bit7=buffer status，bit3:0=点数）
#define GT_REG_TP1    0x8150// 第 1 个触摸点数据地址（每点 8 字节间隔）

// 写 GT9147 寄存器：START → 0x28 → 地址高 → 地址低 → 数据…
bool GT9147_t::writeReg(uint16 reg, const byte* buf, byte len) {
	iic.SendStart();
	if (!(iic << GT_CMD_WR)) { dbg_ack_stage = 0; iic.SendStop(); return false; }
	if (!(iic << (byte)(reg >> 8))) { dbg_ack_stage = 1; iic.SendStop(); return false; }
	if (!(iic << (byte)(reg & 0xFF))) { dbg_ack_stage = 2; iic.SendStop(); return false; }
	for (byte i = 0; i < len; i++)
		if (!(iic << buf[i])) { dbg_ack_stage = 4; iic.SendStop(); return false; }
	iic.SendStop();
	return true;
}

// 读 GT9147 寄存器：写地址 → 重复起始 → 0x29 → 读数据（最后字节回 NACK）
bool GT9147_t::readReg(uint16 reg, byte* buf, byte len) {
	iic.SendStart();
	if (!(iic << GT_CMD_WR)) { dbg_ack_stage = 0; iic.SendStop(); return false; }
	if (!(iic << (byte)(reg >> 8))) { dbg_ack_stage = 1; iic.SendStop(); return false; }
	if (!(iic << (byte)(reg & 0xFF))) { dbg_ack_stage = 2; iic.SendStop(); return false; }
	iic.SendStart();// 重复起始
	if (!(iic << GT_CMD_RD)) { dbg_ack_stage = 3; iic.SendStop(); return false; }
	for (byte i = 0; i < len; i++)
		buf[i] = iic.ReadByte(true, i < len - 1);// 除最后字节外回 ACK
	iic.SendStop();
	return true;
}

// 初始化：INT 上拉输入 → RST 复位脉冲 → INT 浮空 → 读 PID → 软复位（照 HAL GT9147_Init）
bool GT9147_t::Initialize() {
	intn.setMode(GPIOMode::IN_Pull).setPull(true);
	rst.setMode(GPIOMode::OUT_PushPull);
	rst = false;// 复位
	SysDelay_ms(10);
	rst = true;// 释放复位
	SysDelay_ms(10);
	intn.setMode(GPIOMode::IN_Floating);
	SysDelay_ms(100);
	// 读产品 ID（照 HAL gt9147.c：GT911 / GT9147 / GT1158 / GT9271 均可）
	byte pid[5] = { 0 };
	dbg_ack_stage = 0xFF;
	bool rd_ok = readReg(GT_REG_PID, pid, 4);
	for (byte i = 0; i < 4; i++) dbg_pid[i] = pid[i];
	if (!rd_ok) return false;
	bool is_9147 = pid[0] == '9' && pid[1] == '1' && pid[2] == '4' && pid[3] == '7';
	bool is_1158 = pid[0] == '1' && pid[1] == '1' && pid[2] == '5' && pid[3] == '8';
	bool is_911  = pid[0] == '9' && pid[1] == '1' && pid[2] == '1';
	bool is_9271 = pid[0] == '9' && pid[1] == '2' && pid[2] == '7' && pid[3] == '1';
	if (!is_9147 && !is_1158 && !is_911 && !is_9271)
		return false;
	// 软复位仅 GT9147 需要（照 HAL：其他 GT 芯片直接通过）
	if (is_9147) {
		byte ctrl = 0x02;
		writeReg(GT_REG_CTRL, &ctrl, 1);
		SysDelay_ms(10);
		ctrl = 0x00;
		writeReg(GT_REG_CTRL, &ctrl, 1);
	}
	return true;
}

// 扫描：读 GSTID → 清标志 → 逐点读 4 字节（X低 X高 Y低 Y高）→ 横屏坐标映射（照 HAL GT9147_Scan）
byte GT9147_t::Scan() {
	byte sta = 0;
	readReg(GT_REG_GSTID, &sta, 1);
	byte cnt = sta & 0x0F;
	if ((sta & 0x80) && cnt < 6) {// 有 buffer status 标志，先清除
		byte z = 0;
		writeReg(GT_REG_GSTID, &z, 1);
	}
	touch_cnt = 0;
	if (cnt == 0 || cnt > MAX_TOUCH) return 0;
	byte buf[4];
	for (byte i = 0; i < cnt; i++) {
		readReg((uint16)(GT_REG_TP1 + i * 8), buf, 4);// 每点 8 字节间隔
		x[i] = (uint16)(((uint16)buf[1] << 8) | buf[0]);// 横屏：x = 原始X
		y[i] = (uint16)(((uint16)buf[3] << 8) | buf[2]);// 横屏：y = 原始Y
	}
	touch_cnt = cnt;
	return cnt;
}
