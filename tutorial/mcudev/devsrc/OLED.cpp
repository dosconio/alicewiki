#include <cpp/unisym>
#include <cpp/Device/IIC>
#include <cpp/Device/_Video.hpp>

#include "../device/OLED.h"

void OLED_t::setOutput() {
	SendCmd(0xAE); //display off
	SendCmd(0x20);	//Set Memory Addressing Mode	
	SendCmd(0x10);	//00,Horizontal Addressing Mode;01,Vertical Addressing Mode;10,Page Addressing Mode (RESET);11,Invalid
	SendCmd(0xb0);	//Set Page Start Address for Page Addressing Mode,0-7
	SendCmd(0xc8);	//Set COM Output Scan Direction
	SendCmd(0x00); //---set low column address
	SendCmd(0x10); //---set high column address
	SendCmd(0x40); //--set start line address
	SendCmd(0x81); //--set contrast control register
	SendCmd(0xff); //
	SendCmd(0xa1); //--set segment re-map 0 to 127
	SendCmd(0xa6); //--set normal display
	SendCmd(0xa8); //--set multiplex ratio(1 to 64)
	SendCmd(0x3F); //
	SendCmd(0xa4); //0xa4,Output follows RAM content;0xa5,Output ignores RAM content
	SendCmd(0xd3); //-set display offset
	SendCmd(0x00); //-not offset
	SendCmd(0xd5); //--set display clock divide ratio/oscillator frequency
	SendCmd(0xf0); //--set divide ratio
	SendCmd(0xd9); //--set pre-charge period
	SendCmd(0x22); //
	SendCmd(0xda); //--set com pins hardware configuration
	SendCmd(0x12);
	SendCmd(0xdb); //--set vcomh
	SendCmd(0x20); //0x20,0.77xVcc
	SendCmd(0x8d); //--set DC-DC enable
	SendCmd(0x14); //
	SendCmd(0xaf); //--turn on oled panel
}

uni::VideoControlBlock OLED_t::getControlBlock() {
	return uni::VideoControlBlock(getControlInterface(), uni::Size2(128, 64), uni::Color::Black);
}

void OLED_VCI_T::SetCursor(const uni::Point& disp) const {
	oled->SendCmd(0xb0 + disp.y / 8);
	oled->SendCmd(((disp.x & 0xf0) >> 4) | 0x10);
	oled->SendCmd((disp.x & 0x0f) | 0x01);
}

void OLED_VCI_T::DrawPoint(const uni::Point& disp, uni::Color color) const {
	if (!oled->page) return;
	// 128 * (64)
	byte& pnt = (*oled->page)[disp.y / 8][disp.x];
	byte  pos = disp.y % 8;
	if (color == uni::Color::White)
		pnt |= _IMM1S(pos);
	else
		pnt &=~_IMM1S(pos);
	//SetCursor(uni::Point(disp.x, disp.y & 0b111000));
	//oled->SendDat(pnt);
}

void OLED_VCI_T::DrawRectangle(const uni::Rectangle& rect) const {
	for (int i = rect.x; i < rect.x + rect.width; i++)
		for (int j = rect.y; j < rect.y + rect.height; j++)
			DrawPoint(uni::Point(i, j), rect.color);
}

uni::Color OLED_VCI_T::GetColor(uni::Point disp) const {
	using namespace uni;
	if (!oled->page) return Color::Black;
	byte& pnt = (*oled->page)[disp.y / 8][disp.x];
	byte  pos = disp.y % 8;
	return (pnt & _IMM1S(pos)) ? Color::White : Color::Black;
}




