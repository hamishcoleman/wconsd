
all: wconsd.exe

get:
	pscp 192.168.1.1:s/src/wconsd/* ./

put:
	pscp ./* 192.168.1.1:s/src/wconsd/


wconsd.exe: wconsd.c
	gcc -o wconsd.exe wconsd.c

