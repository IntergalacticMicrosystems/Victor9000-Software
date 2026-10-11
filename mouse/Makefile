# MOUSE.DEV (Victor 9000 serial mouse driver) - byte-exact NASM rebuild, plus MOUSE.COM
# Run from this directory (make -C mouse ... from the repo root).
NASM ?= nasm

.PHONY: all verify test-dosbox clean

all: MOUSE_rebuilt.bin MOUSE.COM

MOUSE_rebuilt.bin: MOUSE.asm
	$(NASM) -f bin MOUSE.asm -o $@

# MOUSE.COM: the same resident code as a run-anytime TSR (see MOUSE_COM.asm)
MOUSE.COM: MOUSE_COM.asm MOUSE.asm
	$(NASM) -f bin -I./ MOUSE_COM.asm -o $@

verify: MOUSE_rebuilt.bin MOUSE.COM
	@cmp MOUSE.DEV MOUSE_rebuilt.bin && echo "byte-exact: $$(stat -c %s MOUSE_rebuilt.bin) bytes"
	@echo "MOUSE.COM: $$(stat -c %s MOUSE.COM) bytes"

# Install-path smoke test of MOUSE.COM under headless DOSBox (needs dosbox): see dosbox_test/run.sh
test-dosbox: MOUSE.COM
	./dosbox_test/run.sh

clean:
	rm -f MOUSE_rebuilt.bin MOUSE.COM MOUSE.lst
