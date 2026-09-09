// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 92-AudioPlay：基于 WM8978 与 SAI1 的 SD 卡 WAV 音乐播放器 (UNI库)
 *
 * 预期现象：
 *   1) 上电初始化 Mempool、SDRAM、LTDC、WM8978 音频芯片、SAI1 接口与 SD 卡 FAT 文件系统。
 *   2) LCD 显示播放器界面（Alice.Wiki 92-AudioPlay、曲目信息、播放进度/时间、采样率、码率）。
 *   3) 自动从 SD 卡扫描 WAV 音乐文件（根目录 .wav 文件）。
 *   4) 按键操作：
 *      - KEY0 (PH3): 下一曲
 *      - KEY2 (PC13): 上一曲
 *      - WK_UP (PA0): 暂停 / 播放
 *   5) DS0 (LEDB / PB0) 翻转指示运行状态。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include <cpp/Device/SAI>
#include <cpp/Device/DMA>
#include <cpp/Device/DBG>
#include <c/format/filesys/FAT.h>
#include <c/mempool.h>
#include <c/data.h>
#include <c/consio.h>
#include <c/ustring.h>
#include "../../../device/audio/wm8978.hpp"
#include "../../../device/audio/es8388.hpp"
#include "../_opendev/RGB-LCD.hpp"
#include "../_opendev/SDRAM.hpp"
#include <new>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

extern uni::Mempool mempool;// 由 _opendev/_MEMMAN.cpp 定义

// 板载指示灯与按键
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// WK_UP (PA0)
GPIN& KEYL = GPIOC[13];// KEY2  (PC13)
GPIN& KEYD = GPIOH[ 2];// KEY1  (PH2)
GPIN& KEYR = GPIOH[ 3];// KEY0  (PH3)

void outtxt(const char* str, stduint len) { XART1.out(str, len); }

// Mempool 管理的内存切片（照 80-Memman / 91-ImageShow：SDRAM + SRAM12 + SRAM4，不用 AXI/ITCM）。
// 避开 LTDC 帧缓冲（SDRAM_BANK1_BASE 起 ~0xC0000）与下方 FAT 缓冲（+0x100000 起 1KB）。
#define SDRAM_POOL_BASE      (SDRAM_BANK1_BASE + 0x100000 + 0x10000)
#define SDRAM_POOL_SIZE      (4U * 1024U * 1024U)
#define SRAM12_ADDR          ((stduint)0x30000000)
#define SRAM4_ADDR           ((stduint)0x38000000)

#define IMAGE_CARD_BLOCK_SIZE 512
#define IMAGE_FILE_BLOCK_SIZE IMAGE_CARD_BLOCK_SIZE
#define IMAGE_PROFILE         0
#define IMAGE_CARD_CACHE      1
#define IMAGE_CARD_CACHE_BLOCKS 32
#define IMAGE_CARD_CACHE_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_CACHE_BLOCKS)
#define IMAGE_CARD_CACHE_INVALID_BLOCK ((stduint)-1)
#define IMAGE_CARD_READAHEAD  0
#define IMAGE_CARD_READAHEAD_BLOCKS 8
#define IMAGE_CARD_READ_TIMEOUT_MS 1000
#define IMAGE_CARD_READAHEAD_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_READAHEAD_BLOCKS)
#define IMAGE_FAT_BUFFER_SIZE IMAGE_CARD_BLOCK_SIZE

// FAT 缓冲（外部 SDRAM，避开 LTDC 帧缓冲与 MEMPOOL 切片）
static byte* sector_buf      = (byte*)(SDRAM_BANK1_BASE + 0x100000);        // 扇区缓冲 512B
static byte* fat_buf         = sector_buf + IMAGE_FAT_BUFFER_SIZE;          // FAT 表缓冲 512B

#include "../_opendev/_ImageProfile.hpp"

// 音频 DMA 缓冲（必须位于 D2 域 SRAM1/2，以供 DMA1 访问，不能放在 SDRAM）
#define AUDIO_DMA_BUF_SIZE   8192                                           // DMA 传输缓冲区大小（字节）
#define AUDIO_RING_SLOTS     4
static byte* ring_buf[AUDIO_RING_SLOTS] = {
	(byte*)(0x30000000 + 0x0000),// slot 0: DMA M0 buffer
	(byte*)(0x30000000 + 0x2000),// slot 1: DMA M1 buffer
	(byte*)(0x30000000 + 0x4000),// slot 2: read-ahead buffer
	(byte*)(0x30000000 + 0x6000),// slot 3: read-ahead buffer
};
static byte* audio_temp_buf  = nullptr;                                     // 格式转换临时缓冲，FAT 挂载后由 mempool 分配
#define saibuf0 ring_buf[0]
#define saibuf1 ring_buf[1]

// 软件 I2C 延时与驱动实例（放慢延时以适配 480MHz 主频下的 I2C 上升时间）
static void iic_delay() {
	for (volatile int i = 0; i < 500; i++) {}
}

class AudioIIC : public uni::IIC_SOFT {
public:
	AudioIIC(uni::GPIN& sda, uni::GPIN& scl) : uni::IIC_SOFT(sda, scl, false) {
		push_pull = true;
		setMode();
	}
};

enum class AudioCodecType {
	None,
	WM8978,
	ES8388
};

static AudioCodecType active_codec = AudioCodecType::None;
static byte audio_wire_buf[byteof(AudioIIC)];
static byte wm8978_buf[byteof(WM8978_t)];
static byte es8388_buf[byteof(ES8388_t)];
static AudioIIC* audio_iic = nullptr;
static WM8978_t* wm8978 = nullptr;
static ES8388_t* es8388 = nullptr;

static void codec_set_i2s(byte bits_per_sample) {
	if (active_codec == AudioCodecType::ES8388 && es8388) {
		ES8388_t::I2SLength len = (bits_per_sample == 24) ? ES8388_t::I2SLength::Bit24 : ES8388_t::I2SLength::Bit16;
		es8388->SetI2S(ES8388_t::I2SFormat::Standard, len);
	} else if (active_codec == AudioCodecType::WM8978 && wm8978) {
		WM8978_t::I2SLength len = (bits_per_sample == 24) ? WM8978_t::I2SLength::Bit24 : WM8978_t::I2SLength::Bit16;
		wm8978->SetI2S(WM8978_t::I2SFormat::Standard, len);
	}
}

