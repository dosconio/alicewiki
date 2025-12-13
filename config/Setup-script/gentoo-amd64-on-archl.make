# UTF-8 Makefile TAB4 LF
# doscon.io

dst=/dev/nvme0n1p5
efi=/dev/nvme0n1p2

stage3=/home/ayano/Downloads/stage3-amd64-desktop-systemd-20251207T170056Z.tar.xz
hostname="spade"
user=ayano


all:
	@echo 'setup Gentoo amd64 (based on ArchLinux)'

setup_uni:
	sudo mkdir -p /her
	sudo chmod -R 777 /her


	
setup_sys2: # under archlinux host, `su -`
	# 配置 fstab
	# arch-install-scripts
	genfstab -U /mnt/gentoo >> /mnt/gentoo/etc/fstab
	#
	grub-mkconfig -o /boot/grub/grub.cfg
	#
	umount -l /mnt/gentoo/dev{/shm,/pts,}
	umount -R /mnt/gentoo
	reboot

# 正在生成 grub 配置文件 ...
# 找到 Linux 镜像：/boot/vmlinuz-linux
# 找到 initrd 镜像：/boot/amd-ucode.img /boot/initramfs-linux.img
# 警告： os-prober 将运行以检测其它可引导分区。
# 将使用 os-prober 的输出，以检测分区中可引导的二进制文件，并为其创建新的启动项。
# 发现了 Gentoo Linux，位于 /dev/nvme0n1p5
# 发现了 Windows Boot Manager，位于 /dev/nvme1n1p1@/efi/Microsoft/Boot/bootmgfw.efi
# 正在添加 UEFI 固件设置的引导菜单项……
# 完成


setup_sys1: # after chroot (here should exist make)
	source /etc/profile
	export PS1="(chroot) ${PS1}"
	# Initial Portage
	## 同步树
	emerge-webrsync   # 获取最新的 Portage 快照 (比 rsync 快)
	eselect news read # 查看新闻
	emerge --sync     # 同步 Portage 树 (获取最新 ebuild)
	emerge --ask app-editors/vim # 安装 Vim 编辑器 (推荐)
	eselect editor list          # 列出可用编辑器
	eselect editor set nano        # 将 Nano/Vim 设置为默认编辑器 (vi 通常是指向 vim 的软链接)
	## make.conf
	echo 'TODO: CP /etc/portage/make.conf'
	# Profile、系统设置与本地化
	eselect profile list          # 列出所有可用 Profile
	eselect profile set 8    # 设置选定的 Profile
	emerge -avuDN @world          # 更新系统以匹配新 Profile (a:询问 v:详细 u:升级 D:深层依赖 N:新USE)
	# Timezone and Lango
	echo "Asia/Shanghai" > /etc/timezone
	emerge --config sys-libs/timezone-data
	echo "en_US.UTF-8 UTF-8" > /etc/locale.gen
	echo "zh_CN.UTF-8 UTF-8" >> /etc/locale.gen
	locale-gen
	eselect locale set en_US.utf8
	env-update && source /etc/profile && export PS1="(chroot) ${PS1}"
	# 主机名与网络设置
	echo $(hostname) > /etc/hostname
	# 网络管理工具
	emerge --ask net-misc/networkmanager
	systemctl enable NetworkManager # rc-update add NetworkManager default
	# 快速方案：预编译内核
	etc-update
	emerge --ask sys-kernel/gentoo-kernel-bin
	##emerge --ask sys-kernel/gentoo-sources sys-kernel/genkernel
	##genkernel --install all  # 自动编译并安装内核、模块和 initramfs
	# 安装固件与微码
	mkdir -p /etc/portage/package.license
	## 同意 Linux 固件的授权条款
	echo 'sys-kernel/linux-firmware linux-fw-redistributable no-source-code' > /etc/portage/package.license/linux-firmware
	echo 'sys-kernel/installkernel dracut' > /etc/portage/package.use/installkernel
	emerge --ask sys-kernel/linux-firmware
	# emerge --ask sys-firmware/intel-microcode  # Intel CPU
	# 系统服务工具
	systemctl enable systemd-timesyncd
	emerge --ask sys-fs/e2fsprogs  # ext4
	emerge --ask sys-fs/xfsprogs   # XFS
	emerge --ask sys-fs/dosfstools # FAT/vfat (EFI 分区需要)
	emerge --ask sys-fs/btrfs-progs # Btrfs
	# 建立用户与权限
	passwd root # 设置 root 密码
	useradd -m -G wheel,video,audio,plugdev,network,lp $(user) # 创建用户并加入常用组
	passwd $(user) # 设置用户密码
	emerge --ask app-admin/sudo
	echo "%wheel ALL=(ALL) ALL" > /etc/sudoers.d/wheel # 允许 wheel 组使用 sudo
	# 引导程序
	emerge --ask sys-boot/grub:2
	emerge --ask sys-boot/os-prober
	echo 'GRUB_DISABLE_OS_PROBER=false' >> /etc/default/grub

mount_again:
	sudo mount $(dst) /mnt/gentoo
	sudo mount $(efi) /mnt/gentoo/efi
	sudo mount --types proc /proc /mnt/gentoo/proc
	sudo mount --rbind /sys /mnt/gentoo/sys
	sudo mount --rbind /dev /mnt/gentoo/dev
	sudo mount --rbind /run /mnt/gentoo/run
	sudo mount --make-rslave /mnt/gentoo/sys
	sudo mount --make-rslave /mnt/gentoo/dev
	sudo mount --make-rslave /mnt/gentoo/run
	sudo chroot /mnt/gentoo /bin/bash 
	sudo umount -l /mnt/gentoo/dev{/shm,/pts,}
	sudo umount -R /mnt/gentoo

setup_sys0:
	cd /mnt/gentoo && sudo tar xpvf $(stage3) --xattrs-include='*.*' --numeric-owner
	sudo cp --dereference /etc/resolv.conf /mnt/gentoo/etc/  # copy DNS
	# below are safe to host OS
	sudo mount --types proc /proc /mnt/gentoo/proc
	sudo mount --rbind /sys /mnt/gentoo/sys
	sudo mount --rbind /dev /mnt/gentoo/dev
	sudo mount --rbind /run /mnt/gentoo/run
	sudo mount --make-rslave /mnt/gentoo/sys
	sudo mount --make-rslave /mnt/gentoo/dev
	sudo mount --make-rslave /mnt/gentoo/run
	sudo chroot /mnt/gentoo /bin/bash 

mount:
	sudo mkdir -p /mnt/gentoo
	sudo mount $(dst) /mnt/gentoo
	sudo mkdir -p /mnt/gentoo/efi
	sudo mount $(efi) /mnt/gentoo/efi
	sudo chmod 777 /mnt/gentoo
# NAME        MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS
# nvme0n1     259:0    0 953.9G  0 disk
# ├─nvme0n1p1 259:1    0    14M  0 part
# ├─nvme0n1p2 259:2    0     1G  0 part /mnt/gentoo/efi
# │                                     /boot
# ├─nvme0n1p3 259:3    0    32G  0 part [SWAP]
# ├─nvme0n1p4 259:4    0   256G  0 part /home
# │                                     /
# └─nvme0n1p5 259:13   0 238.4G  0 part /mnt/gentoo

