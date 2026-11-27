// UTF-8 CPP-ISO11 TAB4 CRLF
/*
 * 94-Microphone：ES8388 麦克风录音，保存为 SD 卡上的 PCM WAV。
 * KEYR 开始/停止录音，KEYL 停止保存，KEYD 在停止状态下切换模拟输入1/2。
 * 录音文件：/RECORD/RECxxxxx.TMP，回填 WAV 头并确认保存后改为 .WAV。
 *
 * 麦克风
 *   |
 *   v
 * ES8388：输入选择 -> PGA -> ADC -> I2S 输出
 *                                   | PE3 / SAI1_SD_B
 *                                   v
 *                            SAI1B 同步从接收
 *                                   |
 *                            DMA2 Stream6
 *                                   |
 *                            循环 DMA 暂存区
 *                                   | 完成中断复制
 *                                   v
 *                            AudioBuffer 队列
 *                                   |
 *                     主循环：统计 -> WAV 编码器 -> FileBlockBridge -> FAT -> SD 卡 -> 归还
 *
 * SAI1A 主发送 <- DMA1 Stream5 循环送零
 *   |
 *   +-- 提供 MCLK、SCK、FS，供 ES8388 和 SAI1B 使用
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include <cpp/Device/DBG>
#include <cpp/Device/SysTick>
#include <cpp/Device/IIC>
#include <cpp/System/Audiosys.hpp>
#include <cpp/Device/Mcphone/SAI-RX.hpp>
#include <c/data.h>
#include <c/format/filesys/FAT.h>
#include <c/API-POSIX/fcntl.h>
#include <c/mempool.h>
#include <c/prochip/CortexM7.h>
#include <c/consio.h>
#include <c/ustring.h>
#include "../../../device/audio/wm8978.hpp"
#include "../../../device/audio/es8388.hpp"
#include "../_opendev/RGB-LCD.hpp"
#include "../_opendev/SDRAM.hpp"
#include <new>

extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};
using namespace uni;
extern uni::Mempool mempool;// 由 _opendev/_MEMMAN.cpp 定义。

GPIN& LEDB = GPIOB[0];
GPIN& LEDR = GPIOB[1];
GPIN& KEYU = GPIOA[0];
GPIN& KEYL = GPIOC[13];
GPIN& KEYD = GPIOH[2];
GPIN& KEYR = GPIOH[3];

void outtxt(const char* str, stduint len) { XART1.out(str, len); }

// SDRAM 分区：LCD 位于 +0x100000 以下，FAT 缓冲位于 +0x100000，内存池从 +0x110000 开始。
constexpr stduint card_block_size = 512;
constexpr stduint sdram_pool_base = SDRAM_BANK1_BASE + 0x110000;
constexpr stduint sdram_pool_size = 8U * 1024U * 1024U;
constexpr stduint sram12_base = 0x30000000;
// SRAM1/2 的前 0x8000 字节留给音频 DMA，前 256 字节用于 A 块循环送零。
static byte* const sector_buf = (byte*)(SDRAM_BANK1_BASE + 0x100000);
static byte* const fat_buf = sector_buf + card_block_size;
static_assert(800U * 480U * 2U <= 0x100000U, "LCD overlaps FAT buffers");
static_assert(0x100000U + card_block_size * 2 <= 0x110000U, "FAT overlaps heap");

// 参考 91-ImageShow：把 SDRAM 低 16 MiB 配成 Normal 非缓存，允许普通非对齐读写并保持 DMA 一致性。
static void ConfigureSDRAMMemory() {
	static_assert(sdram_pool_base + sdram_pool_size <= SDRAM_BANK1_BASE + 16U * 1024U * 1024U,
		"SDRAM heap exceeds MPU region");
	Reference(0xE000ED94).rstof(0);
	__DSB();
	Reference(0xE000ED98) = 0;
	Reference(0xE000ED9C) = SDRAM_BANK1_BASE;
	// ENABLE、SIZE=23（16 MiB）、TEX=1、AP=3；C/B=0，Normal 非缓存、全权限。
	Reference(0xE000EDA0) = (1U << 0) | (23U << 1) | (1U << 19) | (3U << 24);
	Reference(0xE000ED94) = (1U << 0) | (1U << 2);
	__DSB();
	__ISB();
}
static FilesysFAT filesystem(0, SDCard1, sector_buf, fat_buf);

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

enum class AudioCodecType { None, WM8978, ES8388 };
static AudioCodecType detected_codec = AudioCodecType::None;
alignas(AudioIIC) static byte audio_wire_buf[sizeof(AudioIIC)];
alignas(WM8978_t) static byte wm8978_buf[sizeof(WM8978_t)];
alignas(ES8388_t) static byte es8388_buf[sizeof(ES8388_t)];
static AudioIIC* audio_iic = nullptr;
static WM8978_t* wm8978 = nullptr;
static ES8388_t* es8388 = nullptr;

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


static void ShowStage(stduint y, const char* stage, const char* status, bool ok) {
	char line[80];
	outsfmtbuf(line, "%s: %s", stage, status);
	outsfmt("[Init] %s\r\n", line);
	lcd_fill_rect(20, y, 760, 16, 0);
	lcd_draw_string(20, y, line, ok ? 0x07E0 : 0xF800);
}

static const char* CodecName() {
	switch (detected_codec) {
	case AudioCodecType::ES8388: return "ES8388";
	case AudioCodecType::WM8978: return "WM8978";
	default: return "None";
	}
}

static bool InitializeCodec() {
	audio_iic = new (audio_wire_buf) AudioIIC(GPIOH[5], GPIOH[4]);
	audio_iic->func_delay = iic_delay;
	es8388 = new (es8388_buf) ES8388_t(*audio_iic);
	byte identity = es8388->Read(0);
	if (audio_iic->getError() == ERR_IIC_NONE) {
		detected_codec = AudioCodecType::ES8388;
		bool ok = es8388->Initialize();
		outsfmt("[Codec] ES8388 R0=0x%02X Init=%s\r\n", (unsigned)identity, ok ? "OK" : "FAIL");
		// 音频芯片初始化后关闭输出通道及 ADC、DAC。
		es8388->SetADDA(false, false);
		ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
		es8388->SetOutput(false, false);
		ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
		es8388->SetHPVol(0);
		ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
		es8388->SetSPKVol(0);
		return (audio_iic->getError() == ERR_IIC_NONE) && ok;
	}
	wm8978 = new (wm8978_buf) WM8978_t(*audio_iic);
	if (!wm8978->WriteReg(0, 0)) return false;
	detected_codec = AudioCodecType::WM8978;
	bool initialized = wm8978->Initialize();
	bool ok = initialized;
	wm8978->SetHPVol(0, 0);
	ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
	wm8978->SetSPKVol(0);
	ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
	wm8978->SetInput(false, false, false);
	ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
	wm8978->SetADDA(false, false);
	ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
	wm8978->SetOutput(false, false);
	ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
	outsfmt("[Codec] WM8978 Init=%s Quiet=%s\r\n", initialized ? "OK" : "FAIL", ok ? "OK" : "FAIL");
	return ok;
}

constexpr uint32 capture_rate = 44100;
constexpr uint32 capture_block_bytes = 4096;
constexpr uint32 capture_block_count = 64;
constexpr uint32 silence_bytes = 256;
static byte* const silence_buffer = (byte*)sram12_base;
static_assert(silence_bytes <= 0x8000, "Audio DMA exceeds reserved SRAM");

enum class CaptureState { Idle, Running, Stopping, Fault };
static CaptureState capture_state = CaptureState::Idle;
static bool capture_ready = false, capture_fault = false;
static bool hardware_owned = false, input_owned = false, receiver_owned = false, sender_owned = false;
static byte capture_input = 0;
alignas(32) static AudioBuffer capture_buffers[capture_block_count];
alignas(SAIRXCapture) static byte capture_object[sizeof(SAIRXCapture)];
static SAIRXCapture* capture = nullptr;
static uint32 capture_blocks = 0, window_frames = 0, window_bytes = 0;
static uint32 report_tick = 0, last_block_tick = 0, stop_warning_tick = 0;

struct SampleStatistics {
	int32 minimum = 32767, maximum = -32768;
	int64 sum = 0;
	uint32 clipped = 0;
};
static SampleStatistics sample_statistics[2];

static void ShowCaptureStatus(const char* text, bool ok) {
	char line[80];
	outsfmtbuf(line, "Capture: %s", text);
	outsfmt("[Capture] %s\r\n", text);
	lcd_fill_rect(20, 220, 760, 16, 0);
	lcd_draw_string(20, 220, line, ok ? 0x07E0 : 0xF800);
	LEDR = !ok;
}

static void ShowCaptureInput() {
	char line[80];
	outsfmtbuf(line, "Input: LIN%u/RIN%u (verify microphone wiring)",
		(unsigned)capture_input + 1, (unsigned)capture_input + 1);
	lcd_fill_rect(20, 280, 760, 16, 0);
	lcd_draw_string(20, 280, line, 0x07FF);
}

static void ResetSampleWindow() {
	window_frames = window_bytes = 0;
	for (auto& statistics : sample_statistics) statistics = SampleStatistics{};
}

struct RecordingSession {
	FAT_FileHandle* handle = nullptr;
	FileBlockBridge* output = nullptr;
	IAudioEncoder* encoder = nullptr;
	char temporary_path[32] = {};
	char final_path[32] = {};
	bool failed = false, accepting = false;
	uint32 data_bytes = 0, maximum_write_ms = 0;
};
static RecordingSession recording;
static FAT_FileHandle recording_directory{};
alignas(FileBlockBridge) static byte recording_storage[sizeof(FileBlockBridge)];
static bool recording_card_ready = false, recording_card_bound = false;
static IOMethod saved_card_method = IOMethod::Loop;
static Handler_t saved_card_error_handler = nullptr;
static Atomic<bool> recording_card_error{false};
static uint32 next_recording_index = 0;

static void RecordingCardError() {
	recording_card_error.store(true, MemoryOrder_Release);
	if (saved_card_error_handler) saved_card_error_handler();
}

static bool RecordingStorageHealthy() {
	return !recording_card_error.load(MemoryOrder_Acquire) &&
		filesystem.getFaultState() == FilesysFaultState::Healthy;
}

static void ShowRecordingPath(const char* path, bool ok) {
	lcd_fill_rect(20, 400, 760, 16, 0);
	lcd_draw_string(20, 400, path, ok ? 0x07FF : 0xF800);
}

static void ReleaseRecording() {
	// 编码器必须先于文件适配对象及文件句柄释放，句柄析构不执行刷盘。
	if (recording.encoder) recording.encoder->Release();
	recording.encoder = nullptr;
	if (recording.output) recording.output->~FileBlockBridge();
	recording.output = nullptr;
	delete recording.handle;
	recording.handle = nullptr;
	recording.accepting = false;
	if (recording_card_bound) {
		SDCard1.ErrorHandler = saved_card_error_handler;
		SDCard1.storage_method = saved_card_method;
		recording_card_bound = false;
	}
}

static void ReportRecordingDirectory(const char* stage, bool ok, uint32 begin) {
	outsfmt("[RecordDir] %s ok=%u ms=%u FAT=%u fault=%u SDerr=%u ctx=%u\r\n", stage,
		(unsigned)ok, (unsigned)((uint32)SysTick::getTick() - begin),
		(unsigned)filesystem.error_number, (unsigned)filesystem.getFaultState(),
		(unsigned)recording_card_error.load(MemoryOrder_Acquire), (unsigned)SDCard1.Context);
	outsfmt("[RecordDir] dir=%u cluster=%u size=%u STA=%08X MASK=%08X IDMA=%08X BASE=%08X\r\n",
		(unsigned)recording_directory.is_dir, (unsigned)recording_directory.start_cluster,
		(unsigned)recording_directory.size, (unsigned)(uint32)SDCard1[SDReg::STA],
		(unsigned)(uint32)SDCard1[SDReg::MASK], (unsigned)(uint32)SDCard1[SDReg::IDMACTRL],
		(unsigned)(uint32)SDCard1[SDReg::IDMABASER]);
}

static void ProbeRecordingReads(uint32 block = 0) {
	// 仅在目录准备失败后，对同一扇区比较轮询与两次 DMA 读取；不写卡，也不打印扇区内容。
	byte* buffers = (byte*)mempool.allocate(card_block_size * 3, 5);
	if (!buffers) { outsfmt("[RecordProbe] No memory\r\n"); return; }
	// 清除上一笔失败传输的残留状态，轮询读取不能在 IDMA 仍使能时开始。
	SDCard1[SDReg::IDMACTRL] = 0;
	SDCard1[SDReg::MASK] = 0;
	SDCard1[SDReg::DCTRL] = 0;
	SDCard1[SDReg::CMD].rstof(6);
	SDCard1[SDReg::ICR] = 0x1FE00FFF;
	MemSet(buffers, 0xA5, card_block_size);
	MemSet(buffers + card_block_size, 0x5A, card_block_size);
	MemSet(buffers + card_block_size * 2, 0x3C, card_block_size);
	uint32 loop_error = 0, dma_error = 0, repeat_error = 0;
	uint32 begin = (uint32)SysTick::getTick();
	bool loop_ok = SDCard1.Read(buffers, block, 1, IOMethod::Loop, 1000, &loop_error);
	uint32 loop_ms = (uint32)SysTick::getTick() - begin;
	begin = (uint32)SysTick::getTick();
	bool dma_ok = SDCard1.ReadDMA_Blocking(buffers + card_block_size, block, 1, 1000, &dma_error);
	uint32 dma_ms = (uint32)SysTick::getTick() - begin;
	bool repeat_ok = SDCard1.ReadDMA_Blocking(buffers + card_block_size * 2, block, 1, 1000, &repeat_error);
	// 命令启动失败也可能留下 IDMA 使能位，释放探测内存前明确关闭。
	SDCard1[SDReg::IDMACTRL] = 0;
	SDCard1[SDReg::MASK] = 0;
	uint32 mismatch = 0, first = card_block_size, last = card_block_size, dma_mismatch = 0;
	uint32 loop_changed = 0, dma_changed = 0, repeat_changed = 0;
	// 不同的预填值用于识别“返回成功但没有写入目标内存”；只输出统计，不输出扇区内容。
	volatile const byte* observed = buffers;
	for (uint32 index = 0; index < card_block_size; ++index) {
		if (observed[index] != 0xA5) ++loop_changed;
		if (observed[card_block_size + index] != 0x5A) ++dma_changed;
		if (observed[card_block_size * 2 + index] != 0x3C) ++repeat_changed;
		if (observed[index] != observed[card_block_size + index]) {
			if (!mismatch) first = index;
			last = index;
			++mismatch;
		}
		if (observed[card_block_size + index] != observed[card_block_size * 2 + index]) ++dma_mismatch;
	}
	outsfmt("[RecordProbe] LBA=%u Loop=%u/%ums err=%08X DMA=%u/%ums err=%08X\r\n",
		(unsigned)block, (unsigned)loop_ok, (unsigned)loop_ms, (unsigned)loop_error,
		(unsigned)dma_ok, (unsigned)dma_ms, (unsigned)dma_error);
	outsfmt("[RecordProbe] Loop/DMA diff=%u first=%u last=%u; DMA2=%u err=%08X diff=%u\r\n",
		(unsigned)mismatch, (unsigned)first, (unsigned)last,
		(unsigned)repeat_ok, (unsigned)repeat_error, (unsigned)dma_mismatch);
	outsfmt("[RecordProbe] Memory=%08X changed: Loop=%u DMA1=%u DMA2=%u /512\r\n",
		(unsigned)(stduint)buffers, (unsigned)loop_changed, (unsigned)dma_changed, (unsigned)repeat_changed);
	if (!block) {
		outsfmt("[RecordProbe] Boot signature valid: Loop=%u DMA1=%u DMA2=%u\r\n",
			(unsigned)(observed[510] == 0x55 && observed[511] == 0xAA),
			(unsigned)(observed[card_block_size + 510] == 0x55 && observed[card_block_size + 511] == 0xAA),
			(unsigned)(observed[card_block_size * 2 + 510] == 0x55 && observed[card_block_size * 2 + 511] == 0xAA));
	}
	mempool.deallocate(buffers, card_block_size * 3);
}

static bool PrepareRecordingDirectory() {
	FilesysSearchArgs args{};
	args.handle_buffer = &recording_directory;
	uint32 begin = (uint32)SysTick::getTick();
	bool found = filesystem.search("/RECORD", &args) != nullptr;
	ReportRecordingDirectory("lookup /RECORD", found, begin);
	if (!found) {
		if (!RecordingStorageHealthy()) return false;
		FilesysCreateParameter parameter;
		begin = (uint32)SysTick::getTick();
		bool created = filesystem.create("/RECORD", O_DIRECTORY, parameter);
		ReportRecordingDirectory("mkdir /RECORD", created, begin);
		if (!created) return false;
		uint32 directory_sector = ((FAT_FileHandle*)parameter.handler)->dir_sector;
		delete (FAT_FileHandle*)parameter.handler;
		if (!RecordingStorageHealthy()) return false;
		begin = (uint32)SysTick::getTick();
		found = filesystem.search("/RECORD", &args) != nullptr;
		ReportRecordingDirectory("relookup /RECORD", found, begin);
		if (!found) { ProbeRecordingReads(directory_sector); return false; }
	}
	if (!recording_directory.is_dir) outsfmt("[RecordDir] /RECORD exists but is not a directory\r\n");
	return recording_directory.is_dir && RecordingStorageHealthy();
}

static bool CreateRecordingFile() {
	FAT_FileHandle existing{};
	FilesysSearchArgs args{};
	args.handle_buffer = &existing;
	while (next_recording_index < 100000) {
		uint32 digits = next_recording_index++;
		StrCopy(recording.temporary_path, "/RECORD/REC00000.TMP");
		StrCopy(recording.final_path, "/RECORD/REC00000.WAV");
		for (byte index = 0; index < 5; ++index) {
			char digit = (char)('0' + digits % 10);
			recording.temporary_path[15 - index] = digit;
			recording.final_path[15 - index] = digit;
			digits /= 10;
		}
		bool temporary_exists = filesystem.search(recording.temporary_path, &args) != nullptr;
		if (!RecordingStorageHealthy()) return false;
		if (temporary_exists) continue;
		bool final_exists = filesystem.search(recording.final_path, &args) != nullptr;
		if (!RecordingStorageHealthy()) return false;
		if (final_exists) continue;
		FilesysCreateParameter parameter;
		parameter.handler = &recording_directory;
		if (!filesystem.create(recording.temporary_path, 0, parameter)) return false;
		recording.handle = (FAT_FileHandle*)parameter.handler;
		return recording.handle && RecordingStorageHealthy();
	}
	return false;
}

static bool BeginRecording() {
	if (!recording_card_ready || filesystem.getFaultState() != FilesysFaultState::Healthy ||
		recording.handle || recording.output || recording.encoder) return false;
	recording = RecordingSession{};
	recording_card_error.store(false, MemoryOrder_Release);
	saved_card_method = SDCard1.storage_method;
	saved_card_error_handler = SDCard1.ErrorHandler;
	recording_card_bound = true;
	SDCard1.ErrorHandler = RecordingCardError;
	// SDMMC 内部 DMA 独立于 SAI 的 DMA1/2，主循环等待完成时允许采集中断运行。
	NVIC.setPriority(SDCard1.getRequest(), 2, 1);
	NVIC.setAble(SDCard1.getRequest(), true);
	SDCard1.storage_method = IOMethod::DMA;
	bool directory_ready = PrepareRecordingDirectory();
	if (!directory_ready) ProbeRecordingReads();
	if (!directory_ready || !CreateRecordingFile()) {
		outsfmt("[Record] Create failed; FAT=%u healthy=%u path=%s\r\n",
			(unsigned)filesystem.error_number, (unsigned)RecordingStorageHealthy(), recording.temporary_path);
		ShowRecordingPath("Recording file creation failed", false);
		ReleaseRecording();
		return false;
	}
	recording.output = new (recording_storage) FileBlockBridge(&filesystem, recording.handle, 0, 1, true);
	AudioEncodeOptions options{};
	AudioResult opened = OpenAudioEncoder(AudioContainerFormat::WAV, *recording.output,
		capture_buffers[0].format, options, recording.encoder, mempool);
	if (opened != AudioResult::OK || !RecordingStorageHealthy()) {
		outsfmt("[Record] Encoder open failed (%u); temporary file: %s\r\n",
			(unsigned)opened, recording.temporary_path);
		ShowRecordingPath(recording.temporary_path, false);
		ReleaseRecording();
		return false;
	}
	recording.accepting = true;
	outsfmt("[Record] Open %s; 44100 Hz / S16LE / 2ch; queue=%u bytes\r\n",
		recording.temporary_path, (unsigned)(capture_block_count * capture_block_bytes));
	ShowRecordingPath(recording.temporary_path, true);
	return true;
}

static AudioResult WriteRecordingBlock(const AudioBuffer& buffer) {
	if (!recording.encoder || !recording.accepting) return AudioResult::OK;
	AudioBufferView view{};
	view.data = buffer.data;
	view.byte_count = buffer.byte_count;
	uint32 begin = (uint32)SysTick::getTick();
	AudioResult written = recording.encoder->WriteSamples(view);
	uint32 elapsed = (uint32)SysTick::getTick() - begin;
	if (elapsed > recording.maximum_write_ms) recording.maximum_write_ms = elapsed;
	bool healthy = RecordingStorageHealthy();
	if (written != AudioResult::OK || !healthy) {
		// 长度上限可以结束并保存此前完整的数据；短写和介质错误不可重试。
		recording.accepting = false;
		recording.failed = written != AudioResult::Unsupported || !healthy;
		outsfmt("[Record] Write stopped (%u); FAT=%u healthy=%u bytes=%u\r\n",
			(unsigned)written, (unsigned)filesystem.error_number, (unsigned)healthy,
			(unsigned)recording.data_bytes);
		return written == AudioResult::OK ? AudioResult::IoError : written;
	}
	recording.data_bytes += buffer.byte_count;
	return AudioResult::OK;
}

static void FinishRecording() {
	if (!recording.handle) return;
	recording.accepting = false;
	bool ok = recording.encoder && !recording.failed && RecordingStorageHealthy();
	if (ok) {
		AudioResult finished = recording.encoder->Finish();
		ok = finished == AudioResult::OK && RecordingStorageHealthy() &&
			recording.output->getFileSize() == stduint(44) + recording.data_bytes &&
			recording.handle->size == stduint(44) + recording.data_bytes;
		if (!ok) outsfmt("[Record] WAV finish failed (%u)\r\n", (unsigned)finished);
	}
	// 采集启动失败且没有样本时保留临时文件，不把空会话标记为成功。
	if (capture_fault && !recording.data_bytes) ok = false;
	if (ok) {
		FilesysRenameParameter parameter;
		parameter.handler = recording.handle;
		ok = filesystem.rename(recording.temporary_path, recording.final_path, parameter) &&
			RecordingStorageHealthy();
	}
	if (ok) {
		outsfmt("[Record] %s %s; PCM=%u bytes frames=%u max_write=%u ms\r\n",
			capture_fault ? "Saved partial" : "Saved", recording.final_path,
			(unsigned)recording.data_bytes, (unsigned)(recording.data_bytes / 4),
			(unsigned)recording.maximum_write_ms);
		ShowRecordingPath(recording.final_path, !capture_fault);
	} else {
		capture_fault = true;
		outsfmt("[Record] Save not confirmed; TMP=%s WAV=%s FAT=%u\r\n",
			recording.temporary_path, recording.final_path, (unsigned)filesystem.error_number);
		ShowRecordingPath(recording.temporary_path, false);
	}
	ReleaseRecording();
}

static bool PrepareCaptureMemory() {
	if (capture) return true;
	for (auto& buffer : capture_buffers) {
		buffer.format.sample_format = AudioSampleFormat::S16LE;
		buffer.format.channels = 2;
		buffer.format.sample_rate = capture_rate;
		buffer.capacity = capture_block_bytes;
		buffer.data = mempool.allocate(capture_block_bytes, 5);
		if (!buffer.data) {
			for (auto& allocated : capture_buffers) {
				if (allocated.data) mempool.deallocate(allocated.data, capture_block_bytes);
				allocated.data = nullptr;
			}
			return false;
		}
	}
	capture = new (capture_object) SAIRXCapture(SAI1[2], DMA2[6], mempool);
	return true;
}

static bool ControllerAvailable(DMA_t& dma) {
	if (dma.bind) return false;
	for (byte index = 0; index < 8; ++index) {
		if (dma[DMAReg::CR[index]].bitof(_DMA_SxCR_POS_EN) ||
			dma.streamStates[index] == _DMA_STATE_BUSY ||
			dma.streamStates[index] == _DMA_STATE_ABORT) return false;
	}
	return true;
}

static bool ConfigureCaptureBlock(SAI_Block& block, bool receiver) {
	block.setAudioMode(receiver ? SAIAudioMode_E::SlaveRx : SAIAudioMode_E::MasterTx)
		.setProtocol(SAIProtocol_E::Free)
		.setDataSize(SAIDataSize_E::Data16)
		.setFirstBit(SAIFirstBit_E::MSB)
		.setClockStrobing(SAIClockStrobing_E::RisingEdge)
		.setSynchro(receiver ? SAISynchro_E::Synchronous : SAISynchro_E::Asynchronous)
		.setSynchroExt(SAISynchroExt_E::Disable)
		.setOutputDrive(SAIOutputDrive_E::Enable)
		.setNoDivider(SAINoDivider_E::Divider)
		.setFifoThreshold(SAIFifoThreshold_E::Quarter)
		.setMonoStereo(SAIMonoStereo_E::Stereo)
		.setCompanding(SAICompanding_E::None)
		.setPdmActivation(false)
		.setMckOverSampling(SAIMckOverSampling_E::Disable)
		.setFrameLength(64).setActiveFrameLength(32)
		.setFSDefinition(SAIFSDefinition_E::ChannelIdentification)
		.setFSPolarity(SAIFSPolarity_E::ActiveLow)
		.setFSOffset(SAIFSOffset_E::BeforeFirstBit)
		.setFirstBitOffset(0).setSlotSize(SAISlotSize_E::B32)
		.setSlotNumber(2).setSlotActive(3);
	// 沿用 92-AudioPlay 已验证的显式分频，避免自动计算覆盖 MCKDIV。
	block.setAudioFreq(0).setMckDiv(12);
	if (!block.canMode()) return false;
	block[SAIReg::IMR] = 0;
	block[SAIReg::CLRFR] = 0x77;
	block[SAIReg::CR2].setof(SAI_xCR2_FFLUSH_Pos);
	return true;
}

static bool ConfigureCaptureHardware() {
	DMA1.enClock(true);
	DMA2.enClock(true);
	if (!ControllerAvailable(DMA1) || !ControllerAvailable(DMA2) ||
		DMA1.streamCallbacks[5].HasHandler() || DMA2.streamCallbacks[6].HasHandler()) return false;
	if ((SAI1[1].getState() != SAIXState::Reset && SAI1[1].getState() != SAIXState::Ready) ||
		(SAI1[2].getState() != SAIXState::Reset && SAI1[2].getState() != SAIXState::Ready)) return false;
	SAI1.enClock(true);
	hardware_owned = true;
	SAI1[1].enAble(false);
	SAI1[2].enAble(false);
	// 本板 HSE=25MHz，PLL2_P=135.5MHz；PLL3 继续供 LCD 使用。
	if (!RCC.PLL2.setMode(PLLSource::HSE, 0, 0, PLLPara_t{25, 271, 2, 2, 2, 0}) ||
		!RCC.setPeriphClock(PeriphClock::SAI1, ClockSource::PLL2)) return false;
	GPIOE.enClock(true);
	for (byte pin = 2; pin <= 6; ++pin) {
		GPIOE[pin].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh).setPull(true);
		GPIOE[pin]._set_alternate(6);
	}
	if (!ConfigureCaptureBlock(SAI1[1], false) || !ConfigureCaptureBlock(SAI1[2], true)) return false;
	if (!DMA1[5][0].setMode(false, true, false, true, true,
			DMADataWidth::HalfWord, DMADataWidth::HalfWord, 2) ||
		!DMA2[6][0].setMode(true, false, false, true, true,
			DMADataWidth::HalfWord, DMADataWidth::HalfWord, 3) ||
		!SAI1[1].setDMA(&DMA1[5])) return false;
	DMA1[5].setInterruptPriority(2, 0);
	DMA2[6].setInterruptPriority(1, 0);
	NVIC.setPriority(SAI1.getRequest(), 1, 0);
	NVIC.setAble(SAI1.getRequest(), true);
	MemSet(silence_buffer, 0, silence_bytes);
	L1C.CleanDCacheRange(silence_buffer, silence_bytes);
	outsfmt("[Clock] SAI1=%u MCKDIV=12 FRCR=%08X SLOTR=%08X\r\n",
		(unsigned)RCC.getPeriphClockFreq(PeriphClock::SAI1),
		(unsigned)(uint32)SAI1[2][SAIReg::FRCR], (unsigned)(uint32)SAI1[2][SAIReg::SLOTR]);
	return SAI1[2].isPCMReceive(capture_rate);
}

static bool ConfigureCaptureInput() {
	input_owned = true;
	if (!es8388->SetADCEnabled(false) || !es8388->SetInput(capture_input) ||
		!es8388->SetADCFormat(ES8388_t::I2SFormat::Standard, ES8388_t::I2SLength::Bit16))
		return false;
	es8388->SetOutput(false, false);
	if (audio_iic->getError() != ERR_IIC_NONE) return false;
	es8388->SetMicGain(8);
	if (audio_iic->getError() != ERR_IIC_NONE) return false;
	// ES8388 数据手册 6.1.9、6.2：从模式、256Fs、取消静音、0dB 数字衰减、关闭 ALC/噪声门。
	static const byte settings[][2] = {
		{0x08, 0x00}, {0x0D, 0x02}, {0x0F, 0x20},
		{0x10, 0x00}, {0x11, 0x00}, {0x12, 0x38}, {0x16, 0x00}
	};
	for (const auto& setting : settings)
		if (!es8388->Send(setting[0], setting[1])) return false;
	es8388->SetADDA(false, true);
	if (audio_iic->getError() != ERR_IIC_NONE || !es8388->SetMicBias(true) ||
		!es8388->SetADCEnabled(true)) return false;
	byte select = es8388->Read(0x0A);
	if (audio_iic->getError() != ERR_IIC_NONE) return false;
	byte format = es8388->Read(0x0C);
	if (audio_iic->getError() != ERR_IIC_NONE) return false;
	byte power = es8388->Read(0x03);
	if (audio_iic->getError() != ERR_IIC_NONE) return false;
	outsfmt("[ADC] Input=%u R0A=%02X R0C=%02X R03=%02X Gain=24dB\r\n",
		(unsigned)capture_input + 1, (unsigned)select, (unsigned)format, (unsigned)power);
	return (select & 0xF0) == (capture_input ? 0x50 : 0x00) && format == 0x0C && !(power & 0xFC);
}

static void AnalyzeSamples(const AudioBuffer& buffer) {
	const int16* samples = (const int16*)buffer.data;
	uint32 frames = buffer.byte_count / 4;
	for (uint32 frame = 0; frame < frames; ++frame) {
		for (byte channel = 0; channel < 2; ++channel) {
			int32 sample = samples[frame * 2 + channel];
			auto& statistics = sample_statistics[channel];
			if (sample < statistics.minimum) statistics.minimum = sample;
			if (sample > statistics.maximum) statistics.maximum = sample;
			statistics.sum += sample;
			if (sample == -32768 || sample == 32767) ++statistics.clipped;
		}
	}
	++capture_blocks;
	window_frames += frames;
	window_bytes += buffer.byte_count;
	last_block_tick = (uint32)SysTick::getTick();
}

static AudioResult ConsumeCaptureBuffers() {
	AudioResult outcome = AudioResult::OK;
	bool draining = capture_state == CaptureState::Stopping;
	uint32 limit = draining ? capture_block_count : 4;
	for (uint32 index = 0; index < limit; ++index) {
		const AudioBuffer* buffer = nullptr;
		AudioResult acquired = capture->AcquireBuffer(buffer);
		if (acquired != AudioResult::OK) return outcome == AudioResult::OK ? acquired : outcome;
		bool valid = buffer->byte_count && buffer->byte_count <= buffer->capacity && !(buffer->byte_count & 3U);
		AudioResult written = AudioResult::InvalidFormat;
		if (valid) {
			AnalyzeSamples(*buffer);
			written = WriteRecordingBlock(*buffer);
		} else recording.accepting = false;
		// 写入完成或失败后都归还借出的块，停止排空时继续归还其余完整块。
		AudioResult returned = capture->ReturnBuffer(*buffer);
		if (returned != AudioResult::OK) return returned;
		if (written != AudioResult::OK) {
			outcome = written;
			if (!draining) return written;
		}
	}
	return outcome;
}

static void RequestCaptureStop(const char* reason, bool failed) {
	capture_fault = capture_fault || failed;
	if (capture_state == CaptureState::Stopping) return;
	capture_state = CaptureState::Stopping;
	outsfmt("[Capture] %s; RX=%08X TX=%08X DMA2=%08X DMA1=%08X\r\n", reason,
		(unsigned)SAI1[2].getError(), (unsigned)SAI1[1].getError(),
		(unsigned)DMA2[6].getError(), (unsigned)DMA1[5].getError());
	ShowCaptureStatus("Stopping", !capture_fault);
	stop_warning_tick = (uint32)SysTick::getTick();
}

static void FinishCaptureStop(uint32 now) {
	// RX 停止成功前保留 A 块时钟，资源始终留到停止确认后再复用。
	if (receiver_owned) {
		AudioResult stopped = capture->Stop();
		if (stopped != AudioResult::OK) {
			if (uint32(now - stop_warning_tick) >= 1000) {
				stop_warning_tick = now;
				outsfmt("[Capture] RX stop pending (%u)\r\n", (unsigned)stopped);
			}
			return;
		}
		AudioResult drained = ConsumeCaptureBuffers();
		if (drained != AudioResult::OK && drained != AudioResult::EndOfStream &&
			drained != AudioResult::WouldBlock) capture_fault = true;
		receiver_owned = false;
	}
	if (sender_owned) {
		if (!SAI1[1].StopDMA()) {
			if (uint32(now - stop_warning_tick) >= 1000) {
				stop_warning_tick = now;
				outsfmt("[Capture] TX stop pending\r\n");
			}
			return;
		}
		sender_owned = false;
	}
	if (hardware_owned) {
		SAI1[2][SAIReg::IMR] = 0;
		SAI1[1][SAIReg::IMR] = 0;
		SAI1[2].enAble(false);
		SAI1[1].enAble(false);
		NVIC.setAble(SAI1.getRequest(), false);
		hardware_owned = false;
	}
	if (input_owned) {
		bool ok = es8388->SetADCEnabled(false);
		ok = es8388->SetMicBias(false) && ok;
		es8388->SetADDA(false, false);
		ok = (audio_iic->getError() == ERR_IIC_NONE) && ok;
		if (!ok) {
			capture_fault = true;
			if (uint32(now - stop_warning_tick) >= 1000) {
				stop_warning_tick = now;
				outsfmt("[Capture] ADC power-down pending\r\n");
			}
			return;
		}
		input_owned = false;
	}
	FinishRecording();
	capture_state = capture_fault ? CaptureState::Fault : CaptureState::Idle;
	ShowCaptureStatus(capture_fault ? "Fault - KEYR retries" : "Saved - KEYR records", !capture_fault);
}

static void StartCapture() {
	if (!capture_ready || capture_state == CaptureState::Running || capture_state == CaptureState::Stopping) return;
	capture_fault = false;
	if (!PrepareCaptureMemory()) {
		capture_state = CaptureState::Fault;
		ShowCaptureStatus("Buffer allocation failed", false);
		return;
	}
	capture_blocks = 0;
	ResetSampleWindow();
	if (!BeginRecording()) {
		capture_state = CaptureState::Fault;
		ShowCaptureStatus("Cannot create WAV - check SD/FAT", false);
		return;
	}
	if (!ConfigureCaptureHardware() || !ConfigureCaptureInput()) {
		RequestCaptureStop("Configuration failed", true);
		return;
	}
	receiver_owned = true;
	AudioResult started = capture->Start(AudioCaptureMode::Continuous, capture_buffers, capture_block_count);
	if (started != AudioResult::OK) {
		outsfmt("[Capture] RX start failed (%u)\r\n", (unsigned)started);
		RequestCaptureStop("RX start failed", true);
		return;
	}
	// 此时 A 块尚未提供时钟，先核验 DMA 的外设源地址，再启动数据传输。
	outsfmt("[RXDMA] PAR=%08X M0AR=%08X NDTR=%u CR=%08X FCR=%08X\r\n",
		(unsigned)(uint32)DMA2[DMAReg::PAR[6]], (unsigned)(uint32)DMA2[DMAReg::M0AR[6]],
		(unsigned)(uint32)DMA2[DMAReg::NDTR[6]], (unsigned)(uint32)DMA2[DMAReg::CR[6]],
		(unsigned)(uint32)DMA2[DMAReg::FCR[6]]);
	if ((uint32)DMA2[DMAReg::PAR[6]] != SAI1[2].blockBase() + (stduint)SAIReg::DR) {
		RequestCaptureStop("RX DMA peripheral address mismatch", true);
		return;
	}
	// DMA 发送入口会先使能 A 块；预填 FIFO，覆盖 DMA 启动前的短暂间隙。
	for (byte index = 0; index < 8; ++index) SAI1[1][SAIReg::DR] = 0;
	sender_owned = true;
	if (SAI1[1].Transmit(silence_buffer, silence_bytes / 2, IOMethod::DMA) != silence_bytes / 2) {
		RequestCaptureStop("Clock sender start failed", true);
		return;
	}
	DMA1[5].enInterrupt(false, 1);
	DMA1[5].enInterrupt(false, 2);
	report_tick = last_block_tick = (uint32)SysTick::getTick();
	capture_state = CaptureState::Running;
	ShowCaptureStatus("Recording - 44100 Hz / S16LE / 2ch", true);
}

static void ReportCapture(uint32 now) {
	uint32 elapsed = now - report_tick;
	if (elapsed < 1000) return;
	char line[80];
	uint32 byte_rate = (uint32)((uint64)window_bytes * 1000 / elapsed);
	outsfmt("[PCM] ms=%u blocks=%u Bps=%u expected=%u frames=%u\r\n", (unsigned)elapsed,
		(unsigned)capture_blocks, (unsigned)byte_rate, (unsigned)(capture_rate * 4), (unsigned)window_frames);
	outsfmtbuf(line, "Blocks=%u  B/s=%u  Expected=%u", (unsigned)capture_blocks,
		(unsigned)byte_rate, (unsigned)(capture_rate * 4));
	lcd_fill_rect(20, 310, 760, 16, 0);
	lcd_draw_string(20, 310, line, 0xFFFF);
	for (byte channel = 0; channel < 2; ++channel) {
		const auto& statistics = sample_statistics[channel];
		int32 minimum = window_frames ? statistics.minimum : 0;
		int32 maximum = window_frames ? statistics.maximum : 0;
		int32 mean = window_frames ? (int32)(statistics.sum / window_frames) : 0;
		outsfmtbuf(line, "%c: min=%d max=%d mean=%d clip=%u", channel ? 'R' : 'L',
			(int)minimum, (int)maximum, (int)mean, (unsigned)statistics.clipped);
		outsfmt("[PCM] %s\r\n", line);
		lcd_fill_rect(20, 340 + channel * 30, 760, 16, 0);
		lcd_draw_string(20, 340 + channel * 30, line, 0x07FF);
	}
	outsfmt("[Record] PCM=%u bytes max_write=%u ms\r\n", (unsigned)recording.data_bytes,
		(unsigned)recording.maximum_write_ms);
	outsfmtbuf(line, "Written=%u KiB  Max block write=%u ms", (unsigned)(recording.data_bytes / 1024),
		(unsigned)recording.maximum_write_ms);
	lcd_fill_rect(20, 430, 760, 16, 0);
	lcd_draw_string(20, 430, line, 0xFFFF);
	ResetSampleWindow();
	report_tick = now;
}

static void ServiceCapture(uint32 now) {
	if (capture_state == CaptureState::Stopping) {
		FinishCaptureStop(now);
		return;
	}
	if (capture_state != CaptureState::Running) return;
	AudioResult serviced = capture->ServiceCapture();
	if (serviced != AudioResult::OK && serviced != AudioResult::WouldBlock) {
		outsfmt("[Capture] RX service failed (%u)\r\n", (unsigned)serviced);
		RequestCaptureStop("RX error", true);
		return;
	}
	AudioResult consumed = ConsumeCaptureBuffers();
	if (consumed != AudioResult::OK && consumed != AudioResult::WouldBlock) {
		outsfmt("[Capture] Buffer delivery failed (%u)\r\n", (unsigned)consumed);
		RequestCaptureStop("Buffer error", true);
		return;
	}
	if (SAI1[1].getError() || DMA1[5].getError()) {
		RequestCaptureStop("Clock sender error", true);
		return;
	}
	now = (uint32)SysTick::getTick();
	if (uint32(now - last_block_tick) > 500) {
		outsfmt("[Capture] No PCM; NDTR=%u SR_A=%08X SR_B=%08X\r\n",
			(unsigned)(uint32)DMA2[DMAReg::NDTR[6]],
			(unsigned)(uint32)SAI1[1][SAIReg::SR], (unsigned)(uint32)SAI1[2][SAIReg::SR]);
		RequestCaptureStop("No PCM blocks for 500 ms", true);
		return;
	}
	ReportCapture(now);
}

struct DebouncedKey {
	GPIN& pin;
	const char* name;
	bool active_high;
	bool candidate = false;
	bool stable = false;
	uint32 changed_at = 0;

	DebouncedKey(GPIN& pin, const char* name, bool active_high)
		: pin(pin), name(name), active_high(active_high) {}

	void Initialize(uint32 now) {
		candidate = stable = (bool(pin) == active_high);
		changed_at = now;
	}

	bool Pressed(uint32 now) {
		bool pressed = bool(pin) == active_high;
		if (pressed != candidate) {
			candidate = pressed;
			changed_at = now;
		}
		if (candidate == stable || uint32(now - changed_at) < 20) return false;
		stable = candidate;
		return stable;
	}
};

static DebouncedKey keys[] = {
	{KEYU, "KEYU", true},
	{KEYL, "KEYL", false},
	{KEYD, "KEYD", false},
	{KEYR, "KEYR", false},
};

// 系统时钟配置失败时，用 LED 持续指示故障。
void erro(const char* str) {
	(void)str;
	GPIOB.enClock(true);
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i = 0; i < 1000000; i++) {}
	}
}

int main() {
	L1C.enAbleICacheAll();// 初期接入 DMA 时保持 D-cache 关闭。
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	GPIOA.enClock(true);
	GPIOB.enClock(true);
	GPIOC.enClock(true);
	GPIOH.enClock(true);
	LEDB.setMode(GPIOMode::OUT) = true;
	LEDR.setMode(GPIOMode::OUT) = true;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true);
	if (!XART1.setMode(115200)) erro();

	outsfmt("\r\n========================================\r\n");
	outsfmt("APOLLO STM32H7 - 94-Microphone (WM8978 / ES8388)\r\n");
	outsfmt("========================================\r\n");

	sdram_init();
	ConfigureSDRAMMemory();
	mempool.enable_auto_expand = false;
	// SDMMC1 的 IDMA 无法访问 D2/D3 SRAM；FAT 事务缓冲会从通用堆分配，因此只加入 SDRAM。
	mempool.Append(Slice{sdram_pool_base, sdram_pool_size});
	outsfmt("[Memory] Heap=%08X size=%u; SDRAM only for SDMMC1 IDMA\r\n",
		(unsigned)sdram_pool_base, (unsigned)sdram_pool_size);

	ltdc_init();
	lcd_fill_rect(0, 0, 800, 480, 0);
	lcd_draw_string(20, 20, "Alice.Wiki 94-Microphone", 0xFFE0);
	lcd_draw_string(20, 50, "ES8388 microphone / SD WAV recorder", 0x07FF);
	ShowStage(90, "SDRAM / LTDC", "Initialized", true);
	DBGMCU.enDBGStandby(true);
	DBGMCU.enDBGStandbyDomain3(true);

	// SD 卡初始化失败后，仍继续探测音频芯片并进入空闲按键循环。
	bool card_ready = SDCard1.setMode();
	ShowStage(120, "SD card", card_ready ? "OK" : "FAIL / check card", card_ready);
	bool fat_ready = false;
	if (card_ready) {
		SDCard1.Block_Size = card_block_size;
		SDCard1.storage_method = IOMethod::Loop;
		fat_ready = filesystem.loadfs();
	}
	char line[80];
	if (fat_ready) outsfmtbuf(line, "FAT%u mounted", (unsigned)filesystem.fat_type);
	else if (card_ready) outsfmtbuf(line, "Mount failed (%u)", (unsigned)filesystem.error_number);
	else StrCopy(line, "Pending SD card");
	ShowStage(150, "Filesystem", line, fat_ready);

	bool codec_ready = InitializeCodec();
	outsfmtbuf(line, "%s / %s", CodecName(), codec_ready ? "Initialized, outputs off" : "FAIL");
	ShowStage(180, "Codec", line, codec_ready);
	recording_card_ready = fat_ready;
	capture_ready = codec_ready && detected_codec == AudioCodecType::ES8388 && fat_ready;
	lcd_draw_string(20, 250, "KEYR: record/stop  KEYL: save  KEYD: input", 0x07FF);
	ShowCaptureInput();
	ShowCaptureStatus(capture_ready ? "Ready - KEYR records" : "Codec or SD/FAT unavailable", capture_ready);
	outsfmt("[Init] WAV recording %s; SD/FAT=%s\r\n", capture_ready ? "ready" : "unavailable",
		fat_ready ? "ready" : "unavailable");

	uint32 heartbeat = (uint32)SysTick::getTick();
	for (auto& key : keys) key.Initialize(heartbeat);
	while (true) {
		uint32 now = (uint32)SysTick::getTick();
		ServiceCapture(now);
		for (auto& key : keys) {
			if (!key.Pressed(now)) continue;
			outsfmt("[Key] %s pressed\r\n", key.name);
			if (&key.pin == &KEYR) {
				if (capture_state == CaptureState::Running) RequestCaptureStop("KEYR stop", false);
				else StartCapture();
			} else if (&key.pin == &KEYL && capture_state == CaptureState::Running) {
				RequestCaptureStop("KEYL stop", false);
			} else if (&key.pin == &KEYD &&
				(capture_state == CaptureState::Idle || capture_state == CaptureState::Fault)) {
				capture_input ^= 1;
				ShowCaptureInput();
				outsfmt("[Capture] Input=%u; selection applies at next start\r\n", (unsigned)capture_input + 1);
			}
		}
		if (uint32(now - heartbeat) >= 500) {
			heartbeat = now;
			LEDB.Toggle();
		}
	}
}
