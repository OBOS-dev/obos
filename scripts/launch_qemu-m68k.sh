cd ../

rm qemu_log.txt

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
-append "--root-fs-uuid=initrd --log-level=1 --initrd-module=initrd --initrd-driver-module=initrd_driver --working-set-cap=8388608 --init-args -cignore /bin/bash" \
-initrd "config/initrd.tar"

cd scripts
