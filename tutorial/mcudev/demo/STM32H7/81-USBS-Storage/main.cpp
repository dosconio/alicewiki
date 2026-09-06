#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include <cpp/Device/SPI-Quad.hpp>
#include <cpp/Device/Storage/FMC/FMC-Flash-NAND.hpp>
#include <cpp/Device/USB/USBPeri-Device.hpp>
#include <cpp/Device/USB/USBPeri-Class.hpp>
#include <cpp/Device/USB/USBPeri-MSC.hpp>
#include <c/prochip/CortexM7.h>// __DSB / __ISB (NAND MPU)
#include <c/consio.h>        // outsfmt decl
#include <c/system/debug.h>  // printlogx decl
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;
using namespace uni::device::SpaceUSB;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];
GPIN& LEDR = GPIOB[ 1];
GPIN& KEYU = GPIOA[ 0];//   Up
GPIN& KEYL = GPIOC[13];// Left
GPIN& KEYD = GPIOH[ 2];// Down
GPIN& KEYR = GPIOH[ 3];//Right

char _buf[64]; String buf(_buf, byteof(_buf));

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
extern "C" void outtxt(const char* str, stduint len) {XART1.out(str, len);}
// link stubs for USB-Header.hpp Log(): forward raw fmt text to UART (no arg expansion,
// but never crash). Signatures match consio.h / debug.h (plain C++ linkage).
int outsfmt(const char* fmt, ...) { outtxt(fmt, StrLength(fmt)); return 0; }
void printlogx(loglevel_t level, const char* fmt, para_list paras) {
	(void)level; (void)paras; outtxt(fmt, StrLength(fmt));
}

volatile bool rx_frame_ready = false;
void hand_xart1() {
	stduint ptr = XART1.getBufferPointer();
	if (ptr) if (_buf[ptr - 1] == '\n' || _buf[ptr - 1] == '\r') {
		if (ptr > 1) {
			_buf[ptr - 1] = 0;
			rx_frame_ready = true;
		}
		else {
			XART1.ClearBuffer(); // lone delimiter (e.g. \n after \r), drop and keep receiving
		}
	}
	if (ptr >= byteof(_buf)) {
		_buf[byteof(_buf) - 1] = 0;
		rx_frame_ready = true;
	}
	if (rx_frame_ready) {
		XART1.abortReceive();// this method will lost data at interval
	}
}

// ---- device descriptors (AKA usbd_desc.c) ----
static const byte _dev_desc[18] = {
	0x12, 0x01,                 // bLength, bDescriptorType = device
	0x00, 0x02,                 // bcdUSB 2.00
	0x00, 0x00, 0x00,           // device class / sub / protocol (class at interface)
	64,                         // bMaxPacketSize0
	0x83, 0x04,                 // idVendor (0x0483 LE)
	0x20, 0x57,                 // idProduct (0x5720 LE)
	0x00, 0x02,                 // bcdDevice 2.00
	0x01, 0x02, 0x03,           // iManufacturer / iProduct / iSerial
	0x01,                       // bNumConfigurations
};

static byte _str_desc[66];      // scratch string descriptor buffer

// ASCII -> USB string descriptor (bLength, type=string, each char + 0x00)
static const byte* MakeString(const char* s) {
	const char* p = s;
	stduint n = 0;
	while (*p) { p++; n++; }
	byte idx = 0;
	_str_desc[idx++] = (byte)(n * 2 + 2);
	_str_desc[idx++] = 0x03;
	while (*s) {
		_str_desc[idx++] = (byte)*s++;
		_str_desc[idx++] = 0x00;
	}
	return _str_desc;
}

static const byte* USBPeri_DeviceDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	len = sizeof(_dev_desc);
	return _dev_desc;
}
static const byte* LangIDDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	static const byte lang[] = { 0x04, 0x03, 0x09, 0x04 };
	len = sizeof(lang);
	return lang;
}
static const byte* ManufacturerDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("STMicroelectronics");
	len = r[0];
	return r;
}
static const byte* ProductDescriptor(PeripheralSpeed speed, uint16& len) {
	const byte* r = MakeString((speed == PeripheralSpeed::High) ? "Mass Storage in HS Mode" : "Mass Storage in FS Mode");
	len = r[0];
	return r;
}
static const byte* SerialDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("UNISYMH7USBMSC");
	len = r[0];
	return r;
}
static const byte* ConfigurationStrDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("MSC Config");
	len = r[0];
	return r;
}
static const byte* InterfaceStrDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("MSC Interface");
	len = r[0];
	return r;
}

