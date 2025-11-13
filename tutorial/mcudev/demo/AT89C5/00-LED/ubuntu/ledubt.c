#include <MCU/Intel/sdcc8051.h> // #include <mcs51/8051.h>

#define delaya delay(100)

void LED_SingleRun(byte logic);
void delay(unsigned int xn);

void main() 
{
	while (1)
	{
		LED_SingleRun(0);
		delaya;
		LED_SingleRun(1);
	}
}

void LED_SingleRun(byte logic)
{
	byte stream = 1;
	byte direction = 0;
	while (1)
	{
		outpb(p2, logic ? stream : ~stream);
		delaya;
		direction ? ((stream >>= 1) || (direction=0, stream=1)) :
			((stream <<= 1) || (direction=1, stream=0b01000000));
		if (stream == 1) break;
	}
	outpb(p2, logic ? stream : ~stream);
	delaya;
}

void delay(unsigned int xn){
	unsigned int a, b;
	for(a = xn; a > 0; a--)
		for(b = 100; b > 0; b--)
			;
}
