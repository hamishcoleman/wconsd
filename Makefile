
all: wconsd.exe

get:
	pscp 192.168.1.1:s/src/wconsd/*.c ./

put:
	pscp ./*.c 192.168.1.1:s/src/wconsd/


wconsd.exe: wconsd.c
	gcc -o wconsd.exe wconsd.c -lws2_32

