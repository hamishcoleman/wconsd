
all: wconsd

get:
	pscp 192.168.1.1:s/src/wconsd/* ./

put:
	pscp ./* 192.168.1.1:s/src/wconsd/


