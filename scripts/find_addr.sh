clear

cd ..
export address=$1
export exe=$2
if [ -z "$exe" ]
then
	export exe=out/oboskrnl
fi
addr2line -e $exe -Cfpira 0x$address
objdump -d $exe -C -M intel | grep --color -C 10 -n $address
cd scripts
