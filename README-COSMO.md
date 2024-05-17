## Build REBOL 3 as an Actually Portable Executable

Make sure you have the following programs on your PATH:

- `curl`
- `unzip`
- GNU `make`

Run `make -C ./ape r3`, make will download the cosmocc toolchain and a static build of REBOL3, then build the interpreter.

Run the newly built interpreter with `./make/r3`