// SAI PLL2 时钟分频预设表（HSE = 25MHz，PLL2M = 25 -> VCO输入 1MHz）
// OSR=0: Fs = freq/(256*MCKDIV) → MCKDIV = freq/(256*Fs)
struct SAIPreset {
	uint16 rate_div10;// 采样率 / 10
	uint16 pll2n;
	uint8  pll2p;
};
static const SAIPreset SAI_PSC_TBL[] = {
	{ 800 , 344, 7 },// 8kHz     (ker_ck = 49.143MHz, MCKDIV = 24 -> 7998Hz)
	{ 1102, 429, 2 },// 11.025kHz(ker_ck = 214.5MHz,  MCKDIV = 76 -> 超63上限, 不支持)
	{ 1600, 344, 7 },// 16kHz    (ker_ck = 49.143MHz, MCKDIV = 12 -> 15995Hz)
	{ 2205, 429, 2 },// 22.05kHz (ker_ck = 214.5MHz,  MCKDIV = 38 -> 22049Hz)
	{ 3200, 344, 7 },// 32kHz    (ker_ck = 49.143MHz, MCKDIV = 6  -> 31990Hz)
	{ 4410, 271, 2 },// 44.1kHz  (ker_ck = 135.5MHz,  MCKDIV = 12 -> 44108Hz)
	{ 4800, 344, 7 },// 48kHz    (ker_ck = 49.143MHz, MCKDIV = 4  -> 47986Hz)
	{ 8820, 271, 2 },// 88.2kHz  (ker_ck = 135.5MHz,  MCKDIV = 6  -> 88216Hz)
	{ 9600, 344, 7 },// 96kHz    (ker_ck = 49.143MHz, MCKDIV = 2  -> 95971Hz)
	{ 17640,271, 6 },// 176.4kHz (ker_ck = 45.166MHz, MCKDIV = 1  -> 176430Hz)
	{ 19200,295, 6 },// 192kHz   (ker_ck = 49.166MHz, MCKDIV = 1  -> 192055Hz)
};

// DMA 传输状态与乒乓指示
static volatile uint32 dma_done_count[2] = { 0, 0 };
static volatile uint32 dma_underruns     = 0;
static volatile bool is_playing          = true;

// DMA1 双缓冲传输完成回调（对接 UNI 库 DMA 框架）
// 库按 CT 位分派: XferCpltCallback = M0(saibuf0) 完成, XferM1CpltCallback = M1(saibuf1) 完成
// 直接按实际完成方计数, 不再用奇偶假设(奇偶在丢事件/合并时会与真实顺序脱节导致回填错缓冲→每块播两遍→0.5x)
static void on_dma_xfer_cplt0() {
	dma_done_count[0]++;
}

static void on_dma_xfer_cplt1() {
	dma_done_count[1]++;
}

// 动态配置 SAI PLL2 采样率时钟
static bool sai_set_samplerate(uint32 samplerate) {
	uint16 rate_div10 = (uint16)(samplerate / 10);
	const SAIPreset* match = nullptr;
	for (size_t i = 0; i < sizeof(SAI_PSC_TBL) / sizeof(SAI_PSC_TBL[0]); i++) {
		if (SAI_PSC_TBL[i].rate_div10 == rate_div10) {
			match = &SAI_PSC_TBL[i];
			break;
		}
	}
	if (!match) {
		outsfmt("[SAI] Unsupported sample rate: %u Hz\r\n", (unsigned)samplerate);
		return false;
	}

	// 停用 SAI1 Block A
	SAI1[1].enAble(false);

	// 关闭 PLL2 时钟
	RCC[RCCReg::CR].setof(26, false);
	while (RCC[RCCReg::CR].bitof(27)); // 等待 PLL2RDY 清零

	// 配置 PLL2：HSE(25MHz) / 25 = 1MHz
	RCC[RCCReg::PLLCKSELR].maset(12, 6, 25);// DIVM2 = 25
	RCC[RCCReg::PLL2DIVR] = (match->pll2n - 1U) | (((uint32)match->pll2p - 1U) << 9U);
	RCC[RCCReg::PLLCFGR].maset(6, 2, 0);    // PLL2RGE = 0 (1~2MHz)
	RCC[RCCReg::PLLCFGR].rstof(5);         // PLL2VCOSEL = 0 (WIDE)
	RCC[RCCReg::PLLCFGR].rstof(4);         // PLL2FRACEN = 0
	RCC[RCCReg::PLLCFGR].setof(19);        // DIVP2EN

	// 开启 PLL2 并等待稳定
	RCC[RCCReg::CR].setof(26, true);
	while (!RCC[RCCReg::CR].bitof(27));

	// 路由 PLL2_P 作为 SAI1 时钟源 (RCC_D2CCIP1R.SAI1SEL = 1)
	RCC[RCCReg::D2CCIP1R].maset(0, 3, 1);

	// 计算 MCKDIV: OSR=0 时芯片按 Fs = SAI_CK/(256*MCKDIV) 出帧率（与阿波罗H743 ES8388 官方例程一致）
	// MCKDIV = freq/(256*samplerate)
	uint32 freq = (1000000ULL * match->pll2n) / match->pll2p;
	uint32 tmpval = (freq * 10) / (samplerate * 256);
	uint32 mckdiv = tmpval / 10;
	if ((tmpval % 10) > 8) mckdiv += 1;
	if (mckdiv == 0) mckdiv = 1;

	// 设置 SAI1 MCKDIV (CR1 bits [25:20])，NOMCK (bit 19) 保持 0（使能分频输出 MCLK）
	SAI1[1][SAIReg::CR1].maset(20, 6, mckdiv);
	SAI1[1][SAIReg::CR1].rstof(19);// NOMCK = 0 (Master Clock Generator Enabled)
	SAI1[1][SAIReg::CR1].setof(17);// DMAEN
	SAI1[1][SAIReg::CLRFR] = 0x77; // 清除所有标志

	outsfmt("[SAI] Set Rate=%u Hz, PLL2N=%u, PLL2P=%u, Freq=%u, MCKDIV=%u, CR1=0x%08X\r\n",
		(unsigned)samplerate, (unsigned)match->pll2n, (unsigned)match->pll2p,
		(unsigned)freq, (unsigned)mckdiv, (unsigned)(uint32)SAI1[1][SAIReg::CR1]);
	return true;
}

