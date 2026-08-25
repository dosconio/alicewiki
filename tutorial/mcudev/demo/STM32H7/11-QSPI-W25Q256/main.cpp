// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 11-QSPI-W25Q256：QSPI 驱动 W25Q256 闪存读写演示（SPI 1 线模式）
 *
 * 预期现象：
 *   1) 上电后 QSPI 初始化，检测 W25Q256（ID 0xEF18），串口 115200 打印：
 *        UNISYM STM32H743 QSPI W25Q256 demo
 *        W25Q256 ready, ID = 0xEF18
 *   2) 擦除扇区 0，写入 "QSPI TEST OK"，读回校验，串口打印：
 *        write = QSPI TEST OK
 *        read  = QSPI TEST OK
 *        verify = PASS
 *      校验失败则打印 verify = FAIL。
 *   3) DS0（LEDB）每 500ms 翻转，指示程序运行。
 *   4) 检测不到 W25Q256 时串口打印 "W25Q256 check failed, ID = 0xXXXX"，
 *      DS1（LEDR）点亮闪烁，程序停在 erro()。
 *
 * 使用说明：
 *   1) 串口 115200；QSPI 引脚 PB6(nCS)/PB2(CLK)/PF8,9(IO0,1)/PF7,6(IO2,3)。
 *   2) 全部使用 SPI 1 线模式（不进入 QPI），24 位地址，测试地址固定为 0。
 *   3) 若 flash 残留 QPI 模式（此前程序进入过且未断电），初始化会自动先发
 *      退出 QPI 命令再重试，无需手动处理。
 *   4) 每次运行先擦除扇区 0 再写入，可重复执行。
 *
 * 迁移调试经验（务必留意，均为踩过的坑）：
 *   1) [驱动] QUADSPI_DR 必须按 8 位访问：H7 的 QSPI FIFO 是 32 位宽、
 *      8 项深。SPI 模式收发的字节打包进同一个 32 位 FIFO 字，若用 32 位
 *      读写 DR，一次就把整个字弹出/压入，导致多字节传输只搬 1 个字节
 *      （现象：读回 0xEF 00 00 ...，只有第 1 字节对）。
 *      修复：用 uni::Reference_T<byte>((QSPI 基址 + DR 偏移)) 逐字节访问
 *      （驱动 Receive/Transmit 与 interrupt_qspi.hpp 的 FIFO 中断路径均已改）。
 *   2) [驱动] 间接读的轮询应等 "FT(FIFO 阈值到达) 或 TC(传输完成)" 任一置位
 *      再读一个字节，绝不能写 FLEVEL/FIFO 计数值的条件式，否则小数据量
 *      （低于 FIFO 阈值）永远等不到而超时。
 *   3) [flash] W25Q256 的 QPI 模式、4 字节地址模式都是易失位，软复位不清除。
 *      本工程只走 SPI 1 线 + 24 位地址，所以初始化必须：
 *        - 先发 0xE9（退出 4 字节地址模式）回到 3 字节地址；
 *        - ID 读失败则发 0xFF（退出 QPI，四线指令）再重试。
 *      否则地址/时序错位，表现为 read 永远为空、ID 反序（0x18EF）。
 *   4) [flash] 擦除扇区约需 400ms~2s，页编程约需 3ms。命令发出后仅轮询
 *      WIP(SR1.bit0) 不足以覆盖（首字节 WIP 尚未置位），需先 SysDelay_ms
 *      再轮询 WIP，否则随后读取 flash 仍忙、返回全 0。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SPI-Quad.hpp>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];
GPIN& LEDR = GPIOB[ 1];

char _buf[64]; String buf(_buf, byteof(_buf));

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// ---- W25Q256 (SPI 1-line mode) opcodes ----
static const byte W25X_WriteEnable      = 0x06;
static const byte W25X_ReadStatusReg1   = 0x05;
static const byte W25X_ManufactDeviceID = 0x90;
static const byte W25X_ReadData         = 0x03;
static const byte W25X_PageProgram      = 0x02;
static const byte W25X_SectorErase      = 0x20;
static const byte W25X_ExitQPIMode      = 0xFF;

static const stduint W25Q256_ID = 0xEF18;

// send one QSPI command (blocking, SPI 1-line); data phase length = nb_data (0 = none)
static void w25qCmd(byte instr, stduint addr, stduint dummy,
	QSPIAddrMode amode, QSPIAddressSize asize, QSPIDataMode dmode, stduint nb_data = 0) {
	QSPI_Command cmd;
	cmd.instruction = instr;
	cmd.address = addr;
	cmd.dummy_cycles = dummy;
	cmd.instruction_mode = QSPIInstrMode::Line1;
	cmd.address_mode = amode;
	cmd.address_size = asize;
	cmd.data_mode = dmode;
	cmd.nb_data = nb_data;
	cmd.alternate_byte_mode = QSPIAlternateMode::None;
	cmd.ddr_mode = QSPIDdrMode::Disable;
	cmd.ddr_hold_half_cycle = QSPIDdrHold::AnalogDelay;
	cmd.sioo_mode = QSPISiooMode::EveryCmd;
	QSPI1.Command(cmd, IOMethod::Loop);
}

static void w25qWriteEnable() {
	w25qCmd(W25X_WriteEnable, 0, 0, QSPIAddrMode::None, QSPIAddressSize::_8, QSPIDataMode::None);
}

