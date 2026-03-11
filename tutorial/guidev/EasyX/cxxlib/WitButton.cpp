// Version of EasyX

#include <Control/Button>
#include <easyx.h>

namespace uni { namespace Witch {
//
void Button::Show() {
	COLORREF& bkcolor = (COLORREF&) backcolor;
	setlinecolor(RGB(170, 170, 170));
	setfillcolor(bkcolor);
	fillroundrect(left, top, left + width, top + height, 5, 5);
}

void Button::setBackcolor(uint32 bkcolor) {
	this->backcolor = bkcolor;
}

void Button::Event(void* event) {
	ExMessage* msg = (ExMessage*)event;
	switch (msg->message) {
	case WM_LBUTTONDOWN:
		std::cout << "LBTN \n";
		break;
	default:
		break;
	}
}

void Button::Render() {
	//this->left++;
	this->Show();
	Sleep(10);
}

//
}}
