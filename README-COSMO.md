## Build Ren-C as an Actually Portable Executable

- Clone this repo and checkout `renc-cosmo-testing`
- Download a [static build of REBOL 3](https://github.com/jjsullivan5196/r3/blob/renc-cosmo-testing/prebuilt/README.md), make it executable if needed
- Download the cosmocc toolchain https://cosmo.zip/pub/cosmocc/ and unzip it
- Create a directory `build` in the repo root, change to it
- Run the following, make sure `CC` is set to the path of the `cosmocc` binary:

```bash
CC="path/to/cosmocc/bin/cosmocc" path/to/r3static ../make.r config: ../configs/cosmo.r 
```

Run `./r3` to start the interpreter.