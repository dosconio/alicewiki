
// Version of EasyX

#include <Form>
#include <easyx.h>

#include <iostream>//dbg

namespace uni { namespace Witch {
	void Form::Show() {
		initgraph(width, height, EX_SHOWCONSOLE);
		setbkmode(0);// transparent
		Node* nod = Controls.Root();
		while (nod) {
			Control* ctrl = (Control*)nod->offs;
			ctrl->Show();
			nod = nod->next;
		}
	}

	int Form::Loop() {
		ExMessage msg;
		while (true) {
			if (peekmessage(&msg, EX_MOUSE)) {
				Node* nod = Controls.Root();
				if (nod->offs) do {
					Control* ctrl = (Control*)nod->offs;
					// std::cout << "msgx" << msg.x << " msgy" << msg.y << std::endl;
					if (msg.x >= ctrl->left && msg.x <= (ctrl->left + ctrl->width) && msg.y >= ctrl->top && msg.y <= (ctrl->top + ctrl->height)) {
						//std::cout << "msgx" << msg.x << " msgy" << msg.y << std::endl;
						ctrl->Event(&msg);
					}
						
					
				} while (nod = nod->next);

				switch (msg.message) {
				case WM_RBUTTONDOWN:
					return 0;//{TEMP}
					break;
				default:
					break;
				}
			}
			BeginBatchDraw();
			cleardevice();
			Node* nod = Controls.Root();
			while (nod) {
				Control* ctrl = (Control*)nod->offs;
				ctrl->Render();
				nod = nod->next;
			}
			EndBatchDraw();
		}
		return 0;
	}


} }

