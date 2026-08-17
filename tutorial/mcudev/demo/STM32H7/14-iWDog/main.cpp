#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/Watchdog>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

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
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

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
	//rx_frame_ready = true;
	if (rx_frame_ready) {
		XART1.abortReceive();// this method will lost data at interval
	}
}

int main() {
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
	buf.Format("Ciallo~\n");
	XART1.OutFormat(buf.reference());
	XART1.OutFormat("Hello, %s %s\n", "Happy", "World");
	SysDelay_ms(100);
	// 溢出时间 Tout = (4 x 2^prer x rlr) / 32 (ms)，LSI = 32kHz
	// Div64 对应 prer = 4：Tout = (64 x rlr) / 32 = 2 x rlr (ms)
	// 取 rlr = 2500 -> Tout = 5000ms = 5s
	IWDG1.setMode(IWDGPrescaler::Div64, 2500);
	// ==== 预期现象（与 HAL 例程一致：LED 常亮，复位时闪一下） ====
	// 1) 上电复位：串口打印 "Ciallo~" + "Hello, Happy World"，LEDR 点亮，LEDB 开始闪烁
	// 2) 一直按住 KEYU(WK_UP)：持续喂狗不复位 -> LEDR 常亮不闪，串口只打印一次
	// 3) 松开 KEYU：约 5s 后看门狗复位 -> 复位瞬间 LEDR 熄灭一下再重新点亮(GPIO 被复位)，串口再次打印，循环往复
	LEDR = false;                              // 点亮 LEDR，表示看门狗已启动
	while (1) {
		LEDB.Toggle();
		if (KEYU) IWDG1.Refresh();  // 按下 WK_UP -> 喂狗
		//XART1.out("ciallo ", 7);
		if (rx_frame_ready) {
			XART1.OutFormat("Hello, %s\n", buf.reference());
			XART1.ClearBuffer();
			rx_frame_ready = false;
			XART1.Receive(_buf, byteof(_buf), IOMethod::Rupt); 
			continue;
        }
		SysDelay_ms(250);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
