cd ..
export address=$1
export exe=$2
if [ -z "$exe" ]
then
	export exe=out/oboskrnl
fi
m68k-obos-addr2line -e $exe -Cfpira $address
#m68k-obos-gdb "$exe" -batch -ex "set disassembly-flavor intel" -ex "disassemble/sr $address" | grep --color -i -C 10 -n $address
m68k-obos-objdump -d $exe -C | grep --color -i -C 10 -n ${address##0x}
cd scripts