// 初始化 SAI1 Block A 引脚与寄存器（严格对齐官方例程：64位帧长，32位Slot）
static void sai1a_init(byte bits_per_sample) {
	GPIOE.enClock(true);
	// 引脚复用：PE2 (MCLK), PE4 (FS), PE5 (SCK), PE6 (SD) -> AF6，开启上拉
	GPIOE[2].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh).setPull(true);
	GPIOE[2]._set_alternate(6);
	GPIOE[4].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh).setPull(true);
	GPIOE[4]._set_alternate(6);
	GPIOE[5].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh).setPull(true);
	GPIOE[5]._set_alternate(6);
	GPIOE[6].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh).setPull(true);
	GPIOE[6]._set_alternate(6);

	SAI1.enClock(true);
	SAI1[1].enAble(false);

	uint32 datasize_bits = (bits_per_sample == 24) ? (6 << 5) : (4 << 5); // 24-bit (DS=6) : 16-bit (DS=4)

	// SAI Block A CR1: Master TX, Free Protocol, Asynchronous, Stereo, OUTDRIV, DMAEN
	SAI1[1][SAIReg::CR1] = (0 << 0)    // Master TX
	                     | (0 << 2)    // Free Protocol
	                     | datasize_bits
	                     | (0 << 9)    // Clock Strobing Falling Edge (Master TX outputs on falling edge, Codec samples on rising edge)
	                     | (0 << 10)   // Asynchronous
	                     | (0 << 12)   // Stereo mode (MONO = 0)
	                     | (1 << 13)   // OUTDRIV enable
	                     | (1 << 17)   // DMAEN
	                     | (0 << 19);  // NOMCK = 0 (Master Clock Divider Enabled)

	SAI1[1][SAIReg::CR2] = (1 << 0);   // FIFO Threshold = 1/4 FIFO

	// 帧配置：FrameLength=64 (FRL=63), ActiveFrameLength=32 (FSALL=31), FSDEF=1 (SOF+Channel ID), FSPOL=0 (Active Low), FSOFF=1 (Before First Bit)
	SAI1[1][SAIReg::FRCR] = (63 << 0) | (31 << 8) | (1 << 16) | (0 << 17) | (1 << 18);

	// Slot 配置：FBOFF=0, SLOTSZ=2 (32-bit slot), NBSLOT=2 (NBSLOT=1 in reg), SLOTACTIVE=Slot0 | Slot1 (0x0003)
	SAI1[1][SAIReg::SLOTR] = (0 << 0) | (2 << 6) | (1 << 8) | (3 << 16);

	SAI1[1][SAIReg::CLRFR] = 0x77;
}

// 初始化 DMA1 Stream5 循环双缓冲 (Request 87 = SAI1_A)
static void dma_audio_init(byte* buf0, byte* buf1, uint32 num_items, byte width) {
	DMA1.enClock();
	*(volatile uint32*)0x580244D8 |= (1 << 28);// 使能 DMAMUX1 时钟 (RCC_AHB1ENR.DMAMUX1EN)
	*(volatile uint32*)(0x40020800 + 5 * 4) = 87U;// DMAMUX1 Channel 5 = Request 87 (SAI1_A)

	// 注册双缓冲传输完成回调
	DMA1.XferCpltCallback = on_dma_xfer_cplt0;
	DMA1.XferM1CpltCallback = on_dma_xfer_cplt1;

	// 关闭 DMA1 Stream5
	*(volatile uint32*)0x40020088 = 0;
	while (*(volatile uint32*)0x40020088 & 1);

	// 清除 Stream5 标志位
	*(volatile uint32*)0x4002000C = (0x3F << 6);// HIFCR 清零
	SAI1[1][SAIReg::CLRFR] = 0x77;

	*(volatile uint32*)0x40020090 = 0x40015820;// SAI1_Block_A->DR (0x40015804 + 0x1C)
	*(volatile uint32*)0x40020094 = (uint32)buf0;
	*(volatile uint32*)0x40020098 = (uint32)buf1;
	*(volatile uint32*)0x4002008C = num_items;

	// DIR=M2P (01), CIRC=1, MINC=1, DBM=1, TCIE=1, TEIE=1, DMEIE=1, PL=High(10)
	uint32 size_bits = (width == 2) ? ((2 << 11) | (2 << 13)) : ((1 << 11) | (1 << 13));
	*(volatile uint32*)0x40020088 = (1 << 6) | (1 << 8) | (1 << 10) | size_bits | (2 << 16) | (1 << 18) | (1 << 4) | (1 << 2) | (1 << 1);

	DMA1[5].setInterruptPriority(0, 0);
	DMA1[5].enInterruptNVIC(true);

	outsfmt("[DMA] Init Stream5: PAR=0x%08X, MUX5=0x%08X, Items=%u, Width=%u, CR=0x%08X\r\n",
		(unsigned)*(volatile uint32*)0x40020090, (unsigned)*(volatile uint32*)(0x40020800 + 5 * 4),
		(unsigned)num_items, (unsigned)width, (unsigned)*(volatile uint32*)0x40020088);
}

static void audio_play_start() {
	*(volatile uint32*)0x40020088 |= 1;// DMA1_Stream5->CR EN=1
	SAI1[1][SAIReg::CLRFR] = 0x77;
	SAI1[1].enAble(true);
}

static void audio_play_stop() {
	SAI1[1].enAble(false);
	*(volatile uint32*)0x40020088 &= ~1;// DMA1_Stream5->CR EN=0
	while (*(volatile uint32*)0x40020088 & 1);
	SAI1[1][SAIReg::CLRFR] = 0x77;
}

