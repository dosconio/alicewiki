#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/FMC>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// Not Advertisement: Openedv Board Parameters used
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];
GPIN& KEYU = GPIOA[ 0];//   Up   (WK_UP)
GPIN& KEYL = GPIOC[13];// Left  (KEY2)
GPIN& KEYD = GPIOH[ 2];// Down  (KEY1)
GPIN& KEYR = GPIOH[ 3];// Right (KEY0)

char _buf[64]; String buf(_buf, byteof(_buf));

extern "C" {
char* StrHeap(const char* valit_str){ (void)valit_str; return nullptr; }
char* StrHeapAppendChars(char* dest, char chr, size_t n){(void)dest; (void)chr; (void)n; return nullptr; }
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}
}

// 本实验开机后，显示提示信息，然后按下KEY0按键，即测试外部SDRAM容量大小并输出USART。
// 按下KEY1按键，即显示预存在外部SDRAM的数据。DS0指示程序运行状态。
// 对应正点原子实验14 SDRAM实验（HAL库版）；按键映射：KEY0=KEYR(PH3)，KEY1=KEYD(PH2)，按下为低。

// SDRAM 测试数据数组（位于 FMC SDRAM Bank1，0xC0000000）
uint16* testsram = (uint16*)FMC_SDRAM_BANK1_BASE;

