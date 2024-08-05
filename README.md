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
cmake -GNinja -DCMAKE_BUILD_TYPE=Release --toolchain=src/build/x86_64/toolchain.cmake -B build .
rem chmod +x dependencies/hyper/hyper_install-linux-x86_64
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
## License Notice
Most code in this repository is licensed under the MIT license.<br>
Any code from other projects that require a license notice will be put below.
### src/oboskrnl/utils/tree.h
Taken from sys/tree.h from OpenBSD, and is licensed under this:
```
 Copyright 2002 Niels Provos <provos@citi.umich.edu>
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
 
 THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
### Hashmap implementation
### src/oboskrnl/utils/hashmap.h & src/oboskrnl/utils/hashmap.c
Found [here](https://github.com/tidwall/hashmap.c)
```The MIT License (MIT)

Copyright (c) 2020 Joshua J Baker

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.```

