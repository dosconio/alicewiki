# Arinux 202512

nvmeid?=0
dev_root=/dev/nvme$(nvmeid)n1p6
dev_boot=/dev/nvme$(nvmeid)n1p2

src_soft=/her/tmp0/lfs-packages-12.3-rc2.tar

ARI=/mnt/arinux

ento:
	@echo assert $(ARI) /mnt/arinux



#### #### #### #### mount #### #### #### ####

mount:
	mount -v -t ext4 $(dev_root) $(ARI)
	-su - ari # sudo chroot $(ARI) /bin/bash
	umount $(ARI)

# init
# - LFS Chapter 1~4
init: # su -
	-mkdir -pv $(ARI)
	mount -v -t ext4 $(dev_root) $(ARI)
	chown root:root $(ARI)
	chmod 755 $(ARI)
	
	# add sources of softwares
	## now Arinux has not own packman
	-mkdir -v $(ARI)/sources
	chmod -v a+wt $(ARI)/sources
	tar -xvf $(src_soft) -C $(ARI)/sources
	## The fs records the owner by its UID, and the UID of a normal user in the host distro is not assigned. So the files will be left owned by an unnamed UID in the final system. Fix it.
	chown root:root $(ARI)/sources/*
	bash ./make-roottree.sh

	# make temporary user system
	groupadd ari
	## -s shell -g group -m create_home -k skeleton
	useradd -s /bin/bash -g ari -m -k /dev/null ari
	echo "k" | passwd --stdin ari
	bash ./make-give_tmpusr_power.sh
	echo use 'su - ari' to login

	#
	umount $(ARI)
