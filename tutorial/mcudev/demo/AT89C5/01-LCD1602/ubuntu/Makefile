# ASCII C-SDCC TAB4 LF
# Attribute: CPU(STC89)
# LastCheck: 20240303
# AllAuthor: @dosconio
# ModuTitle: LCD1602
# Copyright: 

AS   = sdas8051
CC   = sdcc -I/mnt/hgfs/unisym/inc/c
ULIB = /mnt/hgfs/unisym/lib
PREP = sdcpp

MCU = /dev/ttyUSB0
FLS = stcgal -P stc89 -p $(MCU)

DIR = ~/_obj
IDN = stc89lcd1602
OUT = $(DIR)/$(IDN).ihx
HEX = $(DIR)/$(IDN).hex

.PHONY: defa all main delay lcd lp clean

defa: main delay lcd
	@$(CC) -o $(OUT) $(DIR)/$(IDN).rel $(DIR)/delay.rel $(DIR)/hd44780.rel $(DIR)/lcd1602.rel
	@packihx $(OUT) > $(HEX)

all: clean defa
	@sudo $(FLS) $(HEX)

main: lcd1602ubt.c
	@$(CC) -o $(DIR)/$(IDN).rel -c $(<)

delay: $(ULIB)/asm/8051/delay11M.asm
	@$(AS) -o $(DIR)/delay.rel $(<)
	@$(AS) -l $(DIR)/delay.lst $(<)

lcd: # 
	@$(CC) -o $(DIR)/hd44780.rel -c $(ULIB)/c/driver/HD44780.c -D_HD44780_PORT_CMD=p2 \
		-D_HD44780_PORT_DAT=p0 -D_IMPLEMENT_SDCC8051 \
		-D_HD44780_PIN_RS=p2_6 -D_HD44780_PIN_RW=p2_5 -D_HD44780_PIN_EN=p2_7

	@$(CC) -o $(DIR)/lcd1602.rel -c $(ULIB)/c/driver/device/LCD1602.c -D_IMPLEMENT_SDCC8051

lp: # list ports
	ls /dev/tty*

clean:
	-rm -f $(DIR)/*.rel
	-rm -f $(DIR)/*.ihx
	-rm -f $(DIR)/*.hex
	-rm -f $(DIR)/*.lk
	-rm -f $(DIR)/*.lst
	-rm -f $(DIR)/*.asm
	-rm -f $(DIR)/*.mem
	-rm -f $(DIR)/*.map
	-rm -f $(DIR)/*.rst
	-rm -f $(DIR)/*.sym
