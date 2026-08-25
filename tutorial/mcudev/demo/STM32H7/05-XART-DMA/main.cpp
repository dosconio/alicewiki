// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 05-XART-DMA：USART1 DMA 发送
 *
 * 预期现象：
 *   1) 上电后串口打印 "DMA TEST"；DS0（LEDB）每 200ms 翻转指示运行。
 *   2) 按 KEY0：串口打印 "DMA DATA:"，DMA2 后台把 7800 字节搬到 USART1 发送，
 *      期间 DS0（LEDB）闪烁指示 CPU 空闲；发送完成打印 "Transmit Finished!"。
 *
 * 使用说明：
 *   1) 串口 115200；DMA2 Stream0，DMAMUX 请求线 = USART1_TX（getDMARequestID 自动获取）。
 *   2) 发送缓冲 7800 字节，由文本 + \r\n 循环填充，一次 DMA 单次传输。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYR = GPIOH[ 3];// KEY0 按下=0

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

#define SEND_BUF_SIZE 7800
static byte SendBuff[SEND_BUF_SIZE];
static const char TEXT_TO_SEND[] = "unisym Apollo STM32H7 DMA TEST";

// 配置 DMA2 Stream0 为 USART1_TX，并绑定到 XART1.hdmatx
static void dma_tx_init() {
	DMA2.enClock();                                  // 使能 DMA2 + DMAMUX1 时钟
	const DMAStream& tx = DMA2[0];                   // DMA2 Stream0
	tx[0].setMode(false, true, false, true, false, 2, 2, 0);// M2P，字节，内存自增，单次
	tx[0].setRequest(XART1.getDMARequestID(true));   // DMAMUX 请求线 = USART1_TX
	tx.setInterruptPriority(1, 0);
	tx.enInterruptNVIC(true);
	XART1.hdmatx = &tx;
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);// KEY0 (PH3)

	XART1.setMode(115200);
	XART1.enInterrupt();// 开 UART NVIC：DMA 搬完后靠 TC 中断释放 lock_t
	XART1.OutFormat("DMA TEST\r\n");
	dma_tx_init();

	// 填充发送缓冲：文本 + \r\n 循环
	{
		stduint j = sizeof(TEXT_TO_SEND), t = 0, mask = 0;
		for0(i, SEND_BUF_SIZE) {
			if (t >= j) {
				if (mask) { SendBuff[i] = 0x0a; t = 0; }
				else      { SendBuff[i] = 0x0d; mask++; }
			} else {
				mask = 0;
				SendBuff[i] = (byte)TEXT_TO_SEND[t];
				t++;
			}
		}
	}

	bool key0_prev = true;
	while (1) {
		bool k0 = (bool)KEYR;
		if (!k0 && key0_prev) { // KEY0 按下沿
			XART1.OutFormat("\r\nDMA DATA:\r\n");
			if (XART1.out((const char*)SendBuff, SEND_BUF_SIZE, IOMethod::DMA) != SEND_BUF_SIZE) {
				XART1.OutFormat("DMA start failed\r\n");
			} else {
				// DMA 后台搬运，CPU 只点灯（传输期间勿打印，否则与 DMA 数据流交织）
				while (!XART1.isReady()) {
					LEDB.Toggle();
					SysDelay_ms(100);
				}
				XART1.OutFormat("\r\nTransmit Finished!\r\n");
			}
		}
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
