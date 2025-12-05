export old_pwd=$PWD
cd $(git rev-parse --show-toplevel)
mkdir -p tar
mkdir -p tar/dev
mkdir -p tar/sys/perm
cp out/uart tar
cp out/ahci tar
cp out/extfs tar
cp out/bochs_vbe tar
cp out/r8169 tar
cp out/i8042 tar
cp out/libps2 tar
cp out/e1000 tar
cp out/init tar
cd tar
tar -H ustar -cf ../config/initrd.tar `ls -A`
cd $old_pwd
