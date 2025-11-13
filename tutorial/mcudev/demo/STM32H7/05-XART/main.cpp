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

char _buf[16 + 1]; String buf(_buf, byteof(_buf) - 1);

extern "C" {
char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}
}

void hand_xart1() {
	// while (!XART1.isReady());
	stduint timeout = 0x1FFFF;
	if (buf[0] == '\r') XART1.ClearBuffer();
	if (buf[XART1.getBufferPointer() - 1] == '\n') {
		//  assume \r\n
		buf[XART1.getBufferPointer() - 1] = 0;
		XART1.OutFormat("Hello, %s\n", buf.reference());
		XART1.ClearBuffer();
	}
	if (XART1.isReady()) XART1.innByInterrupt();
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
	XART1.innByInterrupt();
	
	SysDelay_ms(1000);
	buf.Format("Ciallo~\n");
	XART1.OutFormat(buf.reference());
	XART1.OutFormat("Hello, %s %s\n", "Happy", "World");
	
	while (true) {
		LEDB.Toggle();
		//XART1.out("ciallo ", 7);
		SysDelay_ms(5000);
	}
}

void erro(char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