// 打印音频链路全套核心寄存器状态
static void audio_dump_diagnostic() {
	outsfmt("\r\n--- AUDIO DIAGNOSTIC DUMP ---\r\n");
	outsfmt("RCC_D2CCIP1R = 0x%08X (SAI1SEL=%u)\r\n",
		(unsigned)*(volatile uint32*)0x58024450, (unsigned)(*(volatile uint32*)0x58024450 & 0x7));
	outsfmt("RCC_PLL2DIVR  = 0x%08X (N=%u, P=%u)\r\n",
		(unsigned)*(volatile uint32*)0x58024438,
		(unsigned)((*(volatile uint32*)0x58024438 & 0x1FF) + 1),
		(unsigned)(((*(volatile uint32*)0x58024438 >> 9) & 0x7F) + 1));
	outsfmt("SAI1_CR1      = 0x%08X (MCKDIV=%u, DS=%u, OUTDRIV=%u, DMAEN=%u)\r\n",
		(unsigned)(uint32)SAI1[1][SAIReg::CR1],
		(unsigned)(((uint32)SAI1[1][SAIReg::CR1] >> 20) & 0x3F),
		(unsigned)(((uint32)SAI1[1][SAIReg::CR1] >> 5) & 0x7),
		(unsigned)(((uint32)SAI1[1][SAIReg::CR1] >> 13) & 1),
		(unsigned)(((uint32)SAI1[1][SAIReg::CR1] >> 17) & 1));
	outsfmt("SAI1_FRCR     = 0x%08X (FRL=%u, FSALL=%u)\r\n",
		(unsigned)(uint32)SAI1[1][SAIReg::FRCR],
		(unsigned)((uint32)SAI1[1][SAIReg::FRCR] & 0xFF),
		(unsigned)(((uint32)SAI1[1][SAIReg::FRCR] >> 8) & 0x7F));
	outsfmt("SAI1_SLOTR    = 0x%08X (SLOTSZ=%u, NBSLOT=%u, ACTIVE=0x%04X)\r\n",
		(unsigned)(uint32)SAI1[1][SAIReg::SLOTR],
		(unsigned)(((uint32)SAI1[1][SAIReg::SLOTR] >> 6) & 0x3),
		(unsigned)(((uint32)SAI1[1][SAIReg::SLOTR] >> 8) & 0xF),
		(unsigned)(((uint32)SAI1[1][SAIReg::SLOTR] >> 16) & 0xFFFF));
	outsfmt("SAI1_SR       = 0x%08X (FLVL=%u, OVRUDR=%u)\r\n",
		(unsigned)(uint32)SAI1[1][SAIReg::SR],
		(unsigned)(((uint32)SAI1[1][SAIReg::SR] >> 16) & 0x7),
		(unsigned)((uint32)SAI1[1][SAIReg::SR] & 1));
	outsfmt("DMA1_S5_CR    = 0x%08X, NDTR = %u\r\n",
		(unsigned)*(volatile uint32*)0x40020088, (unsigned)*(volatile uint32*)0x4002008C);
	outsfmt("DMAMUX1_CCR5  = 0x%08X (REQ=%u)\r\n",
		(unsigned)*(volatile uint32*)(0x40020800 + 5 * 4),
		(unsigned)(*(volatile uint32*)(0x40020800 + 5 * 4) & 0xFF));
	if (active_codec == AudioCodecType::ES8388 && es8388 && audio_iic) {
		outsfmt("ES8388 I2C_Err=0x%02X, R0=0x%02X R1=0x%02X R2=0x%02X R4=0x%02X R23=0x%02X R39=0x%02X R40=0x%02X R41=0x%02X R42=0x%02X R46=0x%02X R47=0x%02X R48=0x%02X R49=0x%02X\r\n",
			(unsigned)audio_iic->getError(),
			(unsigned)es8388->Read(0), (unsigned)es8388->Read(1),
			(unsigned)es8388->Read(2), (unsigned)es8388->Read(4),
			(unsigned)es8388->Read(23),
			(unsigned)es8388->Read(39), (unsigned)es8388->Read(40),
			(unsigned)es8388->Read(41), (unsigned)es8388->Read(42),
			(unsigned)es8388->Read(46), (unsigned)es8388->Read(47),
			(unsigned)es8388->Read(48), (unsigned)es8388->Read(49));
	} else if (active_codec == AudioCodecType::WM8978 && wm8978 && audio_iic) {
		outsfmt("WM8978 I2C_Err=0x%02X, R1=0x%03X R2=0x%03X R3=0x%03X R4=0x%03X R10=0x%03X R43=0x%03X R49=0x%03X R50=0x%03X R54=0x%03X R55=0x%03X\r\n",
			(unsigned)audio_iic->getError(),
			(unsigned)wm8978->ReadReg(1), (unsigned)wm8978->ReadReg(2),
			(unsigned)wm8978->ReadReg(3), (unsigned)wm8978->ReadReg(4),
			(unsigned)wm8978->ReadReg(10), (unsigned)wm8978->ReadReg(43),
			(unsigned)wm8978->ReadReg(49), (unsigned)wm8978->ReadReg(50),
			(unsigned)wm8978->ReadReg(54), (unsigned)wm8978->ReadReg(55));
	}
	outsfmt("-----------------------------\r\n\r\n");
}

// 1kHz standard sine wave power-on test
static void audio_test_beep() {
	outsfmt("[SelfTest] Outputting 1kHz Test Beep (44.1kHz 16-bit Stereo)...\r\n");
	static const int16 SINE_TABLE[44] = {
		0, 4672, 9216, 13500, 17400, 20800, 23600, 25700, 27000, 27400, 27000,
		25700, 23600, 20800, 17400, 13500, 9216, 4672, 0, -4672, -9216, -13500,
		-17400, -20800, -23600, -25700, -27000, -27400, -27000, -25700, -23600,
		-20800, -17400, -13500, -9216, -4672, 0
	};
	int16* dst0 = (int16*)saibuf0;
	int16* dst1 = (int16*)saibuf1;
	for (uint32 i = 0; i < (AUDIO_DMA_BUF_SIZE / 4); i++) {
		int16 s = SINE_TABLE[i % 44];
		dst0[2 * i + 0] = s;
		dst0[2 * i + 1] = s;
		dst1[2 * i + 0] = s;
		dst1[2 * i + 1] = s;
	}

	codec_set_i2s(16);
	sai1a_init(16);
	sai_set_samplerate(44100);

	dma_audio_init(saibuf0, saibuf1, AUDIO_DMA_BUF_SIZE / 2, 1);
	audio_play_start();
	audio_dump_diagnostic();

	SysDelay_ms(1000);// Play 1 second sine wave
	audio_play_stop();
	outsfmt("[SelfTest] Beep test ended.\r\n");
}