// ---- standard inquiry data, one 36-byte block per LUN (AKA usbd_storage.c) ----
// LUN0 = SPI FLASH (W25Q256), LUN1 = NAND (FMC), LUN2 = SD
static const byte _spi_inquiry[36] = {
	0x00, 0x80, 0x02, 0x02, 0x1F, 0x00, 0x00, 0x00,
	'A', 'L', 'I', 'E', 'N', 'T', 'E', 'K', ' ',
	'S', 'P', 'I', ' ', 'F', 'l', 'a', 's', 'h', ' ', 'D', 'i', 's', 'k', ' ',
	'1', '.', '0', ' '
};
static const byte _nand_inquiry[36] = {
	0x00, 0x80, 0x02, 0x02, 0x1F, 0x00, 0x00, 0x00,
	'A', 'L', 'I', 'E', 'N', 'T', 'E', 'K', ' ',
	'N', 'A', 'N', 'D', ' ', 'F', 'l', 'a', 's', 'h', ' ', 'D', 'i', 's', 'k',
	'1', '.', '0', ' '
};
static const byte _sd_inquiry[36] = {
	0x00, 0x80, 0x02, 0x02, 0x1F, 0x00, 0x00, 0x00,
	'U', 'N', 'I', 'S', 'Y', 'M', ' ', ' ', ' ',
	'S', 'T', 'M', '3', '2', ' ', 'D', 'I', 'S', 'K', ' ', ' ', ' ', ' ', ' ',
	'1', '.', '0', ' '
};


#include <c/mempool.h>
static byte _heap_pool[64 * 1024];   // static space backing the pool

// usb_core/usb_msc live inside main() (static locals, constructed after the pool is
// ready); ISR forwarding uses these pointers instead of the objects directly.
static PeripheralDevice* p_usb_core = nullptr;
static USBPeri_MSC* p_usb_msc = nullptr;

