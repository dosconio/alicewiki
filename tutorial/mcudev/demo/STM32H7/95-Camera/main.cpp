// ASCII CPP-ISO11 TAB4 CRLF
/*
	95-Camera: OV5640 preview, BMP/JPEG photographs and JPEG serial output.
	U: scale/crop; D: save 2592x1944 JPEG; L: focus; R: save BMP.
	USART1: 115200 command console. Send '?' for commands.
	USART2: 921600, PA2 TX / PA3 RX, raw JPEG data for the original camera viewer.
	JPEG stream keys: U JPEG size, D focus, L effect, R contrast.
	Only the main loop starts/stops captures, switches shared pins or accesses FAT.
*/
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/Graphic/DCMI.hpp>
#include <cpp/atomic>
#include <cpp/Device/SD.hpp>
#include <c/format/filesys/FAT.h>
#include <c/format/picture/BMP.h>
#include <c/API-POSIX/fcntl.h>
#include <c/mempool.h>
#include <c/prochip/CortexM7.h>
#include "../_opendev/RGB-LCD.hpp"
#include "OV5640.hpp"

extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};
using namespace uni;
extern uni::Mempool mempool;

GPIN& LEDB = GPIOB[0];
GPIN& LEDR = GPIOB[1];
GPIN& KEYU = GPIOA[0];
GPIN& KEYL = GPIOC[13];
GPIN& KEYD = GPIOH[2];
GPIN& KEYR = GPIOH[3];
void outtxt(const char* str, stduint len) { XART1.out(str, len); }

namespace {
	constexpr stduint lcd_width = 800, lcd_height = 480;
	constexpr stduint image_bytes = lcd_width * lcd_height * 2;
	// RM0433, FMC SDRAM mapping: 16-bit, 13-row, 9-column gives 8 MiB per internal bank.
	constexpr stduint capture_address = SDRAM_BANK1_BASE + 0xB00000;
	constexpr stduint jpeg_address = SDRAM_BANK1_BASE + 0x300000;
	constexpr stduint jpeg_capacity = 8U * 1024U * 1024U;
	constexpr stduint pool_address = SDRAM_BANK1_BASE + 0xC00000;
	constexpr stduint pool_size = 4U * 1024U * 1024U;
	static_assert(SDRAM_BANK1_BASE + image_bytes <= SDRAM_BANK1_BASE + 0x100000, "LCD overlaps FAT buffers");
	static_assert(jpeg_address + jpeg_capacity <= capture_address, "JPEG overlaps capture buffer");
	static_assert(capture_address + image_bytes <= pool_address, "Capture overlaps heap");
	static_assert(jpeg_address + jpeg_capacity <= pool_address, "JPEG overlaps heap");
	static_assert(pool_address + pool_size == SDRAM_BANK1_BASE + 16U * 1024U * 1024U, "MPU region overflow");

	// The entire low 16 MiB is Normal, non-cacheable for DMA, LTDC and SD accesses.
	constexpr stduint mpu_control = 0xE000ED94, mpu_region = 0xE000ED98;
	constexpr stduint mpu_base = 0xE000ED9C, mpu_attributes = 0xE000EDA0;
	constexpr uint32 sdram_attributes = (1U << 0) | (23U << 1) | (1U << 19) | (3U << 24);
	byte* const sector_buffer = (byte*)(SDRAM_BANK1_BASE + 0x100000);
	byte* const fat_buffer = sector_buffer + 512;
	OV5640_t sensor;
	FilesysFAT filesystem(0, SDCard1, sector_buffer, fat_buffer);
	FAT_FileHandle photo_directory{};
	ImageBuffer captured_image{};
	Atomic<bool> capture_error{false};
	Atomic<stduint> fifo_warning_count{0};
	Atomic<stduint> line_events{0}, vsync_events{0}, minimum_dma_remaining{0xFFFFFFFFU};
	Atomic<bool> capture_diagnostics_busy{false};
	struct CaptureDiagnostics {
		uint32 dcmi_error, dcmi_state, dma_error, dma_state;
		uint32 received_bytes;
		uint32 dcmi_control, dcmi_status, dcmi_raw_flags, dcmi_masked_flags, dcmi_interrupts;
		uint32 dma_control, dma_fifo, dma_remaining, dma_flags;
		uint32 peripheral_address, memory0_address, memory1_address;
		uint32 lines, vsyncs, minimum_remaining, mux_control, dma_clocks;
	};
	volatile CaptureDiagnostics capture_diagnostics{};
	bool card_ready = false, camera_pins_active = false, focus_ready = false;
	bool serial_stream = false, scaled_preview = true;
	bool discard_preview = true, discard_jpeg = true;
	enum class CaptureFormat { None, RGB565, JPEG };
	CaptureFormat capture_format = CaptureFormat::None;
	byte jpeg_size_index = 2, contrast = 3, effect = 0, light_mode = 0;
	byte saturation = 3, brightness = 4, test_pattern = 0;
	bool flash_enabled = false;
	uint16 active_jpeg_width = 0, active_jpeg_height = 0;
	const uint16 jpeg_sizes[][2] = {
		{160, 120}, {320, 240}, {640, 480}, {800, 600}, {1024, 768}, {1280, 800},
		{1440, 900}, {1280, 1024}, {1600, 1200}, {1920, 1080}, {2048, 1536}, {2592, 1944},
	};
	char commands[16]{};
	byte command_head = 0, command_tail = 0;

