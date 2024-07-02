cd ../

rm qemu_log.txt

qemu-system-x86_64 \
-drive file=out/obos.iso,format=raw \
-m 1G -mem-path /tmp/memory.img \
-gdb tcp:0.0.0.0:1234 -S \
-M q35 \
-cpu Haswell \
-monitor stdio \
-debugcon file:/dev/stdout \
-serial tcp:0.0.0.0:1534,server,nowait \
-smp cores=1,threads=1,sockets=1 \
-M smm=off \
-d int \
-D qemu_log.txt
# -object memory-backend-file,id=mem1,share=on,mem-path=/tmp/memory.img,size=1G \
# -drive if=pflash,format=raw,unit=1,file=ovmf/OVMF_VARS_4M.fd \
# -enable-kvm \
# -drive if=pflash,format=raw,unit=0,file=ovmf/OVMF_CODE_4M.fd,readonly=on
# -drive id=disk2,file=disk.img,if=none,format=raw -device ide-hd,drive=disk2,bus=ahci1.1 \
# -M smm=off \
# -no-reboot
# -enable-kvm \
# -no-shutdown

cd scripts
