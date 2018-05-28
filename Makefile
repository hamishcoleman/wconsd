
all: wconsd.exe portenum.exe svctest.exe

# These two targets were used to exchange files with a windows machine for
# compilation and testing
get:
	pscp 192.168.1.1:s/src/wconsd/*.c ./

put:
	pscp ./*.exe ./*.c 192.168.1.1:s/src/wconsd/

CFLAGS:=-Wall
#CC:=gcc
CC:=i586-mingw32msvc-gcc

LIBCLI:=libcli/libcli/libcli.o

wconsd.c: debug.h scm.h
win-scm.c: scm.h

modules.c: module.h

MODULES:=modules.o win-scm.o

wconsd.exe: wconsd.o $(MODULES) $(LIBCLI)
	$(CC) -o $@ $^ -lws2_32

svctest.exe: svctest.o win-scm.c
	$(CC) -o $@ $^

portenum.exe: portenum.c
	$(CC) $(CFLAGS) -o $@ portenum.c -lwinspool -lsetupapi

test: wconsd.exe
	./wconsd.exe -d

wconsd: wconsd.c
	winegcc -Wall -mno-cygwin -mwindows -o $@ wconsd.c -lws2_32

wine: wconsd
	/usr/lib/wine/wine.bin wconsd.exe.so -p 9600

clean:
	rm -f *.o wconsd.exe portenum.exe svctest.exe
