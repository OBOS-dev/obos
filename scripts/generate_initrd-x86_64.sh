export old_pwd=$PWD
cd $(git rev-parse --show-toplevel)
cp out/uart tar
cd tar
tar -H ustar -cf ../config/initrd.tar *
cd $old_pwd