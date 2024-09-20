export old_pwd=$PWD
cd $(git rev-parse --show-toplevel)
if [[ ! -d tar ]]
then
    mkdir tar
fi
cp out/uart tar
cp out/ahci tar
cp out/slowfat tar
if [[ ! -d tar/dev ]]
then
    mkdir tar/dev
fi
cd tar
tar -H ustar -cf ../config/initrd.tar *
cd $old_pwd
