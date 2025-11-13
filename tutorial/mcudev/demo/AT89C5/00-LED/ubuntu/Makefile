# ASCII C-SDCC TAB4 LF
# Attribute: CPU(STC89)
# LastCheck: 20240229
# AllAuthor: @dosconio
# ModuTitle: LED
# Copyright: 

AS = sdas8051 -o # using: $(AS) outf inf0 inf1 ...
CC = sdcc -I/mnt/hgfs/unisym/inc/c -o
PREP = sdcpp

MCU = /dev/ttyUSB0
FLS = stcgal -P stc89 -p $(MCU)

DIR = ~/_obj
IDN = stc89led
OUT = $(DIR)/$(IDN).ihx
HEX = $(DIR)/$(IDN).hex

.PHONY: defa c lp clean

defa: c
	packihx $(OUT) > $(HEX)
	sudo $(FLS) $(HEX)

c: ledubt.c
	@$(CC) $(OUT) $(<) -D_PORT_SPECIAL

lp: # list ports
	ls /dev/tty*

clean:
	-rm -f $(DIR)/$(IDN).rel
	-rm -f $(DIR)/$(IDN).ihx
	-rm -f $(DIR)/$(IDN).hex
	-rm -f $(DIR)/$(IDN).lk
	-rm -f $(DIR)/$(IDN).lst
	-rm -f $(DIR)/$(IDN).asm
	-rm -f $(DIR)/$(IDN).mem
	-rm -f $(DIR)/$(IDN).map
	-rm -f $(DIR)/$(IDN).rst
	-rm -f $(DIR)/$(IDN).sym