	void QueueCommand(char command) {
		byte next = (byte)((command_head + 1) % sizeof(commands));
		if (next == command_tail) return;
		commands[command_head] = command;
		command_head = next;
	}

	char PopCommand() {
		if (command_tail == command_head) return 0;
		char command = commands[command_tail];
		command_tail = (byte)((command_tail + 1) % sizeof(commands));
		return command;
	}

	void PollControls() {
		// UART.cpp uses ISR.RXNE bit 5 and ICR.ORECF bit 3 on this controller.
		if (XART1[XARTReg::ISR].bitof(5)) {
			int value = XART1.inn();
			if (value > 0 && value != '\r' && value != '\n') QueueCommand((char)value);
		}
		if (XART1[XARTReg::ISR].bitof(3)) XART1[XARTReg::ICR] = _IMM1S(3);
		static byte previous = 0, stable = 0;
		static uint64 changed = 0;
		byte keys = (!bool(KEYR) ? 1 : 0) | (!bool(KEYD) ? 2 : 0) |
			(!bool(KEYL) ? 4 : 0) | (bool(KEYU) ? 8 : 0);
		uint64 now = SysTick::getTick();
		if (keys != previous) { previous = keys; changed = now; }
		if (keys == stable || now - changed < 20) return;
		byte pressed = keys & ~stable;
		stable = keys;
		if (pressed & 1) QueueCommand(serial_stream ? 'c' : 'b');
		if (pressed & 2) QueueCommand(serial_stream ? 'f' : 'j');
		if (pressed & 4) QueueCommand(serial_stream ? 'e' : 'f');
		if (pressed & 8) QueueCommand(serial_stream ? 'r' : 'u');
	}

	uint32 preview_clock_hz = 0;
	struct PreviewTiming {
		uint64 capture_cycles = 0, display_cycles = 0, elapsed_cycles = 0;
		uint32 previous_end = 0, frames = 0;
		bool active = false;
	} preview_timing;

	uint32 ReadPreviewCycles() { return Reference(_DWT_BASE + 4); }

	void InitPreviewTiming() {
		// Cortex-M7 DWT setup follows 93-VideoPlay; CYCCNT continues while IRQs are masked.
		Reference(0xE000EDFCU).setof(24, true);
		Reference(_DWT_BASE + 0xFB0) = 0xC5ACCE55U;
		Reference(_DWT_BASE).setof(0, true);
		uint32 before = ReadPreviewCycles();
		for (volatile unsigned i = 0; i < 1000; ++i) {}
		if (ReadPreviewCycles() != before) preview_clock_hz = RCC.Sysclock.getCoreFrequency();
	}

	void ReportPreviewTiming(uint32 begin, uint32 captured, uint32 displayed) {
		if (!preview_clock_hz) return;
		if (!preview_timing.active) {
			preview_timing.previous_end = begin;
			preview_timing.active = true;
		}
		preview_timing.capture_cycles += (uint32)(captured - begin);
		preview_timing.display_cycles += (uint32)(displayed - captured);
		preview_timing.elapsed_cycles += (uint32)(displayed - preview_timing.previous_end);
		preview_timing.previous_end = displayed;
		++preview_timing.frames;
		if (preview_timing.elapsed_cycles < preview_clock_hz) return;
		uint64 divisor = (uint64)preview_clock_hz * preview_timing.frames;
		uint32 capture_us = (uint32)(preview_timing.capture_cycles * 1000000 / divisor);
		uint32 display_us = (uint32)(preview_timing.display_cycles * 1000000 / divisor);
		uint32 fps10 = (uint32)((uint64)preview_timing.frames * preview_clock_hz * 10 / preview_timing.elapsed_cycles);
		XART1.OutFormat("Preview: capture=%u.%03u ms display=%u.%03u ms FPS=%u.%u frames=%u\r\n",
			(unsigned)(capture_us / 1000), (unsigned)(capture_us % 1000),
			(unsigned)(display_us / 1000), (unsigned)(display_us % 1000),
			(unsigned)(fps10 / 10), (unsigned)(fps10 % 10), (unsigned)preview_timing.frames);
		preview_timing.capture_cycles = preview_timing.display_cycles = preview_timing.elapsed_cycles = 0;
		preview_timing.frames = 0;
	}

	void InitMemoryProtection() {
		Reference control{mpu_control}, region{mpu_region}, base{mpu_base}, attributes{mpu_attributes};
		control.rstof(0);
		__DSB();
		region = 0U;
		base = SDRAM_BANK1_BASE;
		attributes = sdram_attributes;
		control = (1U << 0) | (1U << 2);
		__DSB();
		__ISB();
	}