// SDRAM 模式寄存器值（照实验14）
#define SDRAM_MODEREG_BURST_LENGTH_1          ((uint16)0x0000)
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL   ((uint16)0x0000)
#define SDRAM_MODEREG_CAS_LATENCY_2           ((uint16)0x0020)
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD ((uint16)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE  ((uint16)0x0200)

// FMC SDRAM 引脚复用（照实验14 HAL_SDRAM_MspInit：PC/PD/PE/PF/PG 全部 AF12）
static void sdram_gpio_raw(stduint base, const byte* pins, byte n) {
	Reference moder(base + 0x00), otyper(base + 0x04), speed(base + 0x08);
	Reference pupdr(base + 0x0C), afrl(base + 0x20), afrh(base + 0x24);
	for (byte i = 0; i < n; i++) {
		byte p = pins[i];
		moder.maset(p << 1, 2, 2);                 // AF 模式
		otyper.rstof(p);                           // 推挽
		speed.maset(p << 1, 2, 3);                 // Veryhigh
		pupdr.maset(p << 1, 2, 1);                 // 上拉
		(p < 8 ? afrl : afrh).maset((p & 7) << 2, 4, 12);  // AF12
	}
}

void sdram_gpio_config() {
	// 使能 GPIO C/D/E/F/G 时钟（AHB4ENR bits 2-6）
	for (byte port = 2; port <= 6; port++)
		RCC[RCCReg::AHB4ENR].setof(port, true);
	static const byte pc[] = {0,2,3}, pd[] = {0,1,8,9,10,14,15}, pe[] = {0,1,7,8,9,10,11,12,13,14,15},
		pf[] = {0,1,2,3,4,5,11,12,13,14,15}, pg[] = {0,1,2,4,5,8,15};
	sdram_gpio_raw(0x58020800, pc, byteof(pc));  // GPIOC
	sdram_gpio_raw(0x58020C00, pd, byteof(pd));  // GPIOD
	sdram_gpio_raw(0x58021000, pe, byteof(pe));  // GPIOE
	sdram_gpio_raw(0x58021400, pf, byteof(pf));  // GPIOF
	sdram_gpio_raw(0x58021800, pg, byteof(pg));  // GPIOG
}

// SDRAM 初始化（照实验14 SDRAM_Init + SDRAM_Initialization_Sequence）
void sdram_init() {
	sdram_gpio_config();// FMC 引脚复用（HAL_SDRAM_MspInit 部分）

	SDRAMInit ini;
	ini.bank = SDRAMBank::Bank1;
	ini.column = SDRAMColumn::C9;
	ini.row = SDRAMRow::R13;
	ini.dataWidth = SDRAMDataWidth::W16;
	ini.bankNum = SDRAMBankNum::Four;
	ini.cas = SDRAMCas::CL2;
	ini.clockPeriod = SDRAMClock::C2;
	ini.readBurst = true;
	SDRAMTiming tim;
	tim.loadToActive = 2;
	tim.exitSelfRefresh = 8;
	tim.selfRefreshTime = 6;
	tim.rowCycle = 7;// TRC 6→7：60ns→70ns ≥ W9825G6KH tRC min 65ns（参考也用6，容量测试同样16KB break）
	tim.writeRecovery = 2;
	tim.rpDelay = 2;
	tim.rcdDelay = 2;
	FMC.SDRAM.setMode(ini, tim);

	SDRAMCommand cmd;
	cmd.target = SDRAMTarget::Bank1;
	// 1) 时钟配置使能（随后至少延时 200us）
	cmd.mode = SDRAMCmd::ClockConfigEnable;
	cmd.autoRefreshNumber = 1;
	cmd.modeRegister = 0;
	FMC.SDRAM.setCommand(cmd);
	SysDelay_us(500);
	// 2) 预充电所有 bank
	cmd.mode = SDRAMCmd::PALL;
	FMC.SDRAM.setCommand(cmd);
	// 3) 自动刷新 8 次
	cmd.mode = SDRAMCmd::AutoRefresh;
	cmd.autoRefreshNumber = 8;
	FMC.SDRAM.setCommand(cmd);
	// 4) 配置模式寄存器
	cmd.mode = SDRAMCmd::LoadMode;
	cmd.autoRefreshNumber = 1;
	cmd.modeRegister = SDRAM_MODEREG_BURST_LENGTH_1 | SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL |
		SDRAM_MODEREG_CAS_LATENCY_2 | SDRAM_MODEREG_OPERATING_MODE_STANDARD | SDRAM_MODEREG_WRITEBURST_MODE_SINGLE;
	FMC.SDRAM.setCommand(cmd);
	// 5) 刷新率：COUNT = 64*1000*100/8192 - 20 = 761
	FMC.SDRAM.setRefreshRate(761);
}

// SDRAM 容量测试（照实验14 fsmc_sdram_test；只输出最终容量，避免刷屏）
void fsmc_sdram_test() {
	volatile uint32 i = 0, temp = 0, sval = 0;
	// 每隔 16K 字节写入一个递增数据，共 2048 个 = 32MB
	for (i = 0; i < 32 * 1024 * 1024; i += 16 * 1024) {
		*(volatile uint32*)(FMC_SDRAM_BANK1_BASE + i) = temp;
		temp++;
	}
	// 依次读回校验：后读出的数据必须大于第一次读到的
	for (i = 0; i < 32 * 1024 * 1024; i += 16 * 1024) {
		temp = *(volatile uint32*)(FMC_SDRAM_BANK1_BASE + i);
		if (i == 0) sval = temp;
		else if (temp <= sval) break;
	}
	XART1.OutFormat("SDRAM Capacity:%dKB\r\n", (uint16)(temp - sval + 1) * 16);
}

// 按键扫描（照实验14 KEY_Scan，不支持连按；返回 0 无按键 / 1=KEY0 / 2=KEY1）
byte key_scan() {
	static bool key_up = true;
	if (key_up && (!KEYR || !KEYD)) {
		SysDelay_ms(10);
		key_up = false;
		if (!KEYR) return 1;// KEY0 (PH3)
		else if (!KEYD) return 2;// KEY1 (PH2)
	}
	else if (KEYR && KEYD) key_up = true;
	return 0;
}

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	LEDB.setMode(GPIOMode::OUT) = !false;// DS0
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// WK_UP (PA0)
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);// KEY2 (PC13)
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);// KEY1 (PH2)
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);// KEY0 (PH3)

	XART1.setMode(115200);

	// 显示提示信息
	XART1.OutFormat("APOLLO STM32H7\r\n");
	XART1.OutFormat("SDRAM TEST\r\n");
	XART1.OutFormat("KEY0:Test SDRAM\r\n");
	XART1.OutFormat("KEY1:TEST Data\r\n");

	sdram_init();// 初始化 SDRAM

	// 预存测试数据到外部 SDRAM
	uint32 ts;
	for (ts = 0; ts < 25000; ts++) testsram[ts] = (uint16)ts;

	byte i = 0;
	while (true) {
		byte key = key_scan();
		if (key == 1) {
			fsmc_sdram_test();// KEY0：测试 SDRAM 容量
			for (ts = 0; ts < 25000; ts++) testsram[ts] = (uint16)ts;// 重新预存测试数据
		}
		else if (key == 2) {// KEY1：显示预存数据
			for (ts = 0; ts < 25000; ts++)
			{
				// XART1.OutFormat("testsram[%d]:%d\r\n", ts, testsram[ts]);
				if (ts != testsram[ts]) XART1.OutFormat("testsram[%d]:%d\r\n", ts, testsram[ts]);
			}
			XART1.OutFormat("Test OK");
		}
		else SysDelay_ms(10);
		i++;
		if (i == 20) {// DS0 闪烁
			i = 0;
			LEDB.Toggle();
		}
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
