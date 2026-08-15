#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
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

// TIM3 update interrupt: toggle LEDR every 500ms (200MHz/20000 = 10kHz; 5000 ticks -> 500ms)
void hand_tim3() {
	LEDR.Toggle();
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();
	
	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	TIM3.setMode(20000, 5000);// 200MHz/20000 = 10kHz; 5000 ticks -> 500ms
	TIM3.setInterrupt(&hand_tim3);
	TIM3.enInterrupt();
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
	while (1) {
		LEDB.Toggle();
		//XART1.out("ciallo ", 7);
		if (rx_frame_ready) {
			XART1.OutFormat("Hello, %s\n", buf.reference());
			XART1.ClearBuffer();
			rx_frame_ready = false;
			XART1.Receive(_buf, byteof(_buf), IOMethod::Rupt); 
			continue;
        }
		SysDelay_ms(200);
	}
	while (true) {
		LEDB.Toggle();
		//XART1.out("ciallo ", 7);
		SysDelay_ms(1000);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
