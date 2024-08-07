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
-D qemu_log.txt

cd scripts