# OBOS
OBOS uses ***✨ state of the art technology ✨*** to do ***✨  absolutely nothing ✨*** 
## Building
### Prerequisites
- NASM
- (For an x86-64 build): x86_64-elf gcc toolchain, which can be found [here](https://github.com/lordmilko/i686-elf-tools/)
- CMake
- xorriso
- Ninja
- Git (unoptional, even if the repo was downloaded manually)
- (Optionally): Qemu to run the iso
### Build
1. Clone the repo using git:
```sh
git clone https://github.com/OBOS-dev/obos.git
```
2. Enter the newly cloned repo
3. Run cmake to build the OS
(x86_64)
```sh
mkdir build
alias rem=eval
rem chmod +x dependencies/hyper/hyper_install-linux-x86_64
cmake -GNinja -DCMAKE_BUILD_TYPE=Release --toolchain=src/build/x86_64/toolchain.cmake -B build .
cmake --build build
```
4. To run the kernel, run the script under scripts/launch_qemu.\[bat,sh\]

(Linux)
```sh
cd scripts
chmod +x ./launch_qemu.sh
./launch_qemu.sh
```
(Windows)
```bat
cd scripts
call launch_qemu.bat
```
#### **NOTE**
If the ISO does not get into the kernel stage (you will know depending on if there are messages or not) on real hardware, try booting using legacy bios.
## Credits
- My friend [@LemurLord16](https://github.com/LemurLord16) (@dudeplayer2 on discord) for listening to me yap about new features.