	void ConfigurePin(GPIN& pin, byte alternate) {
		pin.setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh).setPull(true);
		pin._set_alternate(alternate);
	}

	bool StopCapture() {
		if (DCMI.Stop()) return true;
		XART1.OutFormat("DCMI stop failed: 0x%08X\r\n", (unsigned)DCMI.getError());
		return false;
	}

	bool SelectCamera() {
		if (camera_pins_active) return true;
		if (!StopCapture() || !sensor.enOutput(false)) return false;
		ConfigurePin(GPIOA[6], 13);
		ConfigurePin(GPIOB[7], 13);
		ConfigurePin(GPIOB[8], 13);
		ConfigurePin(GPIOB[9], 13);
		ConfigurePin(GPIOC[6], 13);
		ConfigurePin(GPIOC[7], 13);
		ConfigurePin(GPIOC[8], 13);
		ConfigurePin(GPIOC[9], 13);
		ConfigurePin(GPIOC[11], 13);
		ConfigurePin(GPIOD[3], 13);
		// The original implementation uses PH8 (HSYNC); PH7 is DCMI_D9.
		ConfigurePin(GPIOH[8], 13);
		if (!sensor.enOutput(true)) return false;
		camera_pins_active = true;
		return true;
	}

	bool SelectCard() {
		// PC8/PC9/PC11 are shared. Disable sensor drivers before selecting SD alternate functions.
		if (!StopCapture()) return false;
		camera_pins_active = false;
		if (!sensor.enOutput(false)) return false;
		ConfigurePin(GPIOC[8], 12);
		ConfigurePin(GPIOC[9], 12);
		ConfigurePin(GPIOC[11], 12);
		return true;
	}

	void CaptureLine() {
		line_events.fetch_add(1, MemoryOrder_Relaxed);
		stduint remaining = DMA[1][DMAReg::NDTR[1]];
		if (remaining < minimum_dma_remaining.load(MemoryOrder_Relaxed))
			minimum_dma_remaining.store(remaining, MemoryOrder_Relaxed);
	}

	void CaptureVsync() { vsync_events.fetch_add(1, MemoryOrder_Relaxed); }

	void RecordCaptureError() {
		if (capture_error.load(MemoryOrder_Acquire) ||
			capture_diagnostics_busy.exchange(true, MemoryOrder_Acquire)) return;
		if (capture_error.load(MemoryOrder_Acquire)) {
			capture_diagnostics_busy.store(false, MemoryOrder_Release);
			return;
		}
		// Preserve the first callback state before StopCapture clears flags or releases DMA.
		const DMAStream& stream = DMA[1][1];
		capture_diagnostics.dcmi_error = DCMI.getError();
		capture_diagnostics.received_bytes = DCMI.getReceivedSize();
		capture_diagnostics.dcmi_state = (uint32)DCMI.getState();
		capture_diagnostics.dma_error = stream.getError();
		capture_diagnostics.dma_state = stream.getState();
		capture_diagnostics.dcmi_control = DCMI[DCMIReg::CR];
		capture_diagnostics.dcmi_status = DCMI[DCMIReg::SR];
		capture_diagnostics.dcmi_raw_flags = DCMI[DCMIReg::RISR];
		capture_diagnostics.dcmi_masked_flags = DCMI[DCMIReg::MISR];
		capture_diagnostics.dcmi_interrupts = DCMI[DCMIReg::IER];
		capture_diagnostics.dma_control = DMA[1][DMAReg::CR[1]];
		capture_diagnostics.dma_fifo = DMA[1][DMAReg::FCR[1]];
		capture_diagnostics.dma_remaining = DMA[1][DMAReg::NDTR[1]];
		capture_diagnostics.dma_flags = DMA[1][DMAReg::LISR];
		capture_diagnostics.peripheral_address = DMA[1][DMAReg::PAR[1]];
		capture_diagnostics.memory0_address = DMA[1][DMAReg::M0AR[1]];
		capture_diagnostics.memory1_address = DMA[1][DMAReg::M1AR[1]];
		capture_diagnostics.lines = line_events.load(MemoryOrder_Relaxed);
		capture_diagnostics.vsyncs = vsync_events.load(MemoryOrder_Relaxed);
		capture_diagnostics.minimum_remaining = minimum_dma_remaining.load(MemoryOrder_Relaxed);
		// DMA.cpp maps DMA1 stream 1 to DMAMUX1 channel 1 (base 0x40020800).
		capture_diagnostics.mux_control = Reference(0x40020804U);
		capture_diagnostics.dma_clocks = RCC[RCCReg::AHB1ENR];
		capture_error.store(true, MemoryOrder_Release);
		capture_diagnostics_busy.store(false, MemoryOrder_Release);
	}

	void CaptureError() {
		// The fixed-length DCMI path retains Busy/Ready and no DCMI error for DMA FE alone.
		if (DCMI.getError() == _DCMI_ERROR_NONE && DMA[1][1].getError() == _DMA_ERROR_FE) {
			fifo_warning_count.fetch_add(1, MemoryOrder_Relaxed);
			return;
		}
		RecordCaptureError();
	}

	void PrintCaptureDiagnostics(const char* reason) {
		if (!capture_error.load(MemoryOrder_Acquire)) return;
		// UART output stays in the main loop, outside the DMA/DCMI interrupt handlers.
		XART1.OutFormat("DCMI %s: error=0x%08X state=%u DMA error=0x%08X state=%u\r\n", reason,
			(unsigned)capture_diagnostics.dcmi_error, (unsigned)capture_diagnostics.dcmi_state,
			(unsigned)capture_diagnostics.dma_error, (unsigned)capture_diagnostics.dma_state);
		XART1.OutFormat(" Published/copied bytes=%u\r\n", (unsigned)capture_diagnostics.received_bytes);
		XART1.OutFormat(" DCMI CR=%08X SR=%08X RISR=%08X MISR=%08X IER=%08X\r\n",
			(unsigned)capture_diagnostics.dcmi_control, (unsigned)capture_diagnostics.dcmi_status,
			(unsigned)capture_diagnostics.dcmi_raw_flags, (unsigned)capture_diagnostics.dcmi_masked_flags,
			(unsigned)capture_diagnostics.dcmi_interrupts);
		XART1.OutFormat(" DMA1S1 CR=%08X FCR=%08X NDTR=%u LISR=%08X\r\n",
			(unsigned)capture_diagnostics.dma_control, (unsigned)capture_diagnostics.dma_fifo,
			(unsigned)capture_diagnostics.dma_remaining, (unsigned)capture_diagnostics.dma_flags);
		XART1.OutFormat(" DMA1S1 PAR=%08X M0AR=%08X M1AR=%08X\r\n",
			(unsigned)capture_diagnostics.peripheral_address, (unsigned)capture_diagnostics.memory0_address,
			(unsigned)capture_diagnostics.memory1_address);
		XART1.OutFormat(" Lines=%u VSYNC=%u minNDTR=%u DMAMUX1.C1CR=%08X AHB1ENR=%08X\r\n",
			(unsigned)capture_diagnostics.lines, (unsigned)capture_diagnostics.vsyncs,
			(unsigned)capture_diagnostics.minimum_remaining, (unsigned)capture_diagnostics.mux_control,
			(unsigned)capture_diagnostics.dma_clocks);
	}

	bool ConfigureCapture(bool jpeg) {
		// OV5640 table 6-6: bit 0 selects VSYNC polarity; restore the original example's 0x21.
		byte polarity = 0;
		if (!sensor.WriteRegister(0x4740, 0x21) || !sensor.ReadRegister(0x4740, polarity) || polarity != 0x21) {
			XART1.OutFormat("OV5640 polarity setup failed: expected=21 readback=%02X\r\n", (unsigned)polarity);
			return false;
		}
		DCMIConfig_t config;
		config.pixel_clock_rising = true;
		config.jpeg_mode = jpeg;
		if (!DCMI.setMode(config) || !DCMI.enCrop(false)) return false;
		// AN5020, FIFO and burst transfer configuration: use INC4 for fixed RGB565 frames in SDRAM.
		DMA[1][DMAReg::CR[1]].maset(_DMA_SxCR_POS_MBURST, 2, jpeg ? 0 : 1);
		DMA[1][1][0].setFIFOThreshold(jpeg ? DMAChannel::_1HalfFull : DMAChannel::_Full);
		XART1.OutFormat("Capture DMA: MBURST=%u FTH=%u\r\n",
			(unsigned)DMA[1][DMAReg::CR[1]].masof(_DMA_SxCR_POS_MBURST, 2),
			(unsigned)DMA[1][DMAReg::FCR[1]].masof(_DMA_SxFCR_POS_FTH, 2));
		DCMI.hdmarx = &DMA[1][1];
		DCMI.ErrorHandler = CaptureError;
		DCMI.LineHandler = CaptureLine;
		DCMI.VsyncHandler = CaptureVsync;
		DCMI.setInterruptPriority(2, 2);
		DCMI.enInterrupt();
		static bool configuration_reported = false;
		if (!configuration_reported) {
			XART1.OutFormat("Preview buffers: LCD=%08X capture=%08X\r\n",
				(unsigned)SDRAM_BANK1_BASE, (unsigned)capture_address);
			XART1.OutFormat("Camera pins PC8 mode=%u AF=%u, PC9 mode=%u AF=%u, PC11 mode=%u AF=%u\r\n",
				(unsigned)GPIOC[GPIOReg::MODER].masof(16, 2), (unsigned)GPIOC[8].getAlternate(),
				(unsigned)GPIOC[GPIOReg::MODER].masof(18, 2), (unsigned)GPIOC[9].getAlternate(),
				(unsigned)GPIOC[GPIOReg::MODER].masof(22, 2), (unsigned)GPIOC[11].getAlternate());
			XART1.OutFormat("IRQ priorities DCMI=%02X DMA1S1=%02X; SD DCTRL=%08X STA=%08X IDMACTRL=%08X\r\n",
				(unsigned)NVIC.Reflect(NVICReg::IP0, _IMM(IRQ_DCMI) >> 2).masof((_IMM(IRQ_DCMI) & 3U) * 8, 8),
				(unsigned)NVIC.Reflect(NVICReg::IP0, _IMM(IRQ_DMA1_Stream1) >> 2).masof((_IMM(IRQ_DMA1_Stream1) & 3U) * 8, 8),
				(unsigned)SDCard1[SDReg::DCTRL], (unsigned)SDCard1[SDReg::STA], (unsigned)SDCard1[SDReg::IDMACTRL]);
			configuration_reported = true;
		}
		return true;
	}

	bool ApplyImageSettings() {
		return sensor.setLightMode(light_mode) && sensor.setSaturation(saturation) &&
			sensor.setBrightness(brightness) && sensor.setContrast(contrast) && sensor.setSharpness(33) &&
			sensor.setEffect(effect) && sensor.setTestPattern(test_pattern) && sensor.enFlash(flash_enabled);
	}

	void PrintSensorConfiguration() {
		const uint16 addresses[] = {0x3008, 0x3017, 0x3018, 0x3035, 0x4300, 0x501F,
			0x3808, 0x3809, 0x380A, 0x380B, 0x4740};
		byte values[sizeof(addresses) / sizeof(addresses[0])];
		for (stduint i = 0; i < sizeof(values); ++i) {
			if (!sensor.ReadRegister(addresses[i], values[i])) {
				XART1.OutFormat("OV5640 readback failed at %04X\r\n", (unsigned)addresses[i]);
				return;
			}
		}
		XART1.OutFormat("OV5640 readback power=%02X outputs=%02X/%02X clock=%02X format=%02X ISP=%02X size=%ux%u polarity=%02X\r\n",
			(unsigned)values[0], (unsigned)values[1], (unsigned)values[2], (unsigned)values[3],
			(unsigned)values[4], (unsigned)values[5], (unsigned)(values[6] * 256U + values[7]),
			(unsigned)(values[8] * 256U + values[9]), (unsigned)values[10]);
	}

	bool PreparePreview() {
		if (!SelectCamera()) return false;
		if (capture_format == CaptureFormat::RGB565) return true;
		capture_format = CaptureFormat::None;
		if (!StopCapture() || !sensor.setRGB565() || !ApplyImageSettings()) return false;
		stduint x = scaled_preview ? 16 : (1280 - lcd_width) / 2;
		stduint y = scaled_preview ? 4 : (800 - lcd_height) / 2;
		if (!sensor.setOutput(Rectangle(Point(x, y), Size2(lcd_width, lcd_height))) || !ConfigureCapture(false)) return false;
		PrintSensorConfiguration();
		capture_format = CaptureFormat::RGB565;
		discard_preview = true;
		return true;
	}

	bool PrepareJPEG(uint16 width, uint16 height) {
		if (!SelectCamera()) return false;
		if (capture_format == CaptureFormat::JPEG && active_jpeg_width == width && active_jpeg_height == height) return true;
		capture_format = CaptureFormat::None;
		if (!StopCapture() || !sensor.setJPEG() || !ApplyImageSettings() ||
			!sensor.setOutput(Rectangle(Point(16, 4), Size2(width, height))) || !ConfigureCapture(true)) return false;
		PrintSensorConfiguration();
		capture_format = CaptureFormat::JPEG;
		active_jpeg_width = width; active_jpeg_height = height;
		discard_jpeg = true;
		return true;
	}

	bool WaitCapture(stduint timeout_ms) {
		static bool dma_buffers_reported = false;
		if (!dma_buffers_reported) {
			XART1.OutFormat("Capture DMA buffers: M0=%08X M1=%08X words=%u\r\n",
				(unsigned)DMA[1][DMAReg::M0AR[1]], (unsigned)DMA[1][DMAReg::M1AR[1]],
				(unsigned)DMA[1][DMAReg::NDTR[1]]);
			dma_buffers_reported = true;
		}
		uint64 begin = SysTick::getTick();
		while (!DCMI.getFrameReady() && !capture_error.load(MemoryOrder_Acquire)) {
			if (SysTick::getTick() - begin >= timeout_ms) {
				RecordCaptureError();
				PrintCaptureDiagnostics("frame timeout");
				StopCapture();
				return false;
			}
			PollControls();
			SysDelay_ms(1);
		}
		bool complete = !capture_error.load(MemoryOrder_Acquire) && DCMI.getFrameReady();
		if (!complete) PrintCaptureDiagnostics("capture failed");
		// Snapshot data stays borrowed; release DMA before displaying or saving it.
		bool stopped = StopCapture();
		static bool frame_completion_reported = false;
		stduint warnings = fifo_warning_count.load(MemoryOrder_Relaxed);
		if (stopped && complete && !frame_completion_reported) {
			XART1.OutFormat("DCMI frame complete: received=%u bytes, DMA FE callbacks=%u\r\n",
				(unsigned)DCMI.getReceivedSize(), (unsigned)warnings);
			frame_completion_reported = true;
		}
		return stopped && complete;
	}

	bool CapturePreview() {
		if (!PreparePreview()) return false;
		byte frames = discard_preview ? 2 : 1;
		for (byte i = 0; i < frames; ++i) {
			line_events.store(0, MemoryOrder_Relaxed);
			vsync_events.store(0, MemoryOrder_Relaxed);
			minimum_dma_remaining.store(0xFFFFFFFFU, MemoryOrder_Relaxed);
			capture_error.store(false, MemoryOrder_Release);
			if (!DCMI.Start(DCMICaptureMode::Snapshot, captured_image) || !WaitCapture(3000)) return false;
		}
		discard_preview = false;
		return true;
	}

	void DiagnosePreviewOverrun() {
		static bool tested = false;
		if (tested || !(DCMI.getError() & _DCMI_ERROR_OVR)) return;
		tested = true;
		if (!StopCapture()) return;
		bool lcd_enabled = LTDC[LTDCReg::GCR].bitof(0);
		if (!lcd_enabled) return;
		XART1.OutFormat("Capture isolation: pausing LCD scan for 8 preview frames\r\n");
		// Stop LTDC memory reads once to isolate contention on the SDRAM capture path.
		LTDC.enAble(false);
		__DSB();
		discard_preview = false;
		unsigned completed = 0;
		while (completed < 8 && CapturePreview()) ++completed;
		StopCapture();
		LTDC.enAble(lcd_enabled);
		__DSB();
		capture_format = CaptureFormat::None;
		XART1.OutFormat("Capture isolation: LCD restored, completed=%u/8\r\n", completed);
	}

	bool CaptureJPEG(uint16 width, uint16 height, const byte*& data, stduint& length) {
		data = nullptr; length = 0;
		if (!PrepareJPEG(width, height)) return false;
		byte frames = discard_jpeg ? 2 : 1;
		for (byte i = 0; i < frames; ++i) {
			line_events.store(0, MemoryOrder_Relaxed);
			vsync_events.store(0, MemoryOrder_Relaxed);
			minimum_dma_remaining.store(0xFFFFFFFFU, MemoryOrder_Relaxed);
			capture_error.store(false, MemoryOrder_Release);
			if (!DCMI.StartVariable((pureptr_t)jpeg_address, jpeg_capacity) || !WaitCapture(5000)) return false;
		}
		discard_jpeg = false;
		const byte* bytes = (const byte*)jpeg_address;
		stduint received = DCMI.getReceivedSize(), start = 0;
		bool found_start = false;
		if (received > jpeg_capacity) return false;
		// Search only within received bytes; exclude DCMI word padding from the saved stream.
		for (stduint i = 0; i + 1 < received; ++i) {
			if (!found_start && bytes[i] == 0xFF && bytes[i + 1] == 0xD8) {
				start = i; found_start = true; ++i;
			} else if (found_start && bytes[i] == 0xFF && bytes[i + 1] == 0xD9) {
				data = bytes + start; length = i + 2 - start;
				return true;
			}
		}
		XART1.OutFormat("JPEG markers missing in %u received bytes\r\n", (unsigned)received);
		return false;
	}

	bool ShowImage(const ImageBuffer& image) {
		if (!image.pixels || image.format != PixelFormat::RGB565 || !image.width || !image.height ||
			image.width > lcd_width || image.height > lcd_height || image.stride < image.width * 2 ||
			(uint64)(image.height - 1) * image.stride + image.width * 2 > image.size) return false;
		stduint x = (lcd_width - image.width) / 2, y = (lcd_height - image.height) / 2;
		byte* target = (byte*)SDRAM_BANK1_BASE + (y * lcd_width + x) * 2;
		for (stduint row = 0; row < image.height; ++row)
			MemCopyN(target + row * lcd_width * 2, (const byte*)image.pixels + row * image.stride, image.width * 2);
		LTDC[1].DrawRectangle(GrafRect(0, lcd_height - 16, lcd_width, 16, Color::Black));
		ltdc_text(4, lcd_height - 16, "U Scale  D JPEG  L Focus  R BMP     USART1: ?");
		return true;
	}

	bool InitCard() {
		if (card_ready) return true;
		if (!SDCard1.setMode()) { XART1.OutFormat("SD initialization failed; preview remains available\r\n"); return false; }
		SDCard1.Block_Size = 512;
		SDCard1.storage_method = IOMethod::Loop;
		if (!filesystem.loadfs()) { XART1.OutFormat("FAT mount failed: %u\r\n", (unsigned)filesystem.error_number); return false; }
		FilesysSearchArgs search{};
		search.handle_buffer = &photo_directory;
		if (!filesystem.search("/PHOTO", &search)) {
			FilesysCreateParameter parameter;
			if (!filesystem.create("/PHOTO", O_DIRECTORY, parameter)) return false;
			delete (FAT_FileHandle*)parameter.handler;
			if (!filesystem.search("/PHOTO", &search)) return false;
		}
		if (!photo_directory.is_dir) return false;
		card_ready = true;
		XART1.OutFormat("FAT%u ready: /PHOTO\r\n", (unsigned)filesystem.fat_type);
		return true;
	}

	FAT_FileHandle* CreatePhoto(bool jpeg, char* path) {
		MemCopyN(path, jpeg ? "/PHOTO/PIC00000.JPG" : "/PHOTO/PIC00000.BMP", 20);
		for (uint32 index = 0; index < 65535; ++index) {
			uint32 digits = index;
			for (byte i = 0; i < 5; ++i) { path[14 - i] = (char)('0' + digits % 10); digits /= 10; }
			FAT_FileHandle existing{};
			FilesysSearchArgs search{};
			search.handle_buffer = &existing;
			if (filesystem.search(path, &search)) continue;
			FilesysCreateParameter parameter;
			parameter.handler = &photo_directory;
			if (!filesystem.create(path, 0, parameter)) return nullptr;
			return (FAT_FileHandle*)parameter.handler;
		}
		return nullptr;
	}

	// Byte-sized storage units let BMPCodec write a new file without reading past its current EOF.
	class PhotoStorage : public StorageTrait {
		FilesysFAT& fs;
		FAT_FileHandle& handle;
	public:
		PhotoStorage(FilesysFAT& fs, FAT_FileHandle& handle) : fs(fs), handle(handle) {
			Block_Size = 1; Block_buffer = nullptr; readable = writable = true;
		}
		using BlockTrait::Read;
		using BlockTrait::Write;
		bool Read(stduint offset, void* dest, stduint count = 1) override {
			if (!dest || offset > handle.size || count > handle.size - offset) return false;
			return fs.readfl(&handle, Slice{offset, count}, (byte*)dest) == count;
		}
		bool Write(stduint offset, const void* source, stduint count = 1) override {
			if (!source || !count || offset > (stduint)-1 - count) return false;
			return fs.writfl(&handle, Slice{offset, count}, (const byte*)source) == count;
		}
		stduint getUnits() override { return handle.size; }
		int operator[](uint64 offset) override {
			byte value;
			return offset < handle.size && Read((stduint)offset, &value) ? value : -1;
		}
	};

	void ReportPhoto(bool ok, const char* path) {
		if (ok) {
			XART1.OutFormat("Saved %s\r\n", path);
			ltdc_text(4, 4, path);
			if (sensor.enBuzzer(true)) { SysDelay_ms(100); sensor.enBuzzer(false); }
		} else {
			XART1.OutFormat("Photo save failed: %s, FAT error %u\r\n", path, (unsigned)filesystem.error_number);
			ltdc_text(4, 4, "Photo save failed");
		}
	}

	bool SavePhoto(bool jpeg) {
		const byte* jpeg_data = nullptr;
		stduint jpeg_length = 0;
		if (jpeg) {
			if (!CaptureJPEG(2592, 1944, jpeg_data, jpeg_length)) return false;
		} else {
			if (!CapturePreview() || !ShowImage(captured_image)) return false;
		}
		if (!SelectCard()) return false;
		char path[24] = {};
		bool ok = InitCard();
		FAT_FileHandle* handle = ok ? CreatePhoto(jpeg, path) : nullptr;
		ok = handle != nullptr;
		if (handle) {
			if (jpeg) {
				ok = filesystem.writfl(handle, Slice{0, jpeg_length}, jpeg_data) == jpeg_length;
			} else {
				PhotoStorage output(filesystem, *handle);
				BMPCodec codec;
				ImageEncodeOptions options;
				ImageEncodeOptionsInit(options);
				ok = codec.Encode(captured_image, output, mempool, options) == ImageResult::OK;
			}
			delete handle;
			if (!ok) filesystem.remove(path);
		}
		if (!SelectCamera()) ok = false;
		ReportPhoto(ok, path);
		return ok;
	}

	bool SendJPEG() {
		const byte* data;
		stduint length;
		if (!CaptureJPEG(jpeg_sizes[jpeg_size_index][0], jpeg_sizes[jpeg_size_index][1], data, length)) return false;
		XART1.OutFormat("JPEG %ux%u: %u bytes -> USART2\r\n", (unsigned)jpeg_sizes[jpeg_size_index][0],
			(unsigned)jpeg_sizes[jpeg_size_index][1], (unsigned)length);
		for (stduint sent = 0; sent < length;) {
			stduint count = length - sent;
			if (count > 256) count = 256;
			if (XART2.out((const char*)data + sent, count) != (int)count) return false;
			sent += count;
			PollControls();
			// Match the original viewer: controls may interrupt the current serial frame.
			if (serial_stream && command_tail != command_head) break;
		}
		return true;
	}

	void PrintHelp() {
		XART1.OutFormat("95-Camera commands (USART1 115200):\r\n"
			" b BMP photo; j 2592x1944 JPEG photo; f single focus; a continuous focus\r\n"
			" u scale/crop; c contrast; e effect; r JPEG size; g send one JPEG\r\n"
			" t continuous JPEG -> USART2 (921600 PA2); p return to preview\r\n"
			" w white balance; s saturation; +/- brightness; z test pattern; l flash\r\n"
			" m retry SD mount; ? help\r\n"
			" Preview keys: U scale, D JPEG, L focus, R BMP\r\n"
			" Stream keys: U size, D focus, L effect, R contrast\r\n");
	}

	void HandleCommand(char command) {
		bool ok = true;
		switch (command) {
		case '?': case 'h': PrintHelp(); return;
		case 'b': ok = SavePhoto(false); break;
		case 'j': ok = SavePhoto(true); break;
		case 'f': ok = focus_ready && sensor.FocusSingle(); break;
		case 'a': ok = focus_ready && sensor.FocusContinuous(); break;
		case 'u': scaled_preview = !scaled_preview; capture_format = CaptureFormat::None; break;
		case 'c': contrast = (byte)((contrast + 1) % 7); ok = sensor.setContrast(contrast); break;
		case 'e': effect = (byte)((effect + 1) % 7); ok = sensor.setEffect(effect); break;
		case 'w': light_mode = (byte)((light_mode + 1) % 5); ok = sensor.setLightMode(light_mode); break;
		case 's': saturation = (byte)((saturation + 1) % 7); ok = sensor.setSaturation(saturation); break;
		case '+': if (brightness < 8) ++brightness; ok = sensor.setBrightness(brightness); break;
		case '-': if (brightness) --brightness; ok = sensor.setBrightness(brightness); break;
		case 'z': test_pattern = (byte)((test_pattern + 1) % 3); ok = sensor.setTestPattern(test_pattern); break;
		case 'l': flash_enabled = !flash_enabled; ok = sensor.enFlash(flash_enabled); break;
		case 'r':
			jpeg_size_index = (byte)((jpeg_size_index + 1) % (sizeof(jpeg_sizes) / sizeof(jpeg_sizes[0])));
			XART1.OutFormat("JPEG size %ux%u\r\n", (unsigned)jpeg_sizes[jpeg_size_index][0],
				(unsigned)jpeg_sizes[jpeg_size_index][1]);
			break;
		case 'g': ok = SendJPEG(); break;
		case 't': serial_stream = !serial_stream; break;
		case 'p': serial_stream = false; break;
		case 'm':
			ok = SelectCard();
			if (ok) { card_ready = false; ok = InitCard(); if (!SelectCamera()) ok = false; }
			break;
		default: return;
		}
		if (!ok) capture_format = CaptureFormat::None;
		XART1.OutFormat("Command %c: %s (contrast %u, effect %u, brightness %u)\r\n", command,
			ok ? "OK" : "FAILED", (unsigned)contrast, (unsigned)effect, (unsigned)brightness);
	}
}