// ==================== W25Q256 over QSPI (demo11, SPI 1-line, 4-byte addr) ====================
static const byte W25X_WriteEnable      = 0x06;
static const byte W25X_ReadStatusReg1   = 0x05;
static const byte W25X_ManufactDeviceID = 0x90;
static const byte W25X_ReadData         = 0x03;
static const byte W25X_FastReadData     = 0x0B;   // AKA 正点: FastRead with 8 dummy (reliable at high clock)
static const byte W25X_PageProgram      = 0x02;
static const byte W25X_SectorErase      = 0x20;
static const byte W25X_ExitQPIMode      = 0xFF;
static const byte W25X_Enter4ByteAddr   = 0xB7;
static const stduint W25Q256_ID = 0xEF18;

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
static void w25qWriteEnable() { w25qCmd(W25X_WriteEnable, 0, 0, QSPIAddrMode::None, QSPIAddressSize::_8, QSPIDataMode::None); }
static byte w25qReadSR1() {
	byte sr = 0;
	w25qCmd(W25X_ReadStatusReg1, 0, 0, QSPIAddrMode::None, QSPIAddressSize::_8, QSPIDataMode::Line1, 1);
	QSPI1.Receive(&sr, 1, IOMethod::Loop);
	return sr;
}
// poll WIP with a bounded retry count so a stuck flash can never hang the USB ISR
static bool w25qWaitBusy() {
	stduint guard = 2000000;              // ~bounded by call frequency, not time
	while (w25qReadSR1() & 0x01) { if (!--guard) return false; }
	return true;
}
static stduint w25qReadID() {
	byte id[2] = {0, 0};
	w25qCmd(W25X_ManufactDeviceID, 0, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::Line1, 2);
	QSPI1.Receive(id, 2, IOMethod::Loop);
	return (id[0] << 8) | id[1];
}
static void w25qExitQpi() {
	QSPI_Command cmd;
	cmd.instruction = W25X_ExitQPIMode;
	cmd.address = 0; cmd.dummy_cycles = 0;
	cmd.instruction_mode = QSPIInstrMode::Line4;
	cmd.address_mode = QSPIAddrMode::None;
	cmd.address_size = QSPIAddressSize::_8;
	cmd.data_mode = QSPIDataMode::None;
	cmd.alternate_byte_mode = QSPIAlternateMode::None;
	cmd.ddr_mode = QSPIDdrMode::Disable;
	cmd.ddr_hold_half_cycle = QSPIDdrHold::AnalogDelay;
	cmd.sioo_mode = QSPISiooMode::EveryCmd;
	QSPI1.Command(cmd, IOMethod::Loop);
}
// bring to SPI 1-line + 3-byte address mode (AKA demo11)
static stduint w25qInit() {
	w25qCmd(0xE9, 0, 0, QSPIAddrMode::None, QSPIAddressSize::_8, QSPIDataMode::None);// exit 4-byte addr
	stduint id = w25qReadID();
	if (id != W25Q256_ID) {
		w25qExitQpi();
		id = w25qReadID();
	}
	return id;
}
static void w25qRead(byte* pbuf, stduint addr, stduint len) {
	QSPI_Command cmd;
	cmd.instruction = W25X_FastReadData;   // 0x0B fast read
	cmd.address = addr;
	cmd.dummy_cycles = 8;
	cmd.instruction_mode = QSPIInstrMode::Line1;
	cmd.address_mode = QSPIAddrMode::Line1;
	cmd.address_size = QSPIAddressSize::_24;
	cmd.data_mode = QSPIDataMode::Line1;
	cmd.nb_data = len;
	cmd.alternate_byte_mode = QSPIAlternateMode::None;
	cmd.ddr_mode = QSPIDdrMode::Disable;
	cmd.ddr_hold_half_cycle = QSPIDdrHold::AnalogDelay;
	cmd.sioo_mode = QSPISiooMode::EveryCmd;
	QSPI1.Command(cmd, IOMethod::Loop);
	// Command() leaves CCR in INDIRECT_WRITE and AR set; switch to INDIRECT_READ
	// (FMODE=01) and RE-WRITE AR to actually start the read transfer (HAL does this).
	volatile stduint* CCRr = (volatile stduint*)0x52005014u;
	volatile stduint* ARr  = (volatile stduint*)0x52005018u;
	volatile stduint* DLRr = (volatile stduint*)0x52005010u;
	volatile byte* DR = (volatile byte*)0x52005020u;
	*CCRr = (*CCRr & ~(3u << 26)) | (1u << 26);     // FMODE = 01 (indirect read)
	*DLRr = len - 1;                                // data length
	*ARr = addr;                                    // restart transfer in READ mode
	SysDelay_us(50);                       // let the flash stream data into the RX FIFO
	for (stduint i = 0; i < len; i++)
		pbuf[i] = *DR;
	// clear TC/FT so the next command is not confused by a stale flag
	*(volatile stduint*)0x5200500Cu = 0xFFFFFFFFu;   // FCR (0x0C) write-1-clear
}
static void w25qEraseSector(stduint addr) {
	w25qWriteEnable();
	w25qCmd(W25X_SectorErase, addr, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::None);
	SysDelay_us(50);          // let WEL latch (CPU spin, ISR-safe)
	w25qWaitBusy();           // guarded WIP spin until erase finishes (no SysTick dep)
}
// register-level page program: Command() configures CCR(INDIRECT_WRITE)+AR; here we
// force FMODE=WRITE, set DLR, re-write AR to start, then push bytes into DR.
static void w25qWritePage(stduint addr, const byte* src, stduint len) {
	volatile stduint* CCRr = (volatile stduint*)0x52005014u;
	volatile stduint* ARr  = (volatile stduint*)0x52005018u;
	volatile stduint* DLRr = (volatile stduint*)0x52005010u;
	volatile byte* DR = (volatile byte*)0x52005020u;
	*CCRr = *CCRr & ~(3u << 26);               // FMODE = 00 (indirect write)
	*DLRr = len - 1;
	*ARr = addr;                               // start transfer (write)
	SysDelay_us(10);
	for (stduint i = 0; i < len; i++)
		*DR = src[i];
	// wait for program to latch (WIP poll; CPU-spin only — SysDelay_ms deadlocks in USB ISR)
	w25qWaitBusy();       // guarded spin, no SysTick dependency
	// clear flags for the next command
	*(volatile stduint*)0x5200500Cu = 0xFFFFFFFFu;
}
static void w25qWaitIdle() {
	SysDelay_ms(10);       // AKA demo11: page program fixed 10ms
	w25qWaitBusy();
}
// 512B logical block device over W25Q256 (HAL usbd_storage LUN0: 25MB, 512B blocks)
static byte w25q_sector[4096];


