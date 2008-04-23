
all: wconsd.exe portenum.exe

get:
	pscp 192.168.1.1:s/src/wconsd/*.c ./

put:
	pscp ./*.exe ./*.c 192.168.1.1:s/src/wconsd/


wconsd.exe: wconsd.c
	gcc -Wall -o wconsd.exe wconsd.c -lws2_32

portenum.exe: portenum.c
	gcc -Wall -o portenum.exe portenum.c -lwinspool -lsetupapi

test: wconsd.exe
	./wconsd.exe -d

wconsd: wconsd.c
	winegcc -Wall -mno-cygwin -mwindows -o wconsd wconsd.c -lws2_32

wine: wconsd
	/usr/lib/wine/wine.bin wconsd.exe.so -p 9600

