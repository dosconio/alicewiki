// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 09-RTC：RTC 实时时钟 + 闹钟演示（LTDC 屏显 + 串口输出）
 *
 * 预期现象：
 *   1) 上电后 4.3 寸 RGB 屏显示标题与当前时间/日期/星期，每秒刷新；
 *      DS0（LEDB）每 100ms 翻转，指示程序运行。
 *   2) 上电 5 秒后 RTC 闹钟 A 触发：串口打印 "ALARM A! hh:mm:ss"，
 *      屏幕底部显示红色 "ALARM!!!"，DS1（LEDR）点亮。
 *   3) 按 KEY0 解除闹钟指示（LEDR 熄灭、红色提示消失），并重新预置闹钟
 *      （当前时间 +5 秒，按星期精确匹配），5 秒后再次触发。
 *
 * 使用说明：
 *   1) 首次运行自动写入初始时间 10:50:00、日期 2017-08-13（星期天），
 *      并写入备份寄存器 0x5050 作标记；此后复位不再覆盖，RTC 持续走时。
 *   2) RTC 时钟源为 LSE（32.768kHz 外部晶振），24 小时制。
 *   3) 串口 115200 输出：初始化信息与每次闹钟触发的时刻。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <c/data.h>
#include <c/driver/RealtimeClock.h>
#include "../_opendev/RGB-LCD.hpp"   // LTDC + SDRAM 共享初始化（RGB-LCD.cpp 已加入工程）
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// 板载资源
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];//   Up   (WK_UP)
GPIN& KEYL = GPIOC[13];// Left  (KEY2)
GPIN& KEYD = GPIOH[ 2];// Down  (KEY1)
GPIN& KEYR = GPIOH[ 3];// Right (KEY0)

char _buf[64]; String buf(_buf, byteof(_buf));

extern "C" {
char* StrHeap(const char* valit_str){ (void)valit_str; return nullptr; }
char* StrHeapAppendChars(char* dest, char chr, size_t n){(void)dest; (void)chr; (void)n; return nullptr; }
char* salc(size_t size){ return 0; }
void outtxt(const char* str, stduint len) {XART1.out(str, len);}
}

// ---- RTC 闹钟 A ----
volatile bool alarm_fired = false;
void hand_alarm() { alarm_fired = true; }

// 预置闹钟 A：当前时间 + offset 秒（按星期精确匹配时分秒，对应 HAL 的 RTC_Set_AlarmA）
static void rtc_alarm_arm(byte offset) {
	datime_t t = {}, d = {};
	RTC.getTime(t, RTCFormat::Bin);
	RTC.getDate(d, RTCFormat::Bin);
	int s = t.second + offset;
	t.minute += s / 60; s %= 60; t.second = (byte)s;
	if (t.minute >= 60) { t.minute -= 60; t.hour = (t.hour + 1) % 24; }
	// unisym weekday()：0=周日..6=周六；RTC 星期域：1=周一..7=周日
	byte wd = (byte)(((int)weekday((word)(1900 + d.year), (word)(d.month + 1), (word)d.mday) + 6) % 7 + 1);
	RTC.setAlarm(RTCAlarm::AlarmA, t, RTCAlarmMask::None, wd, RTCAlarmSel::WeekDay,
		RTCFormat::Bin, 0, RTCAlarmSubSecondMask::None);
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;// DS0
	LEDR.setMode(GPIOMode::OUT) = !false;// DS1
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// WK_UP (PA0)
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);// KEY2 (PC13)
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);// KEY1 (PH2)
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);// KEY0 (PH3)

	XART1.setMode(115200);
	XART1.OutFormat("Apollo STM32H7\r\n");
	XART1.OutFormat("RTC + ALARM TEST\r\n");

	sdram_init(); // 初始化 SDRAM（帧缓冲位于 0xC0000000）
	ltdc_init();  // 初始化 LTDC（引脚 + PLL3 像素时钟 + 时序 + 层）

	XART1.OutFormat("LTDC OK, PixClk=%dHz\r\n", LTDC.getFrequency());

	// ---- RTC：LSE 时钟源，24 小时制 ----
	RTC.enClock(RTCClockSource::LSE);
	RTC.init(RTCHourFormat::Hour24, 0x7F, 0xFF);
	if (RTC.bkupRead(0) != 0x5050) {// 首次配置（备份寄存器标记），设置初始时间日期
		datime_t t = {};
		t.hour = 10; t.minute = 50; t.second = 0;
		RTC.setTime(t, RTCFormat::Bin);
		datime_t d = {};
		d.year = 117; d.month = 7; d.mday = 13;// 2017-08-13
		RTC.setDate(d, RTCFormat::Bin);
		RTC.bkupWrite(0, 0x5050);
	}
	XART1.OutFormat("RTC OK\r\n");

	// 闹钟 A 中断：NVIC 优先级 1/2，预置 +5 秒
	RTC.setInterrupt(hand_alarm);
	RTC.setInterruptPriority(1, 2);
	RTC.enInterrupt();
	rtc_alarm_arm(5);

	bool alarm_on = false;// 闹钟正在响铃（红字 + DS1）
	bool key0_prev = true;// KEY0 释放态（上拉=1）

	char _tb[64];
	while (true) {
		datime_t t = {}, d = {};
		RTC.getTime(t, RTCFormat::Bin);
		RTC.getDate(d, RTCFormat::Bin);

		if (alarm_fired) {
			alarm_fired = false;
			alarm_on = true;
			XART1.OutFormat("ALARM A! %02d:%02d:%02d\r\n", t.hour, t.minute, t.second);
		}
		bool k0 = (bool)KEYR;// 按下=0
		if (!k0 && key0_prev) {// KEY0 按下沿：解除闹钟指示并重新预置
			alarm_on = false;
			rtc_alarm_arm(5);
			XART1.OutFormat("Alarm reset, next +5s\r\n");
		}
		key0_prev = k0;

		// 清屏（黑底）再重绘，避免旧字符残留
		LTDC[1].DrawRectangle(GrafRect(0, 0, 800, 480, Color::Black));
		ltdc_text(30, 50, "Apollo STM32H7");
		ltdc_text(30, 70, "RTC + ALARM TEST");

		String ts(_tb, byteof(_tb));
		ts.Format("Time:%02d:%02d:%02d", t.hour, t.minute, t.second);
		ltdc_text(30, 140, ts.reference());
		ts.Format("Date:20%02d-%02d-%02d", d.year % 100, d.month + 1, d.mday);
		ltdc_text(30, 160, ts.reference());
		unsigned wd = weekday((word)(1900 + d.year), (word)(d.month + 1), (word)d.mday);
		ts.Format("Week:%d", (wd == 0) ? 7 : wd);// weekday() 返回 0=周日，映射为 7
		ltdc_text(30, 180, ts.reference());

		if (alarm_on) ltdc_text(30, 210, "ALARM!!!");
		LEDR = alarm_on;
		LEDB.Toggle();// DS0 闪烁指示运行
		SysDelay_ms(100);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