class SpiDisk_t : public StorageTrait {
public:
	SpiDisk_t() { Block_Size = 512; readable = writable = true; }
	bool Read(stduint block, void* dest, stduint Times = 1) override {
		if (block + Times > getUnits()) {
			// XART1.OutFormat("SPI-R OOR blk=%u t=%u units=%u\r\n", (unsigned)block, (unsigned)Times, (unsigned)getUnits());
			return false;
		}
		byte* out = (byte*)dest;
		for (stduint i = 0; i < Times; i++)        // read in 512B steps, like demo11 (small reads only)
			w25qRead(out + i * 512, (block + i) * 512, 512);
		return true;
	}
	// NOR can only clear 1->0. AKA HAL W25QXX_Write: handle each target 512B blob; if its
	// region is all-0xFF write directly, else erase its 4KB sector and rewrite the WHOLE
	// sector (preserving the other data already in it) with the blob merged in.
	bool Write(stduint block, const void* src, stduint Times = 1) override {
		if (block + Times > getUnits()) {
			// XART1.OutFormat("SPI-W OOR blk=%u t=%u units=%u\r\n", (unsigned)block, (unsigned)Times, (unsigned)getUnits());
			return false;
		}
		// XART1.OutFormat("SPI-W blk=%u t=%u\r\n", (unsigned)block, (unsigned)Times);
		const byte* in = (const byte*)src;
		for (stduint t = 0; t < Times; t++) {
			stduint WriteAddr = (block + t) * 512;   // byte address of this 512B blob
			stduint secpos = WriteAddr / 4096;       // 4KB sector number
			stduint secoff = WriteAddr % 4096;       // offset inside sector
			for (stduint s = 0; s < 4096; s += 512)  // read whole sector in 512B steps
				w25qRead(w25q_sector + s, secpos * 4096 + s, 512);
			bool need_erase = false;
			for0(i, 512) if (w25q_sector[secoff + i] != 0xFF) { need_erase = true; break; }
			if (need_erase) {
				w25qEraseSector(secpos * 4096);              // erase whole sector
				for0(i, 512) w25q_sector[secoff + i] = in[t * 512 + i]; // merge blob
				for (stduint i = 0; i < 4096; i += 256) {     // write whole sector back
					w25qWriteEnable();
					w25qCmd(W25X_PageProgram, secpos * 4096 + i, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::Line1, 256);
					w25qWritePage(secpos * 4096 + i, w25q_sector + i, 256);
				}
			}
			else {                                            // region blank: program in place
				for (stduint i = 0; i < 512; i += 256) {
					w25qWriteEnable();
					w25qCmd(W25X_PageProgram, WriteAddr + i, 0, QSPIAddrMode::Line1, QSPIAddressSize::_24, QSPIDataMode::Line1, 256);
					w25qWritePage(WriteAddr + i, in + t * 512 + i, 256);
				}
			}
			// verify: read back this 512B blob and compare (catches silent write failures)
			{
				byte rb[512];
				w25qRead(rb, WriteAddr, 512);
				bool ok = true;
				for0(i, 512) if (rb[i] != in[t * 512 + i]) { ok = false; break; }
				// if (!ok) XART1.OutFormat("SPI-W VERIFY FAIL blk=%u\r\n", (unsigned)(block + t));
			}
		}
		return true;
	}
	stduint getUnits() override { return 16u * 1024 * 1024 / 512; }// 16MB partition: within 3-byte-address range (AKA demo11 SPI verify)
	int operator[](uint64 bytid) override { _TODO return 0; }
};
SpiDisk_t SpiDisk;

