
#include <MCU/Intel/sdcc8051.h> // #include <mcs51/8051.h>
#include <MCU/delay.h>
#include <driver/LCD1602.h>

void main()
{
	LCD1602_Initialize();
	LCD1602_Clear();
	LCD1602_CursorSet(0, 0);
	LCD1602_OutString("Hello,");
	LCD1602_CursorSet(1, 1);
	LCD1602_OutString("World!");
	LCD1602_Outi32hex(0x20240303);
	while (1)
		;
}
