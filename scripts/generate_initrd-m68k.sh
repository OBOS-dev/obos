export old_pwd=$PWD
cd $(git rev-parse --show-toplevel)
if [[ ! -d tar ]]
then
    mkdir tar
fi
# TODO: Drivers?
cp out/init tar/
if [[ ! -d tar/dev ]]
then
    mkdir tar/dev
fi
cd tar
tar -H ustar -cf ../config/initrd.tar `ls -A`
cd $old_pwd