// WAV 文件格式解析结构体
struct WAVCtrl {
	uint16 audio_format;
	uint16 num_channels;
	uint32 sample_rate;
	uint32 byte_rate;
	uint16 block_align;
	uint16 bits_per_sample;
	uint32 data_size;
	uint32 data_start;
	uint32 total_sec;
	uint32 cur_sec;
};

// 解析 WAV 文件头
static bool wav_parse_header(FilesysFAT& fs, void* file, WAVCtrl& wav) {
	byte hdr[512];
	stduint br = fs.readfl(file, Slice{ 0, 512 }, hdr);
	if (br < 44) return false;

	// 检查 "RIFF" 与 "WAVE"
	if (*(uint32*)(hdr + 0) != 0x46464952 || *(uint32*)(hdr + 8) != 0x45564157) return false;

	uint32 offset = 12;
	bool found_fmt = false, found_data = false;

	while (offset + 8 <= br) {
		uint32 chunk_id = *(uint32*)(hdr + offset);
		uint32 chunk_sz = *(uint32*)(hdr + offset + 4);

		if (chunk_id == 0x20746D66) {// "fmt "
			wav.audio_format    = *(uint16*)(hdr + offset + 8);
			wav.num_channels    = *(uint16*)(hdr + offset + 10);
			wav.sample_rate     = *(uint32*)(hdr + offset + 12);
			wav.byte_rate       = *(uint32*)(hdr + offset + 16);
			wav.block_align     = *(uint16*)(hdr + offset + 20);
			wav.bits_per_sample = *(uint16*)(hdr + offset + 22);
			found_fmt = true;
			offset += 8 + chunk_sz;
		} else if (chunk_id == 0x61746164) {// "data"
			wav.data_size  = chunk_sz;
			wav.data_start = offset + 8;
			found_data = true;
			break;
		} else {
			offset += 8 + chunk_sz;
		}
	}

	if (!found_fmt || !found_data) return false;
	wav.total_sec = wav.byte_rate ? (wav.data_size / wav.byte_rate) : 0;
	wav.cur_sec = 0;
	return true;
}

// 向音频缓冲区填充数据（支持 8-bit、16-bit 与 24-bit PCM 单声道/立体声扩展）
// 向音频缓冲区填充数据（支持 8-bit、16-bit 与 24-bit PCM 单声道/立体声扩展）
static uint32 wav_fill_buffer(FilesysFAT& fs, void* file, stduint file_offset, byte* target_buf, uint32 size, uint16 bits, uint16 channels) {
	stduint bread = 0;
	if (bits == 8) {
		// 8-bit 无符号 PCM (0..255, 128为零点)，输出 16-bit 有符号立体声 (-32768..32767)
		if (channels == 1) {
			uint32 samples_needed = size / 4; // 目标缓冲区容纳的立体声采样点数
			bread = fs.readfl(file, Slice{ file_offset, samples_needed }, audio_temp_buf);
			int16* dst = (int16*)target_buf;
			for (uint32 i = 0; i < bread; i++) {
				int32 sample = ((int32)audio_temp_buf[i] - 128) * 256;
				int16 s = (int16)sample;
				dst[2 * i + 0] = s;
				dst[2 * i + 1] = s;
			}
			for (uint32 i = bread; i < samples_needed; i++) {
				dst[2 * i + 0] = 0;
				dst[2 * i + 1] = 0;
			}
			return bread; // 消费的文件字节数
		} else {
			uint32 samples_needed = size / 4;
			uint32 bytes_to_read = samples_needed * 2;
			bread = fs.readfl(file, Slice{ file_offset, bytes_to_read }, audio_temp_buf);
			uint32 frames = bread / 2;
			int16* dst = (int16*)target_buf;
			for (uint32 i = 0; i < frames; i++) {
				int32 l = ((int32)audio_temp_buf[2 * i + 0] - 128) * 256;
				int32 r = ((int32)audio_temp_buf[2 * i + 1] - 128) * 256;
				dst[2 * i + 0] = (int16)l;
				dst[2 * i + 1] = (int16)r;
			}
			for (uint32 i = frames; i < samples_needed; i++) {
				dst[2 * i + 0] = 0;
				dst[2 * i + 1] = 0;
			}
			return frames * 2; // 消费的文件字节数
		}
	} else if (bits == 24) {
		if (channels == 1) {
			uint32 samples_needed = size / 8; // 每个立体声采样占 8 字节 (两个 32-bit slot)
			bread = fs.readfl(file, Slice{ file_offset, samples_needed * 3 }, audio_temp_buf);
			uint32 count = bread / 3;
			uint32* dst = (uint32*)target_buf;
			for (uint32 i = 0; i < count; i++) {
				byte* src = audio_temp_buf + i * 3;
				uint32 val = ((uint32)src[0]) | ((uint32)src[1] << 8) | ((uint32)src[2] << 16);
				dst[2 * i + 0] = val;
				dst[2 * i + 1] = val;
			}
			for (uint32 i = count; i < samples_needed; i++) {
				dst[2 * i + 0] = 0;
				dst[2 * i + 1] = 0;
			}
			return count * 3;
		} else {
			uint32 samples_needed = size / 8;
			uint32 bytes_to_read = samples_needed * 6;
			bread = fs.readfl(file, Slice{ file_offset, bytes_to_read }, audio_temp_buf);
			uint32 count = bread / 3;
			uint32* dst = (uint32*)target_buf;
			for (uint32 i = 0; i < count; i++) {
				byte* src = audio_temp_buf + i * 3;
				dst[i] = ((uint32)src[0]) | ((uint32)src[1] << 8) | ((uint32)src[2] << 16);
			}
			for (uint32 i = count; i < size / 4; i++) dst[i] = 0;
			return count * 3;
		}
	} else {
		// 16-bit PCM (常用标准 WAV)
		if (channels == 1) {
			uint32 samples_needed = size / 4; // 每个单声道样本扩展为立体声 2 个 16-bit halfword (4 字节)
			bread = fs.readfl(file, Slice{ file_offset, samples_needed * 2 }, audio_temp_buf);
			uint32 count = bread / 2;
			int16* src = (int16*)audio_temp_buf;
			int16* dst = (int16*)target_buf;
			for (uint32 i = 0; i < count; i++) {
				int16 s = src[i];
				dst[2 * i + 0] = s;
				dst[2 * i + 1] = s;
			}
			for (uint32 i = count; i < samples_needed; i++) {
				dst[2 * i + 0] = 0;
				dst[2 * i + 1] = 0;
			}
			return count * 2;
		} else {
			// 16-bit 立体声：直接读取，无需重排
			bread = fs.readfl(file, Slice{ file_offset, size }, target_buf);
			if (bread < size) {
				for (uint32 i = bread; i < size; i++) target_buf[i] = 0;
			}
			return bread;
		}
	}
}