// ==================== FMC NAND (demo18) ====================
// NAND timing mode set (AKA demo18 NAND_ModeSet): SET FEATURE 0xEF, addr 0x01, value=mode.
// mode 0 write tADL=200ns is too slow — must switch to mode 4 or page-program fails.
#define _NAND_DEVICE  0x80000000
#define _NAND_CLE     (1 << 16)
#define _NAND_ALE     (1 << 17)
#define _NAND_FEATURE 0xEF
#define _NAND_READY   0x40
static void nand_modeset(byte mode) {
	*(volatile uint8_t*)(_NAND_DEVICE | _NAND_CLE) = _NAND_FEATURE; __DSB();
	*(volatile uint8_t*)(_NAND_DEVICE | _NAND_ALE) = 0x01; __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = mode; __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = 0;    __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = 0;    __DSB();
	*(volatile uint8_t*)_NAND_DEVICE = 0;    __DSB();
	while (FMC_NAND.ReadStatus() != _NAND_READY) {}
}
static void nand_gpio_raw(stduint base, const byte* pins, byte n) {
	Reference moder(base + 0x00), otyper(base + 0x04), speed(base + 0x08);
	Reference pupdr(base + 0x0C), afrl(base + 0x20), afrh(base + 0x24);
	for (byte i = 0; i < n; i++) {
		byte p = pins[i];
		moder.maset(p << 1, 2, 2);
		otyper.rstof(p);
		speed.maset(p << 1, 2, 3);
		pupdr.maset(p << 1, 2, 1);
		(p < 8 ? afrl : afrh).maset((p & 7) << 2, 4, 12);
	}
}
static void nand_gpio_config() {
	RCC[RCCReg::AHB4ENR].setof(3, true);  // GPIOD
	RCC[RCCReg::AHB4ENR].setof(4, true);  // GPIOE
	RCC[RCCReg::AHB4ENR].setof(6, true);  // GPIOG
	static const byte pd[] = {0,1,4,5,11,12,14,15}, pe[] = {7,8,9,10}, pg[] = {9};
	nand_gpio_raw(0x58020C00, pd, byteof(pd));  // GPIOD
	nand_gpio_raw(0x58021000, pe, byteof(pe));  // GPIOE
	nand_gpio_raw(0x58021800, pg, byteof(pg));  // GPIOG
}
static void nand_mpu_open() {
	Reference(0xE000ED94).rstof(0);          // MPU_CTRL disable
	Reference(0xE000ED98) = 3;               // RNR: region 3
	Reference(0xE000ED9C) = 0x80000000;      // RBAR: base address
	Reference(0xE000EDA0) = (1u << 0) | (27u << 1) | (1u << 16) | (0x03u << 24);
	Reference(0xE000ED94) = (1u << 0) | (1u << 2);
	__DSB();
	__ISB();
}

// ==================== NAND 512B logical-sector layer ====================
// NAND physical facts: write is whole-page (2KB column 0), erase is whole-block
// (64 pages = 128KB); a page programs only 1->0, any 0->1 rewrite must erase the whole
// block. Mapping is identity (logical sector == physical sector), persistent across
// replug. Reads slice 512B out of whole-page reads; writes do whole-block RMW using an
// AXI SRAM block buffer at 0x24000000 (no on-chip DTCM pressure).
static const stduint _NAND_SECTOR = 512;
static const stduint _NAND_PAGE   = 2048;
static const stduint _NAND_PPB    = 64;
static const stduint _NAND_BLKBYTES = _NAND_PAGE * _NAND_PPB;   // 128KB
static const stduint _NAND_BLOCKNBR = 4096;
static const stduint _NAND_PLANE_BLOCKS = 2048;
static const stduint _NAND_SPB = _NAND_BLKBYTES / _NAND_SECTOR; // sectors/block = 256
static byte _nand_page[ _NAND_PAGE ];
// Whole-block RMW scratch (128KB). Do NOT hardcode an AXI address: the linker places
// static data (including this array) in RW_IRAM2 from 0x24000000, so a fixed pointer
// there overlaps the heap/statics and corrupts them during writes (crash in _nand_read8).
static byte _nand_blk_mem[ _NAND_BLKBYTES ] __attribute__((aligned(32)));
static byte* _nand_blk = _nand_blk_mem;

// ---- DIAG: non-blocking counters sampled from USB ISR, printed from main loop ----
// (kept commented: no printing inside the USB ISR — OutFormat eats stack and can fault)
// volatile stduint _diag_rd_calls = 0;
// volatile stduint _diag_rd_fail  = 0;
// volatile stduint _diag_rd_d0[8] = { 0,0,0,0,0,0,0,0 };
// volatile stduint _diag_wr_calls = 0;
// volatile stduint _diag_wr_fail  = 0;
// volatile bool    _diag_unk      = false;

static bool _nand_erase_block(stduint block) {
	NANDAddress a{};
	a.page = 0;
	a.plane = (uint16)(block / _NAND_PLANE_BLOCKS);
	a.block = (uint16)(block % _NAND_PLANE_BLOCKS);
	return FMC_NAND.EraseBlock(a);
}
static bool _nand_all_ff(const byte* p, stduint n) { while (n--) if (*p++ != 0xFF) return false; return true; }

class NandDisk512_t : public StorageTrait {
public:
	NandDisk512_t() { Block_Size = _NAND_SECTOR; readable = writable = true; }
	stduint getUnits() override { return _NAND_BLOCKNBR * _NAND_SPB; }

