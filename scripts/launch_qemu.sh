cd ../

rm qemu_log.txt

qemu-system-x86_64 \
-drive file=out/obos.iso,format=raw \
-m 512M \
-gdb tcp:0.0.0.0:1234 -S \
-cpu "Haswell" \
-M q35 \
-monitor stdio \
-debugcon file:/dev/stdout \
-serial tcp:0.0.0.0:1534,server,nowait \
-smp cores=4,threads=1,sockets=1 \
-d int \
-D qemu_log.txt \
# -object memory-backend-file,id=mem1,share=on,mem-path=/tmp/memory.img,size=1G
# -drive if=pflash,format=raw,unit=1,file=ovmf/OVMF_VARS_4M.fd \
# -drive if=pflash,format=raw,unit=0,file=ovmf/OVMF_CODE_4M.fd,readonly=on
# -drive id=disk2,file=disk.img,if=none,format=raw -device ide-hd,drive=disk2,bus=ahci1.1 \
# -M smm=off \
# -no-reboot
# -no-shutdown

cd scripts
