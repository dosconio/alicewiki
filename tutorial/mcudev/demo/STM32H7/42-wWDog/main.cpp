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

void hand_wwdg() {
	WWDG1.Refresh();  // 喂狗（对应 HAL_WWDG_Refresh）
	LEDR.Toggle();    // LED1 翻转，指示喂狗成功（对应 HAL_WWDG_EarlyWakeupCallback 的 LED1_Toggle）
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
	// ==== 预期现象（与 HAL 例程一致） ====
	// 1) 上电：LED0(LEDB) 点亮 300ms，串口打印 "Ciallo~" + "Hello, Happy World"
	// 2) WWDG 启动后：LED0 熄灭；计数器递减到 0x40 触发 EWI 早期唤醒中断
	//    中断里喂狗 + LED1(LEDR) 翻转 -> LED1 持续闪烁，程序不复位
	LEDB = false;     // 点亮 LED0（对应 HAL LED0(0)）
	SysDelay_ms(300); // 延时 300ms 再启动看门狗，LED0 变化可见
	WWDG1.EarlyWakeupCallback = hand_wwdg;
	WWDG1.setMode(WWDGPrescaler::Div8, 0x7F, 0x5F); // 预分频8，计数器 0x7F，窗口 0x5F
	WWDG1.enInterrupt(true);         // 使能 EWI 早期唤醒中断
	WWDG1.setInterruptPriority(2, 3); // NVIC 抢占2 子3
	WWDG1.enInterruptNVIC(true);      // 使能 NVIC WWDG 中断
	while (1) {
		LEDB = true;    // 熄灭 LED0（对应 HAL LED0(1)）
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
