old_wd=$PWD
cd `git rev-parse --show-toplevel`

rm -f qemu_log.txt

qemu-system-m68k \
-M virt \
-kernel out/m68k_bootloader \
-cpu m68040 \
-monitor stdio \
-s -S \
-m 512M \
-d int \
-D qemu_log.txt \
-serial tcp:0.0.0.0:1534,server,nowait \
-append "--root-fs-uuid=initrd --log-level=2 --initrd-module=initrd --initrd-driver-module=initrd_driver --working-set-cap=8388608 --init-args -cignore /usr/bin/bash --login" \
-initrd "config/initrd.tar"

cd $old_wd
