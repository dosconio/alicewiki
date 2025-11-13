// ASCII C++ TAB4 CRLF
// @dosconio
// 20240427-06:22

#include "E:/PROJ/SVGN/unisym/asm/data/_asm_inst.c"
#include "E:/PROJ/SVGN/unisym/inc/cpp/string"
#include <iostream>
#include <fstream>

int main() {
	char buf[128];
	for (int i = 0; i < sizeof(_asm_inst_literal_list) / sizeof(_asm_inst_literal_list[0]); i++) {
		StrCopy(buf, "E:/tmp/asmnote/");
		StrAppend(buf, _asm_inst_literal_list[i]);
		StrAppend(buf, ".md");
		std::ofstream out(buf);
		out.close();
		std::cout << buf << std::endl;
	}
    return 0;
}
