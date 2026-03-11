

// Version of EasyX

#include <MessageBox>
//#include <winuser.h>
#include <easyx.h>

#undef MessageBox

//{TODO} LPCWSTR 'const wchar_t*'

namespace uni {
	void Witch::MessageBox::Show() {
		state = ::MessageBoxA(
			GetHWnd(), text, title, button_type);
	}
}
