# Cosy

Cosy is a small native Linux debugger for x86-64 and AArch64 programs. It uses
ptrace, ELF symbols, and DWARF debug information; it is intentionally a CLI,
not an IDE or an AI agent.

## Build

Debian/Ubuntu dependencies:

~~~sh
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libdw-dev libelf-dev libreadline-dev python3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
~~~

Build a program that exposes source-level features with:

~~~sh
c++ -g -O0 -fno-omit-frame-pointer demo.cpp -o demo
./build/cosy -- ./demo
~~~

## Commands

help, continue (c), stepi (si), step (s), next (n), finish, break (b), delete,
enable, disable, info break, backtrace (bt), frame, info locals, print, info
registers, register read/write, x, set memory, info threads, thread, watch,
info sharedlibrary, and quit are available. Run help in the debugger for
syntax.

Breakpoint locations can be *0xADDRESS, function, library.so!function, or
file.cpp:line. A shared-library breakpoint remains pending until the dynamic
loader maps that library.

## Limits

Cosy is Linux-only. Source stepping, locals, and frame walking are guaranteed
for debug-first binaries (-g -O0 -fno-omit-frame-pointer). Optimized,
stripped, JIT-generated, or statically unusual binaries can still use address
breakpoints, instruction stepping, registers, and memory, but may have limited
symbolic information. Hardware watchpoint slots are architecture limited
(normally four). During source and instruction stepping other traced threads
remain paused; continue resumes all stopped threads.
