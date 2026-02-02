export ARI='/mnt/arinux'

mkdir -pv $ARI/{her,etc,var} $ARI/usr/{bin,lib,sbin}

for i in bin lib sbin; do
    ln -sv usr/$i $ARI/$i
done

case $(uname -m) in
    x86_64) mkdir -pv $ARI/lib64 ;;
esac

mkdir -pv $ARI/tools
