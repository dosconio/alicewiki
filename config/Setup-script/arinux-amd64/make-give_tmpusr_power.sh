export ARI='/mnt/arinux'

chown -v ari $ARI/{usr{,/*},var,etc,tools}
case $(uname -m) in
  x86_64) chown -v ari $ARI/lib64 ;;
esac
