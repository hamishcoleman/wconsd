
all: wconsd.exe portenum.exe

get:
	pscp 192.168.1.1:s/src/wconsd/*.c ./

put:
	pscp ./*.exe ./*.c 192.168.1.1:s/src/wconsd/

CFLAGS:=-Wall
CC:=gcc

LIBCLI:=libcli/libcli.o

wconsd.c: debug.h scm.h
win-scm.c: scm.h

wconsd.exe: wconsd.o win-scm.o $(LIBCLI)
	$(CC) -o $@ $^ -lws2_32

portenum.exe: portenum.c
	$(CC) $(CFLAGS) -o $@ portenum.c -lwinspool -lsetupapi

test: wconsd.exe
	./wconsd.exe -d

wconsd: wconsd.c
	winegcc -Wall -mno-cygwin -mwindows -o $@ wconsd.c -lws2_32

wine: wconsd
	/usr/lib/wine/wine.bin wconsd.exe.so -p 9600

