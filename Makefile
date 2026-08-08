all: run

run: 
	cc6502 --target=mega65 src/main.c
	ln6502 --target=mega65 --output-format=prg -o main.prg mega65-plain.scm main.o
	xemu-xmega65 -besure -prg main.prg

clean:
	rm main.o main.prg