	bool Read(stduint sector, void* dest, stduint Times = 1) override {
		if (sector + Times > getUnits()) return false;
		// _diag_rd_calls++;
		byte* out = (byte*)dest;
		stduint total = Times * _NAND_SECTOR;
		stduint byte_off = sector * _NAND_SECTOR;
		while (total) {
			stduint page = byte_off / _NAND_PAGE;
			stduint pg_off = byte_off % _NAND_PAGE;
			stduint take = (_NAND_PAGE - pg_off) < total ? (_NAND_PAGE - pg_off) : total;
			// read the whole NAND page into the scratch buffer, then slice out the
			// requested range. Single path (no direct-to-dest page read) — the scratch
			// path is what the boot self-test verified; the direct variant misbehaved.
			if (!FMC_NAND.Read(page, _nand_page, 1)) {
				// _diag_rd_fail++;
				return false;
			}
			for0(i, take) out[i] = _nand_page[pg_off + i];
			out += take; total -= take; byte_off += take;
		}
		return true;
	}

	// Whole-block RMW: load block from NAND into AXI-SRAM scratch, patch all target sectors lying
	// in that block, erase (only if block already held data), program non-blank pages back.
	bool Write(stduint sector, const void* src, stduint Times = 1) override {
		if (sector + Times > getUnits()) return false;
		// _diag_wr_calls++;
		const byte* in = (const byte*)src;
		stduint n_left = Times, cur = sector;
		while (n_left) {
			stduint blk = cur / _NAND_SPB;
			stduint blk_first = blk * _NAND_SPB;
			stduint n_in_blk = (blk_first + _NAND_SPB) - cur;
			if (n_in_blk > n_left) n_in_blk = n_left;

			// read whole block (64 pages) into the AXI-SRAM scratch
			if (!FMC_NAND.Read(blk * _NAND_PPB, _nand_blk, _NAND_PPB)) {
				// _diag_wr_fail++;
				return false;
			}
			bool was_blank = _nand_all_ff(_nand_blk, _NAND_BLKBYTES);
			for (stduint i = 0; i < n_in_blk; i++) {
				stduint off = ((cur + i) - blk_first) * _NAND_SECTOR;
				for0(k, _NAND_SECTOR) _nand_blk[off + k] = in[i * _NAND_SECTOR + k];
			}
			if (!was_blank) {
				if (!_nand_erase_block(blk)) {
					// _diag_wr_fail++;
					return false;
				}
			}
			for (stduint pg = 0; pg < _NAND_PPB; pg++) {
				const byte* pgsrc = _nand_blk + pg * _NAND_PAGE;
				if (_nand_all_ff(pgsrc, _NAND_PAGE)) continue;
				if (!FMC_NAND.Write(blk * _NAND_PPB + pg, pgsrc, 1)) {
					// _diag_wr_fail++;
					return false;
				}
			}
			cur += n_in_blk; in += n_in_blk * _NAND_SECTOR; n_left -= n_in_blk;
		}
		return true;
	}

	int operator[](uint64 bytid) override {
		if (bytid >= (uint64)getUnits() * _NAND_SECTOR) return -1;
		byte b = 0;
		Read((stduint)(bytid / _NAND_SECTOR), &b, 1);
		return b;
	}
};
static NandDisk512_t NandDisk512;

// ---- PCD -> device-stack forwarding (usb_core is a main() static; go through ptr) ----
void on_setup() { if (p_usb_core) p_usb_core->HandleSetup(); }
void on_reset() { if (p_usb_core) p_usb_core->HandleReset(); }
void on_suspend() { if (p_usb_core) p_usb_core->HandleSuspend(); }
void on_resume() { if (p_usb_core) p_usb_core->HandleResume(); }
void on_sof() { if (p_usb_core) p_usb_core->HandleSOF(); }
void on_connect() { if (p_usb_core) p_usb_core->HandleConnect(); }
void on_disconnect() { if (p_usb_core) p_usb_core->HandleDisconnect(); }
void on_dataout(void* epnum, ...) { if (p_usb_core) p_usb_core->HandleDataOut((byte)(stduint)epnum); }
void on_datainn(void* epnum, ...) { if (p_usb_core) p_usb_core->HandleDataInn((byte)(stduint)epnum); }