int main() {
	InitMemoryProtection();
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();
	InitPreviewTiming();
	LEDB.setMode(GPIOMode::OUT) = true;
	LEDR.setMode(GPIOMode::OUT) = true;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true);
	if (!XART1.setMode(115200)) erro();
	// UART2 defaults to PD5/PD6 in the library; this board's camera viewer uses PA2/PA3.
	XART2.auto_setmode_apio = false;
	ConfigurePin(GPIOA[2], 7); ConfigurePin(GPIOA[3], 7);
	if (!XART2.setMode(921600)) erro();
	XART1.OutFormat("95-Camera: OV5640\r\n");
	XART1.OutFormat("Preview timing: DWT core clock=%u Hz\r\n", (unsigned)preview_clock_hz);
	uint64 stage_begin = SysTick::getTick();
	XART1.OutFormat("Initializing SDRAM/LCD...\r\n");
	sdram_init();
	ltdc_init();
	XART1.OutFormat("SDRAM/LCD ready: %u ms\r\n", (unsigned)(SysTick::getTick() - stage_begin));
	mempool.enable_auto_expand = false;
	mempool.Append(Slice{pool_address, pool_size});
	LTDC[1].DrawRectangle(GrafRect(0, 0, lcd_width, lcd_height, Color::Black));
	ltdc_text(4, 4, "OV5640 initializing...");
	stage_begin = SysTick::getTick();
	XART1.OutFormat("Initializing OV5640 registers...\r\n");
	if (!sensor.Initialize()) {
		XART1.OutFormat("OV5640 initialization failed, ID 0x%04X\r\n", (unsigned)sensor.getIdentity());
		ltdc_text(4, 24, "OV5640 initialization failed");
		erro();
	}
	if (!sensor.setRGB565()) erro();
	XART1.OutFormat("OV5640 ID 0x%04X, registers ready: %u ms\r\n", (unsigned)sensor.getIdentity(),
		(unsigned)(SysTick::getTick() - stage_begin));
	stage_begin = SysTick::getTick();
	XART1.OutFormat("Uploading autofocus firmware (4077 bytes)...\r\n");
	focus_ready = sensor.InitializeFocus();
	XART1.OutFormat("Autofocus firmware %s: %u ms\r\n", focus_ready ? "ready" : "failed",
		(unsigned)(SysTick::getTick() - stage_begin));
	stage_begin = SysTick::getTick();
	if (focus_ready) focus_ready = sensor.FocusContinuous();
	XART1.OutFormat("Autofocus continuous %s: %u ms\r\n", focus_ready ? "ready" : "unavailable",
		(unsigned)(SysTick::getTick() - stage_begin));
	stage_begin = SysTick::getTick();
	XART1.OutFormat("Initializing SD/FAT...\r\n");
	if (!SelectCard()) erro();
	InitCard();
	if (!SelectCamera()) erro();
	XART1.OutFormat("SD/FAT initialization finished: %u ms\r\n", (unsigned)(SysTick::getTick() - stage_begin));

	if (!DMA[1].enClock()) erro();
	const DMAStream& capture_dma = DMA[1][1];
	if (!capture_dma[0].setMode(true, false, false, true, false, 4, 4, 2)) erro();
	// FIFO is enabled explicitly on H7; ConfigureCapture selects the threshold and memory burst.
	DMA[1][DMAReg::FCR[1]].setof(_DMA_SxFCR_POS_DMDIS, true);
	capture_dma[0].setFIFOThreshold(DMAChannel::_1HalfFull);
	capture_dma.setInterruptPriority(2, 3);
	captured_image.width = lcd_width; captured_image.height = lcd_height;
	captured_image.stride = lcd_width * 2;
	captured_image.format = PixelFormat::RGB565;
	captured_image.colorSpace = ColorSpace::SRGB;
	captured_image.alphaMode = ImageAlphaMode::NONE;
	captured_image.pixels = (void*)capture_address;
	captured_image.size = image_bytes;
	captured_image.allocator = nullptr;
	PrintHelp();
	uint64 last_led = SysTick::getTick();
	while (true) {
		PollControls();
		char command = PopCommand();
		if (command) { HandleCommand(command); preview_timing = PreviewTiming{}; }
		bool ok;
		if (serial_stream) {
			preview_timing = PreviewTiming{};
			ok = SendJPEG();
		} else {
			uint32 begin = ReadPreviewCycles();
			ok = CapturePreview();
			uint32 captured = ReadPreviewCycles();
			if (ok) ok = ShowImage(captured_image);
			uint32 displayed = ReadPreviewCycles();
			if (ok) ReportPreviewTiming(begin, captured, displayed);
		}
		if (!ok) {
			preview_timing = PreviewTiming{};
			if (!serial_stream) DiagnosePreviewOverrun();
			XART1.OutFormat("Camera operation failed; restoring capture configuration\r\n");
			StopCapture();
			capture_format = CaptureFormat::None;
			LEDR.Toggle();
			SysDelay_ms(200);
		}
		if (SysTick::getTick() - last_led >= 500) { LEDB.Toggle(); last_led = SysTick::getTick(); }
	}
}

void printlog(loglevel_t level, const char* fmt, ...) { (void)level; (void)fmt; }
void erro(const char* str) {
	(void)str;
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i = 0; i < 1000000; ++i) {}
	}
}
