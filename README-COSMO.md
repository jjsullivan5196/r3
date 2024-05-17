## Build REBOL 3 as an Actually Portable Executable

Clone this repository first, checkout the `cosmo-oldes` branch

Make sure you have the following programs on your PATH:

- `curl`
- `unzip`
- GNU `make`

Run `make -C ./ape r3`, make will download the cosmocc toolchain and a static build of REBOL3, then build the interpreter.

Run the newly built interpreter with `./make/r3`

### If you can't download the toolchain with the makefile

- Download [https://cosmo.zip/pub/cosmocc/cosmocc-3.3.6.zip](cosmocc) and unzip it to `ape/cosmocc/`
- Download [https://rebolsource.net/downloads/experimental/r3-linux-x64-gbf237fc-static](rebol3-static) to `make/r3-make`, make it executable with `chmod +x ./make/r3-make`