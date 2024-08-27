export old_pwd=$PWD
if [[ ! -d tar ]]
then
    mkdir tar
fi
cd $(git rev-parse --show-toplevel)
cp out/uart tar
cp out/ahci tar
cp out/fat tar
if [[ ! -d tar/dev ]]
then
    mkdir tar/dev
fi
cd tar
tar -H ustar -cf ../config/initrd.tar *
cd $old_pwd