// LCD 文本绘制辅助（16x8 点阵）
#define LCD_FB_STRIDE 800
#define LCD_FB_LINES  480
#define LCD_FB ((uint16*)FMC_SDRAM_BANK1_BASE)

static void lcd_draw_char(stduint x, stduint y, char ch, uint16 color, uint16 bg) {
	if (ch < 32 || ch > 126) ch = '?';
	ch -= 0x20;
	const uint16* datptr = (const uint16*)&_BITFONT_ASCII_16x8[(byte)ch];
	for (byte col = 0; col < 8; col++) {
		uint16 dat = datptr[col];
		for (byte row = 0; row < 16; row++) {
			stduint px = x + col, py = y + (row ^ 0b111);
			if (px < LCD_FB_STRIDE && py < LCD_FB_LINES) {
				LCD_FB[py * LCD_FB_STRIDE + px] = (dat & _IMM1S(row)) ? color : bg;
			}
		}
	}
}

static void lcd_draw_string(stduint x, stduint y, const char* str, uint16 color = 0xFFFF, uint16 bg = 0x0000) {
	while (*str) {
		lcd_draw_char(x, y, *str++, color, bg);
		x += 8;
	}
}

static void lcd_fill_rect(stduint x, stduint y, stduint w, stduint h, uint16 color) {
	for (stduint r = y; r < y + h && r < LCD_FB_LINES; r++) {
		for (stduint c = x; c < x + w && c < LCD_FB_STRIDE; c++) {
			LCD_FB[r * LCD_FB_STRIDE + c] = color;
		}
	}
}

// 刷新主界面与状态
static void lcd_update_ui(const char* song_name, uint32 cur_idx, uint32 total_songs, const WAVCtrl& wav, bool playing) {
	char line[64];
	// 标题栏
	lcd_draw_string(20, 20, "========================================", 0x07E0, 0x0000);
	lcd_draw_string(20, 40, "   Alice.Wiki 92-AudioPlay              ", 0xFFE0, 0x0000);
	lcd_draw_string(20, 60, "========================================", 0x07E0, 0x0000);

	// 曲目序号与歌名
	outsfmtbuf(line, "Track : [%02d / %02d]", (int)(cur_idx + 1), (int)total_songs);
	lcd_draw_string(20, 100, line, 0xFFFF, 0x0000);
	outsfmtbuf(line, "Title : %-30s", song_name);
	lcd_draw_string(20, 130, line, 0x07FF, 0x0000);

	// 播放时间与状态
	outsfmtbuf(line, "Time  : %02d:%02d / %02d:%02d    Status: %s",
		(int)(wav.cur_sec / 60), (int)(wav.cur_sec % 60),
		(int)(wav.total_sec / 60), (int)(wav.total_sec % 60),
		playing ? "PLAYING" : "PAUSED ");
	lcd_draw_string(20, 160, line, 0xFFFF, 0x0000);

	// 采样格式与码率
	outsfmtbuf(line, "Format: %u Hz, %d-bit, %s, %u Kbps",
		(unsigned)wav.sample_rate, (int)wav.bits_per_sample,
		(wav.num_channels == 1) ? "Mono" : "Stereo",
		(unsigned)(wav.byte_rate * 8 / 1000));
	lcd_draw_string(20, 190, line, 0xFD20, 0x0000);

	// 按键提示
	lcd_draw_string(20, 250, "----------------------------------------", 0x8410, 0x0000);
	lcd_draw_string(20, 270, "KEY0: NEXT   |  KEY2: PREV   |  WK_UP: PAUSE", 0x07E0, 0x0000);
	lcd_draw_string(20, 290, "----------------------------------------", 0x8410, 0x0000);
}

// 播放列表管理
#define MAX_SONGS 64
static const char* song_paths[MAX_SONGS];
static stduint song_count = 0;

// 文件枚举回调：FAT 按 (is_dir, name) 两参调用
static void on_wav_seg(void* is_dir, ...) {
	if (is_dir) return;
	va_list ap;
	va_start(ap, is_dir);
	const char* nm = va_arg(ap, const char*);
	va_end(ap);
	if (!nm) return;
	stduint len = StrLength(nm);
	if (len < 4) return;
	const char* ext = nm + len - 4;
	if (StrCompareInsensitive(ext, ".wav") != 0) return;
	if (song_count >= MAX_SONGS) return;

	char* full = (char*)mempool.allocate(len + 2);
	full[0] = '/';
	MemCopyN(full + 1, nm, len);
	full[len + 1] = '\0';
	song_paths[song_count++] = full;
}

// 扫描 SD 卡根目录下的所有 .wav 文件
static stduint collect_wav_files(FilesysFAT& fs) {
	song_count = 0;
	FAT_FileHandle h;
	FilesysSearchArgs sargs{};
	sargs.handle_buffer = &h;
	if (!fs.search("/", &sargs)) return 0;
	fs.enumer(&h, on_wav_seg);
	return song_count;
}

