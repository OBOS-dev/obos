clear

cd ..
export address=$1
export exe=$2
if [ -z "$exe" ]
then
	export exe=out/oboskrnl
fi
addr2line -e $exe -Cfpira $address
gdb "$exe" -batch -ex "set disassembly-flavor intel" -ex "disassemble/sr $address" | grep --color -i -C 10 -n $address
# objdump -d $exe -C -M intel | grep --color -i -C 10 -n $address
cd scripts
