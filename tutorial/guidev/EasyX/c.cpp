//{TEMP} no check for mem_leak
#include <iostream>
#include <Form>
#include <MessageBox>
#include <Control/Button>

using namespace uni::Witch;
// EasyX 只支持创建一个窗口，不好玩
// inline static int width() { return getwidth(); };
// inline static int height() { return getheight(); };

int main() {
	Form win(300, 200);
	Button btn(&win, 10, 20, 30, 40);
	btn.setBackcolor(0xEE11EE);

	win.Show();
	MessageBox("Hello", "MsgBox").Show();//{BAD} must: Form win is ON show
	
	//btn.Show();
	return win.Loop();
}
