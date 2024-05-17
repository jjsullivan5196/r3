COSMOCC_ZIP="https://cosmo.zip/pub/cosmocc/cosmocc-3.3.6.zip"
R3_STATIC="https://rebolsource.net/downloads/experimental/r3-linux-x64-gbf237fc-static"
CC="../ape/cosmocc/bin/cosmocc -I/opt/cosmos/include -L/opt/cosmos/lib"

r3: ../make/r3
toolchain: ../make/r3-make ./cosmocc/bin/cosmocc

../make/r3: toolchain
	cd ../make; CC=$(CC) ./r3-make -qs ../src/tools/make-make.reb 0.4.40 linux_x64
	CC=$(CC) make -C ../make prep r3

clean:
	make -C ../make clean
	rm ../make/r3

../make/r3-make:
	curl $(R3_STATIC) -o ../make/r3-make
	chmod +x ../make/r3-make

./cosmocc/bin/cosmocc:
	mkdir cosmocc || true
	cd cosmocc; curl $(COSMOCC_ZIP) -o ./cosmocc.zip
	cd cosmocc; unzip ./cosmocc.zip

clean-toolchain:
	rm ../make/r3-make
	rm ./cosmocc.zip
	rm -rf ./cosmocc
