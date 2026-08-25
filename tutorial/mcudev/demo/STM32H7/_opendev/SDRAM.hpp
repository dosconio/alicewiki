// ASCII CPP-ISO11 TAB4 CRLF
// SDRAM.cpp（共享 外部 SDRAM 初始化）的头文件
// 用法：main.cpp 中 #include "../_opendev/SDRAM.hpp"；
//       SDRAM.cpp 需作为源文件加入工程编译（30-SD-FatFS 的 main.uvprojx 已添加）
#ifndef _OPEDEV_SDRAM_H
#define _OPEDEV_SDRAM_H

#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/FMC>

// 外部 SDRAM Bank1 基地址（0xC0000000），缓冲/帧缓冲可置于此
#define SDRAM_BANK1_BASE FMC_SDRAM_BANK1_BASE

// 初始化外部 SDRAM：FMC 引脚复用 + setMode + 时钟使能/预充电/自动刷新/模式寄存器 + 刷新率
void sdram_init();

#endif