static byte w25qReadSR1() {
	byte sr = 0;
	w25qCmd(W25X_ReadStatusReg1, 0, 0, QSPIAddrMode::None, QSPIAddressSize::_8, QSPIDataMode::Line1, 1);
	QSPI1.Receive(&sr, 1, IOMethod::Loop);
	return sr;
}

static void w25qWaitBusy() {
	while (w25qReadSR1() & 0x01) {}
}

static stduint w25qReadID() {
	byte id[2] = {0, 0};
	w25qCmd(W25X_ManufactDeviceID, 0, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::Line1, 2);
	QSPI1.Receive(id, 2, IOMethod::Loop);
	return (id[0] << 8) | id[1];
}

static void w25qRead(byte* pbuf, stduint addr, stduint len) {
	w25qCmd(W25X_ReadData, addr, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::Line1, len);
	QSPI1.Receive(pbuf, len, IOMethod::Loop);
}

static void w25qWritePage(byte* pbuf, stduint addr, stduint len) {
	w25qWriteEnable();
	w25qCmd(W25X_PageProgram, addr, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::Line1, len);
	QSPI1.Transmit(pbuf, len, IOMethod::Loop);
	SysDelay_ms(10);    // page program: typ ~3ms
	w25qWaitBusy();
}

static void w25qEraseSector(stduint addr) {
	w25qWriteEnable();
	w25qCmd(W25X_SectorErase, addr, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::None);
	SysDelay_ms(500);   // sector erase: typ ~400ms, max ~2s
	w25qWaitBusy();
}

// try to bring a flash stuck in QPI mode (volatile, survives soft reset) back to SPI
static void w25qExitQpi() {
	QSPI_Command cmd;
	cmd.instruction = W25X_ExitQPIMode;
	cmd.address = 0;
	cmd.dummy_cycles = 0;
	cmd.instruction_mode = QSPIInstrMode::Line4;   // Exit-QPI must be sent in QPI mode
	cmd.address_mode = QSPIAddrMode::None;
	cmd.address_size = QSPIAddressSize::_8;
	cmd.data_mode = QSPIDataMode::None;
	cmd.alternate_byte_mode = QSPIAlternateMode::None;
	cmd.ddr_mode = QSPIDdrMode::Disable;
	cmd.ddr_hold_half_cycle = QSPIDdrHold::AnalogDelay;
	cmd.sioo_mode = QSPISiooMode::EveryCmd;
	QSPI1.Command(cmd, IOMethod::Loop);
}

// return device ID, or 0 on failure; SPI (1-line) mode only
static stduint w25qInit() {
	// force back to 3-byte address mode (in case a previous test left 4-byte mode)
	w25qCmd(0xE9, 0, 0, QSPIAddrMode::None, QSPIAddressSize::_8, QSPIDataMode::None);
	stduint id = w25qReadID();
	if (id != W25Q256_ID) {
		w25qExitQpi();          // in case the flash is stuck in QPI mode
		id = w25qReadID();      // retry in SPI mode
	}
	return id;
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;

	XART1.setMode(115200);
	SysDelay_ms(500);
	XART1.OutFormat("UNISYM STM32H743 QSPI W25Q256 demo\r\n");

	// QSPI pins: PB6 nCS(AF10), PB2 CLK(AF9), PF8/9 IO0/1(AF10), PF7/6 IO2/3(AF9)
	GPIOB[6].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOB[2].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(9);
	GPIOF[8].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOF[9].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOF[7].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(9);
	GPIOF[6].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(9);

	// QSPI init (W25Q256: 32MB; prescaler=4 -> QSPI <= 48MHz, within Read-Data spec)
	QSPI1.init.clock_prescaler = 4;
	QSPI1.init.fifo_threshold = 4;
	QSPI1.init.sample_shifting = QSPISampleShift::None;
	QSPI1.init.flash_size = 24;
	QSPI1.init.chip_select_high_time = QSPICSHighTime::Cycle5;
	QSPI1.init.clock_mode = QSPIClockMode::Mode0;
	QSPI1.init.flash_select = QSPIFlashSelect::Flash1;
	QSPI1.init.dual_flash = QSPIDualFlash::Disable;
	if (!QSPI1.setMode()) erro("QSPI init failed");

	stduint id = w25qInit();
	if (id != W25Q256_ID) {
		XART1.OutFormat("W25Q256 check failed, ID = 0x%04X\r\n", id);
		erro("W25Q256 not found");
	}
	XART1.OutFormat("W25Q256 ready, ID = 0x%04X\r\n", id);

	// write + read-back test at address 0
	byte text[] = "QSPI TEST OK";
	const stduint LEN = sizeof(text);
	byte readback[sizeof(text)] = {0};

	w25qEraseSector(0);
	w25qWritePage(text, 0, LEN);
	w25qRead(readback, 0, LEN);

	XART1.OutFormat("write = %s\r\n", (const char*)text);
	XART1.OutFormat("read  = %s\r\n", (const char*)readback);
	bool ok = true;
	for (stduint i = 0; i < LEN; i++) if (readback[i] != text[i]) ok = false;
	XART1.OutFormat("verify = %s\r\n", ok ? "PASS" : "FAIL");

	while (1) {
		LEDB.Toggle();
		SysDelay_ms(500);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
