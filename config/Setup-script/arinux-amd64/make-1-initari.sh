cat > ~/.bash_profile << "EOF"
exec env -i HOME=$HOME TERM=$TERM PS1='\u:\w\$ ' /bin/bash
EOF

cat > ~/.bashrc << "EOF"
set +h
umask 022
ARI=/mnt/arinux
LC_ALL=POSIX
ARI_TGT=$(uname -m)-arinux-linux-gnu
PATH=/usr/bin
if [ ! -L /bin ]; then PATH=/bin:$PATH; fi
PATH=$ARI/tools/bin:$PATH
CONFIG_SITE=$ARI/usr/share/config.site
export ARI LC_ALL ARI_TGT PATH CONFIG_SITE
EOF

# [ ! -e /etc/bash.bashrc ] || mv -v /etc/bash.bashrc /etc/bash.bashrc.NOUSE

cat >> ~/.bashrc << "EOF"
export MAKEFLAGS=-j$(nproc)
EOF

source ~/.bash_profile
