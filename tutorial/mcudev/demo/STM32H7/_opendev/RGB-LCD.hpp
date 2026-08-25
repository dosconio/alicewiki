// ASCII CPP-ISO11 TAB4 CRLF
// RGB-LCD.cpp（共享 LTDC + SDRAM 初始化）的头文件
// 用法：main.cpp 中 #include "../_opendev/RGB-LCD.hpp"；
//       RGB-LCD.cpp 需作为源文件加入工程编译（09-RTC 的 main.uvprojx 已添加）
#ifndef _OPEDEV_RGB_LCD_H
#define _OPEDEV_RGB_LCD_H

#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/LTDC>
#include <cpp/Device/FMC>
#include "SDRAM.hpp"// 提供 void sdram_init();（定义于 SDRAM.cpp）

// 初始化 SDRAM（帧缓冲）与 LTDC（引脚/像素时钟/时序/层），点亮背光
void ltdc_init();
// 在指定位置以红色 16x8 点阵输出一行文本
void ltdc_text(stduint x, stduint y, const char* s);

#endif
