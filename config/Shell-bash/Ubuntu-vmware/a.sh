#!/bin/bash
# run after login


# ---- VMware Guest ----
#[failed]
# sudo cp a.sh /etc/init.d/a.sh
# sudo update-rc.d a.sh defaults 100 && sudo chmod 777 /etc/init.d/a.sh
# sudo service --status-all|grep a
#
# all shared
# `k` the password of the user
echo k | sudo -S vmhgfs-fuse .host:/ /mnt/hgfs -o subtype=vmhgfs-fuse,allow_other
echo
exit 0

