export old_pwd=$PWD
cd $(git rev-parse --show-toplevel)
if [[ ! -d tar ]]
then
    mkdir tar
fi
cp out/uart tar
cp out/ahci tar
rm tar/ahci
cp out/slowfat tar
cp out/bochs_vbe tar
cp out/r8169 tar
cp out/i8042 tar
cp out/libps2 tar
#cp out/init tar
if [[ ! -d tar/dev ]]
then
    mkdir tar/dev
fi
cd tar
tar -H ustar -cf ../config/initrd.tar `ls -A`
cd $old_pwd