// 错误死机闪烁
void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i = 0; i < 1000000; i++) {}
	}
}

int main() {
	L1C.enAbleICacheAll();// 开启 I-Cache 加速指令执行，保持 D-Cache 关闭以确保 DMA 物理内存一致性
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	GPIOA.enClock(true); GPIOB.enClock(true); GPIOC.enClock(true); GPIOH.enClock(true);
	LEDB.setMode(GPIOMode::OUT) = !false;// DS0
	LEDR.setMode(GPIOMode::OUT) = !false;// DS1
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// WK_UP (PA0)
	KEYL.setMode(GPIOMode::IN_Pull).setPull(true); // KEY2  (PC13)
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true); // KEY1  (PH2)
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true); // KEY0  (PH3)

	XART1.setMode(115200);
	outsfmt("\n========================================\n");
	outsfmt("APOLLO STM32H7 - 92-AudioPlay (WM8978 / ES8388)\n");
	outsfmt("========================================\n");

	// 初始化 SDRAM 并注册 Mempool 内存切片
	sdram_init();
	mempool.enable_auto_expand = false;
	mempool.Append(Slice{ SDRAM_POOL_BASE, SDRAM_POOL_SIZE });
	mempool.Append(Slice{ SRAM12_ADDR + 0x8000, 32U * 1024U });
	mempool.Append(Slice{ SRAM4_ADDR,  32U * 1024U });

	ltdc_init();
	lcd_fill_rect(0, 0, 800, 480, 0x0000);
	lcd_draw_string(20, 20, "System initializing...", 0xFFFF, 0x0000);

	DBGMCU.enDBGStandby(true);         // Domain1 待机调试
	DBGMCU.enDBGStandbyDomain3(true);  // Domain3 待机调试

	// 初始化 SDMMC1 与 FAT32 文件系统（照 91-ImageShow 使用 CachedStorageDevice）
	if (!SDCard1.setMode()) {
		outsfmt("SD Card Init Error!\n");
		lcd_draw_string(20, 80, "SD Card Error! Please insert SD card.", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}
	SDCard1.Block_Size = IMAGE_CARD_BLOCK_SIZE;
	outsfmt("SD Card OK!\n");

	CachedStorageDevice cached_sd(SDCard1);
	FilesysFAT fs(0, cached_sd, sector_buf, fat_buf);
	if (!fs.loadfs()) {
		outsfmt("FAT mount Error: %u!\n", (unsigned)fs.error_number);
		lcd_draw_string(20, 80, "FAT Mount Failed!", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}
	outsfmt("FAT%u Mounted OK!\n", (unsigned)fs.fat_type);

	// 扫描歌曲文件
	collect_wav_files(fs);
	outsfmt("Found %u WAV songs.\n", (unsigned)song_count);
	if (song_count == 0) {
		lcd_draw_string(20, 110, "No WAV files found in root dir!", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}

	audio_temp_buf = (byte*)mempool.allocate(AUDIO_DMA_BUF_SIZE);
	if (!audio_temp_buf) {
		outsfmt("Audio temp buffer allocate failed!\n");
		lcd_draw_string(20, 110, "Audio buffer alloc failed!", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}

	// 初始化 I2C 接口
	audio_iic = new (audio_wire_buf) AudioIIC(GPIOH[5], GPIOH[4]);
	audio_iic->func_delay = iic_delay;

	// 探测 ES8388 (0x10)：尝试读取控制寄存器 0
	es8388 = new (es8388_buf) ES8388_t(*audio_iic);
	byte es_r0 = es8388->Read(0);
	bool es_detected = (audio_iic->getError() == ERR_IIC_NONE);

	if (es_detected) {
		active_codec = AudioCodecType::ES8388;
		// 上电冷态即完成 ES8388 全量初始化（I2C 干净、SAI/PLL2/DMA 尚未启动），只做一次
		bool ok = es8388->Initialize();
		es8388->SetADDA(true, false);
		es8388->SetOutput(true, true);
		es8388->SetHPVol(10);
		es8388->SetSPKVol(10);
		outsfmt("Audio Codec Detected: ES8388 (R0=0x%02X, Init=%s)\r\n",
			(unsigned)es_r0, ok ? "OK" : "FAIL");
	} else {
		// 未检出 ES8388，探测 WM8978 (0x1A)
		wm8978 = new (wm8978_buf) WM8978_t(*audio_iic);
		bool wm_ok = wm8978->Initialize();
		if (wm_ok) {
			active_codec = AudioCodecType::WM8978;
			wm8978->SetADDA(true, false);
			wm8978->SetInput(false, false, false);
			wm8978->SetOutput(true, false);
			wm8978->SetHPVol(35, 35); // 适中耳机音量（官方默认40，最大63）
			wm8978->SetSPKVol(28);    // 适中喇叭音量（官方默认30，最大63）
			outsfmt("Audio Codec Detected: WM8978 (Init=OK)\r\n");
		} else {
			active_codec = AudioCodecType::None;
			outsfmt("Audio Codec Detection FAILED: neither ES8388 nor WM8978 responded!\r\n");
		}
	}

	stduint cur_song_idx = 0;
	WAVCtrl wav{};

	// 播放器主循环
	while (true) {
		const char* current_file = song_paths[cur_song_idx];
		FAT_FileHandle fh;
		FilesysSearchArgs sargs{};
		sargs.handle_buffer = &fh;
		void* file_ptr = fs.search(current_file, &sargs);
		if (!file_ptr || fh.is_dir || !fh.size) {
			outsfmt("Failed to open file: %s\n", current_file);
			cur_song_idx = (cur_song_idx + 1) % song_count;
			continue;
		}

		if (!wav_parse_header(fs, file_ptr, wav)) {
			outsfmt("Unsupported or invalid WAV format: %s\n", current_file);
			cur_song_idx = (cur_song_idx + 1) % song_count;
			continue;
		}

		outsfmt("Playing: %s (Rate=%u, Bits=%u, Ch=%u)\n",
			current_file, (unsigned)wav.sample_rate, (unsigned)wav.bits_per_sample, (unsigned)wav.num_channels);

		// 配置 Codec 与 SAI1（8-bit 音频已扩展为 16-bit 流，故 SAI/Codec 设为 16-bit）
		// ES8388/WM8978 全量初始化只在上电时做一次；此处每首歌只设 I2S 格式 + SAI 时钟，避免热路径软复位死机
		byte target_bits = (wav.bits_per_sample == 24) ? 24 : 16;
		codec_set_i2s(target_bits);
		sai1a_init(target_bits);
		sai_set_samplerate(wav.sample_rate);

		// 预填两个 DMA 缓冲（官方例程架构：完成事件后直接从文件回填两个缓冲，无 stage 预读/插零）
		uint32 dma_items = (target_bits == 24) ? (AUDIO_DMA_BUF_SIZE / 4) : (AUDIO_DMA_BUF_SIZE / 2);
		byte dma_width   = (target_bits == 24) ? 2 : 1;
		stduint current_file_offset = wav.data_start;
		stduint consumed0 = wav_fill_buffer(fs, file_ptr, current_file_offset, saibuf0, AUDIO_DMA_BUF_SIZE, wav.bits_per_sample, wav.num_channels);
		current_file_offset += consumed0;
		stduint consumed1 = wav_fill_buffer(fs, file_ptr, current_file_offset, saibuf1, AUDIO_DMA_BUF_SIZE, wav.bits_per_sample, wav.num_channels);
		current_file_offset += consumed1;
		uint32 dma_file_bytes[2] = { (uint32)consumed0, (uint32)consumed1 };
		bool song_over = false;

		outsfmt("[Audio] Prefilled dma=%u/%u\r\n", (unsigned)consumed0, (unsigned)consumed1);

		// 配置并启动 DMA 双缓冲传输
		dma_audio_init(saibuf0, saibuf1, dma_items, dma_width);
		dma_done_count[0] = 0;
		dma_done_count[1] = 0;
		dma_underruns = 0;
		is_playing = true;
		audio_play_start();

		outsfmt("[Audio] Playback started! (DMA1_CR=0x%08X, SAI1_SR=0x%08X)\r\n",
			(unsigned)*(volatile uint32*)0x40020088, (unsigned)(uint32)SAI1[1][SAIReg::SR]);

		stduint current_data_pos = consumed0 + consumed1;
		stduint played_data_pos = 0;
		stduint last_update_sec = (stduint)-1;
		bool song_switch = false;
		stduint next_song_idx = cur_song_idx;

		lcd_update_ui(current_file, cur_song_idx, song_count, wav, is_playing);

		// 单曲播放主流转
		bool need_refill[2] = { false, false };
		while (!song_switch) {
			// 完成事件 = 一轮 M0+M1（两块都播完）→ 两块都标记待回填（保持 1x 回填节奏）
			if (dma_done_count[0] != 0 || dma_done_count[1] != 0) {
				DMA1[5].enInterruptNVIC(false);
				dma_done_count[0] = 0;
				dma_done_count[1] = 0;
				DMA1[5].enInterruptNVIC(true);
				played_data_pos += dma_file_bytes[0] + dma_file_bytes[1];
				if (played_data_pos > wav.data_size) played_data_pos = wav.data_size;
				if (wav.byte_rate > 0) wav.cur_sec = played_data_pos / wav.byte_rate;
				need_refill[0] = true;
				need_refill[1] = true;
			}
			// 安全回填: 只在 DMA 当前读“另一块”时写这块(CT bit19: 0=DMA读M0, 1=DMA读M1)，
			// 每块都在它下次被播出的前半个周期内写入, 既无写读争用也不重播
			bool dma_on1 = (*(volatile uint32*)0x40020088 >> 19) & 1;
			for (byte sb = 0; sb < 2; sb++) {
				bool dma_reading_this = (sb == 0) ? !dma_on1 : dma_on1;
				if (need_refill[sb] && !dma_reading_this) {
					byte* target_buf = (sb == 0) ? saibuf0 : saibuf1;
					if (is_playing && !song_over) {
						uint32 c = wav_fill_buffer(fs, file_ptr, current_file_offset, target_buf, AUDIO_DMA_BUF_SIZE, wav.bits_per_sample, wav.num_channels);
						current_file_offset += c;
						current_data_pos += c;
						dma_file_bytes[sb] = (uint32)c;
						if (c == 0) song_over = true;
					} else {
						for (uint32 i = 0; i < AUDIO_DMA_BUF_SIZE; i++) target_buf[i] = 0;
						dma_file_bytes[sb] = 0;
					}
					need_refill[sb] = false;
				}
			}

			if (song_over && played_data_pos >= wav.data_size) {
				song_switch = true;
				next_song_idx = (cur_song_idx + 1) % song_count;
				break;
			}

			// 按键交互检测
			if (KEYR == 0) {// KEY0: 下一曲
				SysDelay_ms(20);
				if (KEYR == 0) {
					while (KEYR == 0);
					song_switch = true;
					next_song_idx = (cur_song_idx + 1) % song_count;
					break;
				}
			}
			if (KEYL == 0) {// KEY2: 上一曲
				SysDelay_ms(20);
				if (KEYL == 0) {
					while (KEYL == 0);
					song_switch = true;
					next_song_idx = (cur_song_idx == 0) ? (song_count - 1) : (cur_song_idx - 1);
					break;
				}
			}
			if (KEYU == 1) {// WK_UP: 暂停/继续
				SysDelay_ms(20);
				if (KEYU == 1) {
					while (KEYU == 1);
					is_playing = !is_playing;
				}
			}

			// 仅在秒数改变或状态切换时刷新界面，避免频繁刷屏霸占 SDRAM 总线导致音频卡顿断续
			if (wav.cur_sec != last_update_sec) {
				last_update_sec = wav.cur_sec;
				LEDB.Toggle();
				lcd_update_ui(current_file, cur_song_idx, song_count, wav, is_playing);
			}
		}

		audio_play_stop();
		cur_song_idx = next_song_idx;
	}
}
