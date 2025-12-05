export old_pwd=$PWD
cd $(git rev-parse --show-toplevel)
mkdir -p tar tar/dev tar/sys/perm
# TODO: Drivers?
cp out/init tar/
cd tar
tar -H ustar -cf ../config/initrd.tar `ls -A`
cd $old_pwd
