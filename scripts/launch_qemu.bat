@echo off

cd ../

del qemu_log.txt

qemu-system-x86_64 ^
-drive file=out/obos.iso,format=raw ^
-m 512M ^
-gdb tcp:0.0.0.0:1234 -S ^
-accel tcg ^
-cpu "Haswell" ^
-M q35 ^
-monitor stdio ^
-debugcon file:CON ^
-serial tcp:0.0.0.0:1534,server,nowait ^
-smp cores=2,threads=1,sockets=1 ^
-d int ^
-D qemu_log.txt
rem -drive if=pflash,format=raw,unit=1,file=ovmf/OVMF_VARS_4M.fd ^
rem -drive if=pflash,format=raw,unit=0,file=ovmf/OVMF_CODE_4M.fd,readonly=on ^
rem -drive id=disk2,file=disk.img,if=none,format=raw -device ide-hd,drive=disk2,bus=ahci1.1 ^
rem -M smm=off ^
REM -no-shutdown ^
REM -no-reboot ^

cd scripts