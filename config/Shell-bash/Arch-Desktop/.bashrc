# ASCII Config(bashrc) TAB4 LR
# doscon.io
# Apply this by `cp -r ./. ~`.
# If using `zsh`, append `source ~/.bashrc`
# If not running interactively, don't do anything
[[ $- != *i* ]] && return

# PS1='[\u@\h \e[35m\w\e[0m]\$ '
PS1='[\u \e[35m\w\e[0m]\$ '

#### #### #### #### PROXY #### #### #### ####

export http_proxy=http://127.0.0.1:7897
export https_proxy=$http_proxy
export socks5_proxy="socks5://127.0.0.1:7897"

#### #### #### #### HERSYS #### #### #### ####

export uincpath=/her/unisym/inc
export ulibpath=/her/unisym/lib
export uobjpath=/home/ayano/obj
export ubinpath=/home/ayano/bin
export PATH=~/bin/AMD64/Lin64/:$PATH

#### #### #### #### ALIAS #### #### #### ####

alias ls='ls --color=auto'
alias grep='grep --color=auto'
alias cls='clear'

#### #### #### #### . #### #### #### ####

export ZIM_HOME=~/soft/zim

