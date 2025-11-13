ARCH=x86_64
ARCA=x86
KNEL=bzImage
INIF=initramfs.img
UNIX=/usr/src/linux-6.5

all: cp run
	@echo ciallo
cp:
	cp $(UNIX)/arch/$(ARCA)/boot/bzImage ./$(KNEL)
	mkdir -p ./initramfs/bin/
	cp ~/Documents/busybox-1.36.1/_install/bin/busybox ./initramfs/bin/
	
init: initramfs/sa_call452 initramfs/sa_uart
	-cp app/*.ko initramfs/out/
	@cd ./initramfs && find . -print0 | cpio -ov --null --format=newc | gzip -9 > ../$(INIF)
run: init
	@qemu-system-$(ARCH) -kernel $(KNEL) -initrd $(INIF) -m 256M -nographic \
		-append "earlyprintk=serial,ttyS0 console=ttyS0 nokaslr" \
		-gdb tcp::9000 -smp 2
dun: init
	@qemu-system-$(ARCH) -kernel $(KNEL) -initrd $(INIF) -m 256M -nographic \
		-append "earlyprintk=serial,ttyS0 console=ttyS0 nokaslr" \
		-gdb tcp::9000 \
		-S
dbg:
	clear
	@cp ./gdbinit ~/lindbg.gdbinit
	cd $(UNIX) && sudo gdb vmlinux -x ~/lindbg.gdbinit
	
#
initramfs/sa_call452: app/mycall-452.c
	gcc $< -o $@ -static
initramfs/sa_uart: app/mycall-uart.c
	gcc $< -o $@ -static