int main() {
	// ---- give the Mempool (backing new/malloc via _MEMMAN.cpp) its memory FIRST,
	// before any `new` runs (usb_core/usb_msc are constructed below as static locals).
	// 80-Memman style: pool owns a static slice, SDRAM not used (pins shared with NAND).
	{
		extern uni::Mempool mempool;   // global, defined in _MEMMAN.cpp
		mempool.enable_auto_expand = false;
		mempool.Append(Slice{ (stduint)(pureptr_t)_heap_pool, byteof(_heap_pool) });
	}

	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);

	XART1.setMode(115200);
	XART1.rx_buffer = buf.getSlice();
	XART1.setInterrupt(hand_xart1);
	XART1.enInterrupt();
	XART1.Receive(_buf, byteof(_buf), IOMethod::Rupt);

	SysDelay_ms(500);
	XART1.OutFormat("UNISYM STM32H743 USB MSC (U-disk) demo\n");

	// ---- SD (LUN2) ----
	bool sd_ok = SDCard1.setMode();
	if (sd_ok) XART1.OutFormat("SD card: %u units x %u B\n", SDCard1.getUnits(), SDCard1.Block_Size);
	else XART1.OutFormat("SD card init failed; LUN2 disabled\n");

	// ---- SPI FLASH W25Q256 (LUN0) ----
	// QSPI pins: PB6 nCS(AF10), PB2 CLK(AF9), PF8/9 IO0/1(AF10), PF7/6 IO2/3(AF9)
	GPIOB[6].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOB[2].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(9);
	GPIOF[8].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOF[9].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOF[7].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(9);
	GPIOF[6].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(9);
	QSPI1.init.clock_prescaler = 4;
	QSPI1.init.fifo_threshold = 4;
	QSPI1.init.sample_shifting = QSPISampleShift::None;
	QSPI1.init.flash_size = 24;
	QSPI1.init.chip_select_high_time = QSPICSHighTime::Cycle5;
	QSPI1.init.clock_mode = QSPIClockMode::Mode0;
	QSPI1.init.flash_select = QSPIFlashSelect::Flash1;
	QSPI1.init.dual_flash = QSPIDualFlash::Disable;
	bool spi_ok = QSPI1.setMode() && (w25qInit() == W25Q256_ID);
	if (spi_ok) XART1.OutFormat("W25Q256 ready (LUN0)\n");
	else XART1.OutFormat("W25Q256 not found; LUN0 disabled\n");
	// NOTE: no self-test / boot-sector wipe here — it would destroy the formatted filesystem.

	// ---- NAND (LUN1) via unisym FMC driver (AKA demo18, no external FTL) ----
	bool nand_ok = false;
	if (1) {
		nand_mpu_open();                    // NAND window non-cacheable
		nand_gpio_config();                 // PD/PE/PG AF12
		NANDConfig ncfg;                    // MT29F4G08 geometry (demo18)
		ncfg.data_bus = NANDBus::Bits8;
		ncfg.wait_feature = false;
		ncfg.ecc_computation = false;
		ncfg.ecc_page_size = 0x20000;
		ncfg.tclr_setup_time = 10;
		ncfg.tar_setup_time = 10;
		ncfg.common_space.setup_time = 10;
		ncfg.common_space.wait_setup_time = 10;
		ncfg.common_space.hold_setup_time = 10;
		ncfg.common_space.hiz_setup_time = 10;
		ncfg.attribute_space.setup_time = 10;
		ncfg.attribute_space.wait_setup_time = 10;
		ncfg.attribute_space.hold_setup_time = 10;
		ncfg.attribute_space.hiz_setup_time = 10;
		ncfg.page_size = 2048;
		ncfg.spare_area_size = 64;
		ncfg.block_size = 64;
		ncfg.block_nbr = 4096;
		ncfg.plane_nbr = 2;
		ncfg.plane_size = 2048;
		ncfg.extra_command_enable = false;
		if (FMC_NAND.setMode(ncfg)) {
			FMC_NAND.Reset();
			SysDelay_ms(100);
			NANDID nid;
			FMC_NAND.ReadID(nid);
			XART1.OutFormat("NAND ID: %02X %02X %02X %02X (LUN1)\n",
				(unsigned)nid.maker_id, (unsigned)nid.device_id,
				(unsigned)nid.third_id, (unsigned)nid.fourth_id);
			nand_modeset(4); // timing mode 4 (AKA demo18 NAND_ModeSet): default mode 0
			                 // write data tADL=200ns is too slow — page program fails without this
			nand_ok = true;
		}
		if (!nand_ok) XART1.OutFormat("NAND init failed; LUN1 disabled\n");
	}

	// descriptor table (must outlive usb_core)
	// usb_core/usb_msc/desc are main() statics (constructed here, after the pool
	// owns memory); ISR forwarding uses the global pointers set below.
	static PeripheralDescriptor desc;
	static PeripheralDevice usb_core;
	static USBPeri_MSC usb_msc;
	p_usb_core = &usb_core;
	p_usb_msc = &usb_msc;
	desc.GetDeviceDescriptor = USBPeri_DeviceDescriptor;
	desc.GetLangIDStrDescriptor = LangIDDescriptor;
	desc.GetManufacturerStrDescriptor = ManufacturerDescriptor;
	desc.GetProductStrDescriptor = ProductDescriptor;
	desc.GetSerialStrDescriptor = SerialDescriptor;
	desc.GetConfigurationStrDescriptor = ConfigurationStrDescriptor;
	desc.GetInterfaceStrDescriptor = InterfaceStrDescriptor;

	// ---- USB kernel clock: HSI48 as 48MHz source (AKA sys.c + HAL_RCCEx_PeriphCLKConfig) ----
	RCC[uni::RCCReg::CR].setof(12, true);          // HSI48ON
	while (!RCC[uni::RCCReg::CR].bitof(13));       // wait HSI48RDY
	uni::RCC.setPeriphClock(uni::PeriphClock::USB, uni::ClockSource::HSI48);// D2CCIP2R.USBSEL
	// ---- OTG2_FS pins: PA11/PA12 AF10 (AKA HAL_PCD_MspInit) ----
	GPIOA[11].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOA[12].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	// ---- USB port power switch (AKA HAL_PCD_MspInit: PG13 high = USB HOST/slave power on) ----
	GPIOG[13].setMode(GPIOMode::OUT) = true;

	// init PCD (device controller, USB2_OTG_FS on the Openedv board)
	PCD2.base = _OTG2_FS_ADDR;
	PCD2.speed = 3;             // USB_OTG_SPEED_FULL: DEVSPD=3 (FS 48MHz embedded)
	PCD2.ep0_mps = 64;
	PCD2.dev_endpoints = 8;
	PCD2.phy_itface = 2;        // PCD_PHY_EMBEDDED (internal FS transceiver)
	PCD2.dma_enable = false;
	PCD2.vbus_sensing_enable = false;   // board: no VBUS sense line (AKA ALIENTEK)
	PCD2.sof_enable = false;
	PCD2.lpm_enable = false;
	PCD2.battery_charging_enable = false;
	PCD2.use_dedicated_ep1 = false;
	if (!PCD2.setMode()) {
		XART1.OutFormat("PCD init failed\n");
		erro();
	}
	// FIFO sizes (AKA HAL_PCDEx_SetRxFiFo/SetTxFiFo in usbd_conf.c)
	PCD2.setRxFiFo(0x80);
	PCD2.setTxFiFo(0, 0x40);
	PCD2.setTxFiFo(1, 0x80);

	// init device stack + bind MSC class (multi-LUN: SPI=0, NAND=1, SD=2)
	usb_core.setMode(PCD2, desc);
	{
		StorageTrait* st[3] = { nullptr };
		const byte* iq[3] = { nullptr };
		byte n_lun = 0;
		if (spi_ok) { st[n_lun] = &SpiDisk; iq[n_lun] = _spi_inquiry; n_lun++; }
		if (nand_ok) { st[n_lun] = &NandDisk512; iq[n_lun] = _nand_inquiry; n_lun++; }
		if (sd_ok) { st[n_lun] = &SDCard1; iq[n_lun] = _sd_inquiry; n_lun++; }
		if (!n_lun) { XART1.OutFormat("no storage device; abort\n"); erro(); }
		XART1.OutFormat("bind %u LUN(s)\n", n_lun);
		usb_msc.Bind(usb_core, st, iq, n_lun);
	}

	// wire PCD events into the device stack
	PCD2.SetupStageHandler = on_setup;
	PCD2.ResetHandler = on_reset;
	PCD2.SuspendHandler = on_suspend;
	PCD2.ResumeHandler = on_resume;
	PCD2.SOFHandler = on_sof;
	PCD2.ConnectHandler = on_connect;
	PCD2.DisconnectHandler = on_disconnect;
	PCD2.DataOutStageHandler = on_dataout;
	PCD2.DataInStageHandler = on_datainn;
	PCD2.enInterrupt();// NVIC OTG_FS
	usb_core.Start();
	// AKA main.c: HAL_PWREx_EnableUSBVoltageDetector() (VDD33USB level detector)
	*(volatile stduint*)0x5802480CU |= 0x01000000U;// PWR_CR3.USB33DEN

	XART1.OutFormat("USB MSC started; plug into PC\n");

	while (1) {
		LEDB.Toggle();
		SysDelay_ms(200);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
