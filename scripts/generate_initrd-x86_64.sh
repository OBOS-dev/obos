export old_pwd=$PWD
mkdir tar
cd $(git rev-parse --show-toplevel)
cp out/uart tar
mkdir tar/dev
cd tar
tar -H ustar -cf ../config/initrd.tar *
cd $old_pwd