
all: wconsd.exe

get:
	pscp 192.168.1.1:s/src/wconsd/*.c ./

put:
	pscp ./*.c 192.168.1.1:s/src/wconsd/


wconsd.exe: wconsd.c
	gcc -o wconsd.exe wconsd.c -lws2_32

test: wconsd.exe
	./wconsd.exe -d

wconsd: wconsd.c
	winegcc -mno-cygwin -o wconsd wconsd.c -mwindows -lws2_32

wine: wconsd
	/usr/lib/wine/wine.bin wconsd.exe.so -p 9600

