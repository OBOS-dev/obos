cd ../

rm qemu_log.txt

echo $@
qemu-system-x86_64 \
-drive format=raw,file=out/obos.iso,media=disk,index=0 \
-m 2G \
-gdb tcp:0.0.0.0:1234 -S \
-M q35 \
-cpu host \
-accel kvm \
-debugcon file:/dev/stdout \
-monitor stdio \
-serial tcp:0.0.0.0:1534,server,nowait \
-smp cores=1,threads=1,sockets=1 \
-d int \
-D qemu_log.txt "$@"

# -nographic
# -enable-kvm \
# -M smm=off \
# -no-reboot
# -enable-kvm \
# -no-shutdown

cd scripts
