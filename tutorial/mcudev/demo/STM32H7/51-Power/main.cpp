// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 51-Power：待机唤醒实验（PWR STANDBY + WKUP 唤醒）
 *
 * 预期现象：
 *   1) 上电后串口打印 "51-Power: Standby Wakeup TEST"、"Normal boot"、"Entering Standby..."，随后进入 STANDBY 待机，DS0 熄灭。
 *   2) 按下 WK_UP（PA0，即 WKUP1）产生上升沿 → 系统被唤醒并复位重启，串口打印 "Woken up from Standby (WK_UP pressed)"，随后再次进入待机。
 *
 * 使用说明：
 *   1) WKUP1 = PA0（板载 WK_UP 按键），对应 unisym PWR 的 PWRWakeUpPin::WKUP1。
 *   2) 进入待机：PWR.setMode(PWRMode::STANDBY)；唤醒源配置：PWR.enWakeUpPin。
 *   3) 进待机前须禁用 RTC 唤醒源（RTC 在备份域，跨复位保留，否则会被 RTC 立即唤醒），照 HAL Sys_Enter_Standby。
 *   4) 待机唤醒后 MCU 复位重启，靠 PWR.getWakeupFlag 判断启动原因。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <c/driver/RealtimeClock.h>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// WK_UP (PA0 = WKUP1)

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// 进待机前禁用 RTC 唤醒源（照 HAL Sys_Enter_Standby：禁 WUT/闹钟/时间戳）
// RTC 在备份域，需先开备份访问(DBP)；写保护由 unisym RTC 方法内部处理
static void disable_rtc_wakeup() {
	PWR.enBkUpAccess(true);// DBP=1，开备份域写访问
	RTC.deactivateWakeUp();              // 禁唤醒定时器
	RTC.deactivateAlarm(RTCAlarm::AlarmA);// 禁闹钟 A
	RTC.deactivateAlarm(RTCAlarm::AlarmB);// 禁闹钟 B
	RTC.deactivateTimeStamp();           // 禁时间戳
	RTC.deactivateInternalTimeStamp();   // 禁内部时间戳（H7）
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// PA0 下拉，进待机前为低，WKUP1(高电平唤醒)不立刻触发

	XART1.setMode(115200);
	XART1.OutFormat("51-Power: Standby Wakeup TEST\r\n");

	// 判断启动原因：待机唤醒后 WKUPFR 的 WKUPF1 位会置 1
	if (PWR.getWakeupFlag(0x01)) {
		XART1.OutFormat("Woken up from Standby (WK_UP pressed)\r\n");
	} else {
		XART1.OutFormat("Normal boot\r\n");
	}

	// 配置 WKUP1(PA0) 为待机唤醒源：高电平/上升沿，下拉
	PWR.enWakeUpPin(PWRWakeUpPinConfig{ PWRWakeUpPin::WKUP1, 0, 2 });
	// 清除所有唤醒标志
	PWR.clearWakeupFlag(0x3F);

	XART1.OutFormat("Entering Standby... press WK_UP(PA0) to wake\r\n");
	SysDelay_ms(200);// 等串口发送完成

	// 禁用 RTC 唤醒源，避免待机后被 RTC 唤醒
	disable_rtc_wakeup();

	// 进入待机（PDDS_D1/D2/D3=1 + SLEEPDEEP + WFI）
	PWR.setMode(PWRMode::STANDBY);

	// 正常情况下不会执行到这里（待机后由 WK_UP 唤醒复位重跑）
	while (1) { SysDelay_ms(1000); }
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
