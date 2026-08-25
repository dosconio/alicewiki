// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 17-EEPROM：24C02 EEPROM 读写（软件 I2C）
 *
 * 预期现象：
 *   1) 上电后串口打印 "24C02 Ready!"（检测失败则打印 "24C02 Check Failed!"）。
 *   2) 按 KEY1（PH2）：把 "Apollo STM32H7 IIC TEST" 写入 24C02 地址 0，串口打印 "Write OK"。
 *   3) 按 KEY0（PH3）：从地址 0 读回并串口打印 "Read: Apollo STM32H7 IIC TEST"。
 *
 * 使用说明：
 *   1) 24C02 挂在 PH4(SCL)/PH5(SDA)，7 位从机地址 0x50，容量 256 字节。
 *   2) 用软件 I2C（IIC_SOFT）位翻转，无需配置 I2C 外设时钟。
 *   3) 检测标志 0x55 存于 24C02 末尾地址 255，首次上电自动写入。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <new>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYD = GPIOH[ 2];// KEY1 按下=0
GPIN& KEYR = GPIOH[ 3];// KEY0 按下=0

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

#define EE_ADDR 0x50   // 24C02 7-bit slave address
static const char TEXT_BUF[] = "Apollo STM32H7 IIC TEST";
#define SIZE (sizeof(TEXT_BUF))

void iic_delay() { for (volatile int i = 0; i < 100; i++) {} }
static byte ee_wire_buf[byteof(IIC_SOFT)];
static IIC_SOFT* ee = nullptr;

// write bytes to EEPROM (byte-at-a-time)
static void ee_write(word addr, const byte* buf, word len) {
	for (word i = 0; i < len; i++) {
		ee->SendStart(EE_ADDR);                 // START + 0xA0 (write)
		ee->IIC_t::Send((byte)(addr + i), true);// memory address
		ee->IIC_t::Send(buf[i], true);          // data byte
		ee->SendStop();
		SysDelay_ms(5);                          // EEPROM internal write cycle
	}
}

// read bytes from EEPROM (sequential read)
static void ee_read(word addr, byte* buf, word len) {
	ee->SendStart(EE_ADDR);                     // START + 0xA0 (write)
	ee->IIC_t::Send((byte)addr, true);          // memory address
	ee->SendStart();                            // repeated START
	ee->IIC_t::Send((byte)((EE_ADDR << 1) | 1), true); // 0xA1 (read)
	for (word i = 0; i < len; i++)
		buf[i] = ee->ReadByte(true, i != len - 1); // ACK, NACK on last
	ee->SendStop();
}

static bool ee_check() {
	byte t = 0;
	ee_read(255, &t, 1);
	if (t == 0x55) return true;
	byte flag = 0x55;
	ee_write(255, &flag, 1);
	ee_read(255, &t, 1);
	return t == 0x55;
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true);

	XART1.setMode(115200);
	XART1.OutFormat("EEPROM (24C02) soft-I2C PH4/PH5\r\n");

	// software I2C: SDA=PH5, SCL=PH4
	ee = new (ee_wire_buf) IIC_SOFT(GPIOH[5], GPIOH[4]);
	ee->func_delay = iic_delay;

	if (ee_check()) XART1.OutFormat("24C02 Ready!\r\n");
	else             XART1.OutFormat("24C02 Check Failed!\r\n");

	byte datatemp[SIZE];
	bool key1_prev = true, key0_prev = true;
	while (1) {
		bool k1 = (bool)KEYD, k0 = (bool)KEYR;
		if (!k1 && key1_prev) { // KEY1: write
			ee_write(0, (const byte*)TEXT_BUF, SIZE);
			XART1.OutFormat("Write OK\r\n");
		}
		if (!k0 && key0_prev) { // KEY0: read
			ee_read(0, datatemp, SIZE);
			XART1.OutFormat("Read: %s\r\n", (const char*)datatemp);
		}
		key1_prev = k1;
		key0_prev = k0;
		SysDelay_ms(10);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
