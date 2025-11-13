
NAM=main
POR=/dev/ttyUSB0 #ttyUSB0 # use ST-Link GUI
ASM_SOURCES=../startup_stm32f103xe.gnu.s
CPP_SOURCES=$(NAM).cpp

PREF=arm-none-eabi-
CX = $(PREF)g++
CC = $(PREF)gcc
AS = $(PREF)gcc -x assembler-with-cpp
CP = $(PREF)objcopy
SZ = $(PREF)size
LD = $(PREF)ld
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S
FLS = stm32flash

CFG = Makefile $(ubinpath)/libSTM32F1.a
PCU=-mcpu=cortex-m3
MCU=-mthumb $(FPU) $(FLOAT-ABI)
C_DEFS =  -D_MCU_STM32F1x

LIBS = # -lc -lm -lnosys 
LDSCRIPT = ../STM32F103VETx_FLASH.ld

ASFLAGS = $(MCU) $(AS_DEFS) $(AS_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections
LDFLAGS = $(MCU) -specs=nosys.specs -T$(LDSCRIPT) -L$(ubinpath) -lSTM32F1 $(LIBS) -Wl,-Map=build/$(NAM).map,--cref -Wl,--gc-sections #-specs=nano.specs nosys.specs
CFLAGS += $(MCU) $(C_DEFS) -I$(uincpath) $(OPT) -Wall -fdata-sections -ffunction-sections

ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif

BUILD_DIR=build

CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)" # dependency

OBJECTS=
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASMM_SOURCES:.S=.o)))
vpath %.S $(sort $(dir $(ASMM_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(CPP_SOURCES:.cpp=.o)))
vpath %.cpp $(sort $(dir $(CPP_SOURCES)))

glinuni: init $(BUILD_DIR)/$(NAM).elf $(BUILD_DIR)/$(NAM).hex $(BUILD_DIR)/$(NAM).bin

gwinuni:
keiluni:

init:
	@test -d $(BUILD_DIR) || mkdir -p $(BUILD_DIR)
	-clear
newl: clean glinuni

flash:
	@echo Once may fail, press RESET may better.
	sudo $(FLS) -w $(BUILD_DIR)/$(NAM).hex $(POR)
	sudo $(FLS) -g 0x0 $(POR)

clean:
	-rm -fR $(BUILD_DIR)/*

$(BUILD_DIR)/%.o: %.c $(CFG) | $(BUILD_DIR) 
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@
$(BUILD_DIR)/%.o: %.cpp $(CFG) | $(BUILD_DIR) 
	$(CX) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@
$(BUILD_DIR)/%.o: %.s $(CFG) | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/%.o: %.S $(CFG) | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/$(NAM).elf: $(OBJECTS) $(CFG)
	$(CX) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@
$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

-include $(wildcard $(BUILD_DIR)/*.d)
