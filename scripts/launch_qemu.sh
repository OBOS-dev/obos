cd ../

rm qemu_log.txt

echo $@
qemu-system-x86_64 \
-drive file=out/obos.iso,format=raw \
-m 256M \
-gdb tcp:0.0.0.0:1234 -S \
-boot d \
-M q35 \
-cpu host \
-accel kvm \
-debugcon file:/dev/stdout \
-monitor stdio \
-smp cores=1,threads=1,sockets=1 \
-serial tcp:0.0.0.0:1534,server,nowait \
-d int \
-D qemu_log.txt "$@"
-smp cores=$(nproc),threads=1,sockets=1 \
-M smm=off \
-D qemu_log.txt
# -nographic
# -enable-kvm \
# -drive id=disk2,file=disk.img,if=none,format=raw -device ide-hd,drive=disk2,bus=ahci1.1 \
# -M smm=off \
# -no-reboot
# -enable-kvm \
# -no-shutdown

cd scripts
