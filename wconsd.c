/*
 * wconsd.c - serial port server service for Windows NT
 *
 * Copyright (c) 2008 Hamish Coleman <hamish@zot.org>
 *               2003 Benjamin Schweizer <gopher at h07 dot org>
 *               1998 Stephen Early <Stephen.Early@cl.cam.ac.uk>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Note: winsock2.h MUST be included before windows.h */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>

#define VERSION "0.2"

/* Size of buffers for send and receive */
#define BUFSIZE 1024
#define MAXLEN 1024

/* Sockets for listening and communicating */
SOCKET ls=INVALID_SOCKET;

/* Event objects */
HANDLE stopEvent;
HANDLE readEvent, writeEvent;
WSAEVENT listenSocketEvent;

/* COM port */
DCB dcb;
COMMTIMEOUTS timeouts;

/* Port default settings are here */
int   com_port=1;
DWORD com_speed=9600;
BYTE  com_data=8;
BYTE  com_parity=NOPARITY;
BYTE  com_stop=ONESTOPBIT;
BOOL  com_autoclose=TRUE;

int   default_tcpport = 23;

/* Service status: our current status, and handle on service manager */
SERVICE_STATUS wconsd_status;
SERVICE_STATUS_HANDLE wconsd_statusHandle;

int debug_mode = 0;

#define MAXCONNECTIONS	8

int next_connection_id = 0;	/* lifetime unique connection id */
int next_connection_slot = 0;	/* next slot to look at for new connection */
struct connection {
	int active;		/* an active entry cannot be reused */
	int id;			/* connection identifier */
	int menuactive;		/* dont run the serial pump on a menu */
	HANDLE menuThread;
	int netconnected;
	SOCKET net;
	HANDLE netThread;
	int serialconnected;
	HANDLE serial;
	HANDLE serialThread;
	int option_echo;	/* will we echo chars recieved? */
	int net_bytes_rx;
	int net_bytes_tx;
};
struct connection connection[MAXCONNECTIONS];

/* 
 * output from OutputDebugStringA can be seen using sysinternals debugview
 * http://technet.microsoft.com/en-us/sysinternals/bb896647.aspx
 */

/*
 * log a debug message
 */
int dprintf_level = 1;
int dprintf(unsigned char severity, const char *fmt, ...) {
	va_list args;
	char buf[MAXLEN];
	int i;

	if (severity > dprintf_level)
		return 0;

	va_start(args,fmt);
	i=vsnprintf(buf,sizeof(buf),fmt,args);
	va_end(args);

	if (debug_mode) {
		printf("%s",buf);
	} else {
		OutputDebugStringA(buf);
	}

	return i;
}

/*
 * format a string and send it to a net connection
 */
int netprintf(struct connection *conn, const char *fmt, ...) {
	va_list args;
	char buf[MAXLEN];
	int i;
	int bytes;

	va_start(args,fmt);
	i=vsnprintf(buf,sizeof(buf),fmt,args);
	va_end(args);

	bytes = send(conn->net,buf,(i>MAXLEN)?MAXLEN-1:i,0);

	if (bytes==-1) {
		dprintf(1,"wconsd[%i]: netprintf: send error %i\n",conn->id,GetLastError());
	} else {
		conn->net_bytes_tx += bytes;
	}

	return i;
}

/* open the com port */
DWORD open_com_port(struct connection *conn, DWORD *specificError) {
	/* Open the COM port */
	char portstr[12];

	if (conn->serialconnected) {
		dprintf(1,"wcons[%i]: open_com_port: serialconnected\n",conn->id);
	}

	sprintf(portstr, "\\\\.\\COM%d", com_port);
	conn->serial = CreateFile(portstr,
		GENERIC_READ | GENERIC_WRITE,
		0, // Exclusive access
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,
		NULL);
	if (conn->serial == INVALID_HANDLE_VALUE) {
		*specificError=GetLastError();
		return 13;
	}

	if (!GetCommState(conn->serial, &dcb)) {
		*specificError=GetLastError();
		return 14;
	}

	// Fill in the device control block
	dcb.BaudRate=com_speed;
	dcb.ByteSize=com_data;
	dcb.Parity=com_parity;		// NOPARITY, ODDPARITY, EVENPARITY
	dcb.StopBits=com_stop;		// ONESTOPBIT, ONE5STOPBITS, TWOSTOPBITS
	dcb.fBinary=TRUE;
	dcb.fOutxCtsFlow=FALSE;
	dcb.fOutxDsrFlow=FALSE;
	dcb.fDtrControl=DTR_CONTROL_ENABLE; // Always on
	dcb.fDsrSensitivity=FALSE;
	dcb.fTXContinueOnXoff=FALSE;
	dcb.fOutX=FALSE;
	dcb.fInX=FALSE;
	dcb.fErrorChar=FALSE;
	dcb.fNull=FALSE;
	dcb.fRtsControl=RTS_CONTROL_ENABLE; // Always on
	dcb.fAbortOnError=FALSE;

	if (!SetCommState(conn->serial, &dcb)) {
		*specificError=GetLastError();
		return 15;
	}

	timeouts.ReadIntervalTimeout=20;
	timeouts.ReadTotalTimeoutMultiplier=0;
	timeouts.ReadTotalTimeoutConstant=2000;
	timeouts.WriteTotalTimeoutMultiplier=0;
	timeouts.WriteTotalTimeoutConstant=0;
	if (!SetCommTimeouts(conn->serial, &timeouts)) {
		*specificError=GetLastError();
		return 16;
	}
	conn->serialconnected=1;
	return 0;
}

/* close the com port */
void close_com_port(struct connection *conn) {
	CloseHandle(conn->serial);
	conn->serialconnected=0;
}


/* Initialise wconsd: open a listening socket and the COM port, and
 * create lots of event objects. */
DWORD wconsd_init(DWORD argc, LPSTR *argv, DWORD *specificError)
{
	struct sockaddr_in sin;
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;

	/* Start up sockets */
	wVersionRequested = MAKEWORD( 2, 2 );
	
	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 ) {
		*specificError=err;
		/* Tell the user that we could not find a usable */
		/* WinSock DLL.                                  */
		return 1;
	}
	
	/* Confirm that the WinSock DLL supports 2.2.*/
	/* Note that if the DLL supports versions greater    */
	/* than 2.2 in addition to 2.2, it will still return */
	/* 2.2 in wVersion since that is the version we      */
	/* requested.                                        */
	
	if ( LOBYTE( wsaData.wVersion ) != 2 ||
        HIBYTE( wsaData.wVersion ) != 2 ) {
		/* Tell the user that we could not find a usable */
		/* WinSock DLL.                                  */
		WSACleanup( );
		return 2;
	}

	/* The WinSock DLL is acceptable. Proceed. */

	// Create the event object used to signal service shutdown
	stopEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (stopEvent==NULL) {
		*specificError=GetLastError();
		return 3;
	}
	// Event objects for overlapped IO
	readEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (readEvent==NULL) {
		*specificError=GetLastError();
		return 6;
	}
	writeEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (writeEvent==NULL) {
		*specificError=GetLastError();
		return 7;
	}
	// Create the event object for socket operations
	listenSocketEvent = WSACreateEvent();
	if (listenSocketEvent==WSA_INVALID_EVENT) {
		*specificError=WSAGetLastError();
		return 8;
	}

	/* Create a socket to listen for connections. */
	memset(&sin,0,sizeof(sin));
	sin.sin_family=AF_INET;
	sin.sin_port=htons(default_tcpport);
	ls=socket(AF_INET,SOCK_STREAM,0);
	if (ls==INVALID_SOCKET) {
		*specificError=WSAGetLastError();
		return 9;
	}
#ifndef MS_WINDOWS
	{
	int one=1;
	setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,(void*)&one,sizeof(one));
	}
#endif
	if (bind(ls,(struct sockaddr *)&sin,sizeof(sin))==SOCKET_ERROR) {
		*specificError=WSAGetLastError();
		dprintf(1,"wconsd: wconsd_init: failed to bind socket\n");
		return 10;
	}
	if (listen(ls,1)==SOCKET_ERROR) {
		*specificError=WSAGetLastError();
		return 11;
	}

	/* Mark the socket as non-blocking */
	if (WSAEventSelect(ls,listenSocketEvent,FD_ACCEPT)==SOCKET_ERROR) {
		*specificError=WSAGetLastError();
		return 12;
	}
	return 0;
}

/*
 * given a buffer starting with 0xff, process the telnet options and return
 * the number of bytes to skip
 */
int process_telnet_option(struct connection*conn, unsigned char *buf) {

	switch (buf[1]) {
		case 0xf0:	/* suboption end */
		case 241:	/* NOP */
		case 242:	/* Data Mark */
			dprintf(1,"wconsd[%i]: option IAC %i\n",conn->id,buf[1]);
			return 2;
		case 243:	/* Break */
			/* TODO */
			dprintf(1,"wconsd[%i]: break not supported\n",conn->id);
			return 2;
		case 244:	/* Interrupt */
			dprintf(1,"wconsd[%i]: option IAC Interrupt\n",conn->id,buf[1]);
			conn->menuactive=1;
			return 2;
		case 245:	/* abort output */
			dprintf(1,"wconsd[%i]: option IAC %i\n",conn->id,buf[1]);
			return 2;
		case 246:	/* are you there */
			dprintf(1,"wconsd[%i]: option IAC AYT\n",conn->id);
			netprintf(conn,"yes\r\n");
			return 2;
		case 247:	/* erase character */
		case 248:	/* erase line */
		case 249:	/* go ahead */
			dprintf(1,"wconsd[%i]: option IAC %i\n",conn->id,buf[1]);
			return 2;
		case 0xfa:	/* suboption begin */
			if (buf[3]==0) {
				/*
				 * I dont expect us to get any IS statements ...
				 * and if we do, I'm going to need to rewrite the
				 * way chars are absorbed
				 */
				dprintf(1,"wconsd[%i]: option IAC SB %i IS\n",conn->id,buf[2]);
				return 4;
			} else if (buf[3]>1) {
				dprintf(1,"wconsd[%i]: option IAC SB %i error\n",conn->id,buf[2]);
				return 4;
			}
			dprintf(1,"wconsd[%i]: option IAC SB %i SEND\n",conn->id,buf[2]);
			switch (buf[2]) {
				case 5:	/* STATUS */
					netprintf(conn,"%s%c%s%s%s",
						"\xff\xfa\x05",
						0,
						"\xfb\x05",
						conn->option_echo?"\xfb\x01":"",
						"\xff\xf0");
			}
			return 6;
		case 0xfb:	/* WILL */
			dprintf(1,"wconsd[%i]: option IAC WILL %i\n",conn->id,buf[2]);
			return 3;
		case 0xfc:	/* WONT */
			dprintf(1,"wconsd[%i]: option IAC WONT %i\n",conn->id,buf[2]);
			return 3;
		case 0xfd:	/* DO */
			switch (buf[2]) {
				case 0x01:	/* ECHO */
					dprintf(1,"wconsd[%i]: DO ECHO\n",conn->id);
					conn->option_echo=1;
					break;
				case 0x03:	/* suppress go ahead */
					dprintf(1,"wconsd[%i]: DO suppress go ahead\n",conn->id);
					break;
			}
			return 3;
		case 0xfe:	/* DONT */
			switch (buf[2]) {
				case 0x01:	/* ECHO */
					dprintf(1,"wconsd[%i]: DONT ECHO\n",conn->id);
					conn->option_echo=0;
					break;
				default:
					dprintf(1,"wconsd[%i]: option IAC DONT %i\n",conn->id,buf[2]);
			}
			return 3;
		case 0xff:	/* send ff */
			return 1;
		default:
			dprintf(1,"wconsd[%i]: option IAC %i %i\n",conn->id,buf[1],buf[2]);
	}

	/* not normally reached */
	return 3;
}

/*
 * Wrap up all the crazy file writing process in a function
 */
int serial_writefile(struct connection *conn,OVERLAPPED *o,unsigned char *buf,int size) {
	DWORD wsize;

	if (!conn->serialconnected) {
		dprintf(1,"wconsd[%i]: serial_writefile but serial closed\n",conn->id);
		return 0;
	}

	if (!WriteFile(conn->serial,buf,size,&wsize,o)) {
		if (GetLastError()==ERROR_IO_PENDING) {
			// Wait for it...
			if (!GetOverlappedResult(conn->serial,o,&wsize,TRUE)) {
				dprintf(1,"wconsd[%i]: Error %d (overlapped) writing to COM port\n",conn->id,GetLastError());
			}
		} else {
			dprintf(1,"wconsd[%i]: Error %d writing to COM port\n",conn->id,GetLastError());
		}
	}
	if (wsize!=size) {
		dprintf(1,"wconsd[%i]: Eeek! WriteFile: wrote %d of %d\n",conn->id,wsize,size);
	}
	return wsize;
}

DWORD WINAPI wconsd_net_to_com(LPVOID lpParam)
{
	struct connection * conn = (struct connection*)lpParam;
	unsigned char buf[BUFSIZE];
	unsigned char *pbuf;
	int bytes_to_scan;
	DWORD size;
	unsigned long zero=0;
	fd_set s;
	struct timeval tv;
	OVERLAPPED o={0};

	dprintf(1,"wconsd[%i]: start wconsd_net_to_com\n",conn->id);

	o.hEvent = writeEvent;
	while (conn->netconnected) {
		/* There's a bug in some versions of Windows which leads
		 * to recv() returning -1 and indicating error WSAEWOULDBLOCK,
		 * even on a blocking socket. This select() is here to work
		 * around that bug. */
		/*
		 * These sockets are non-blocking, so I am unsure that the
		 * above statement is still true
		 */
		FD_SET(conn->net,&s);
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		select(0,&s,NULL,NULL,&tv);
		size=recv(conn->net,(void*)&buf,BUFSIZE,0);
		if (size==0) {
			closesocket(conn->net);
			conn->net=INVALID_SOCKET;
			conn->netconnected=0;
			dprintf(1,"wconsd[%i]: wconsd_net_to_com size==0\n",conn->id);
			continue;
		}
		if (size==SOCKET_ERROR) {
			/* General paranoia about blocking sockets */
			ioctlsocket(conn->net,FIONBIO,&zero);
			continue;
		}
		conn->net_bytes_rx+=size;

		/*
		 * Scan for telnet options and process then remove them
		 * This loop is reasonably fast if there are no options,
		 * but recursively slow if there are options
		 */
		pbuf=buf;
		bytes_to_scan=size;
		while ((pbuf=memchr(pbuf,0xff,bytes_to_scan))!=NULL) {
			int skip = process_telnet_option(conn, pbuf);

			bytes_to_scan = size-(pbuf-buf)+skip;
			size -= skip;

			memmove(pbuf,pbuf+skip,bytes_to_scan);

			/*
			 * hack to skip quoted quotes
			 */
			if (skip==1) {
				pbuf++;
			}
		}

		/*
		 * Scan for CR NUL sequences and uncook them
		 * TODO - maybe implement a "cooked" mode to bypass this *
		 */
		pbuf=buf;
		bytes_to_scan=size;
		while ((pbuf=memchr(pbuf,0x0d,bytes_to_scan))!=NULL) {
			pbuf++;
			if (*pbuf!=0x00) {
				continue;
			}

			bytes_to_scan = size-(pbuf-buf)+1;
			size -= 1;
			memmove(pbuf,pbuf+1,bytes_to_scan);
		}

		/* TODO - emulate ciscos ctrl-^,x sequence for exit to menu */

		if (conn->menuactive) {
			/* TODO - if we are in menu mode, hook into the menu here */
			// dprintf(1,"wconsd[%i]: unexpected menuactive\n",conn->id);
			return 0;
		}

		if (!conn->serialconnected) {
			dprintf(1,"wconsd[%i]: data to send, but serial closed\n",conn->id);
			continue;
		}

		/*
		 * we could check the return value to see if there was a
		 * short write, but what would our options be?
		 */
		serial_writefile(conn,&o,buf,size);
	}
	dprintf(1,"wconsd[%i]: finish wconsd_net_to_com\n",conn->id);
	return 0;
}

DWORD WINAPI wconsd_com_to_net(LPVOID lpParam)
{
	struct connection * conn = (struct connection*)lpParam;
	unsigned char buf[BUFSIZE];
	DWORD size;
	OVERLAPPED o={0};

	o.hEvent=readEvent;

	dprintf(1,"wconsd[%i]: start wconsd_com_to_net\n",conn->id);

	while (conn->serialconnected && conn->netconnected) {
		if (!ReadFile(conn->serial,buf,BUFSIZE,&size,&o)) {
			if (GetLastError()==ERROR_IO_PENDING) {
				// Wait for overlapped operation to complete
				if (!GetOverlappedResult(conn->serial,&o,&size,TRUE)) {
					dprintf(1,"wconsd: Error %d (overlapped) reading from COM port\n",GetLastError());
				}
			} else {
				dprintf(1,"wconsd[%i]: Error %d reading from COM port\n",conn->id,GetLastError());
				conn->serialconnected=0;
				continue;
			}
		}
		/* We might not have any data if the ReadFile timed out */
		if (size>0) {
			if (!conn->netconnected) {
				dprintf(1,"wconsd[%i]: data to send, but net closed\n",conn->id);
				continue;
			}
			send(conn->net,(void*)&buf,size,0);
			conn->net_bytes_tx+=size;
		}
	}
	dprintf(1,"wconsd[%i]: finish wconsd_com_to_net\n",conn->id);
	return 0;
}

void send_help(struct connection *conn) {
	netprintf(conn,
		"\r\n"
		"NOTE: these commands will change in the next version\r\n\n"
		"available commands:\r\n\n"
		"  port, speed, data, parity, stop\r\n"
		"  help, status, copyright\r\n"
		"  open, close, autoclose\r\n"
		"  show_conn_table\r\n"
		"  quit\r\n");
}

void show_status(struct connection* conn) {
	/* print the status to the net connection */

	netprintf(conn, "status:\r\n\n"
			"  port=%d  speed=%d  data=%d  parity=%d  stop=%d\r\n\n",
			com_port, com_speed, com_data, com_parity, com_stop);

	if(conn->serialconnected) {
		netprintf(conn, "  state=open    autoclose=%d\r\n\n", com_autoclose);
	} else {
		netprintf(conn, "  state=closed  autoclose=%d\r\n\n", com_autoclose);
	}
}

/*
 * I thought that I would need this a lot, but it turns out that there
 * was a lot of duplicated code
 */
int check_atoi(char *p,int old_value,struct connection *conn,char *error) {
	if (!p) {
		netprintf(conn,error);
		return old_value;
	}

	return atoi(p);
}

int process_menu_line(struct connection*conn, char *line) {
	char *command;
	char *parameter1;

	/*
	 * FIXME - non re-entrant code
	 *
	 * my windows build environment does not have strtok_r
	 * and thus I am running these two strtok as close as possible.
	 * I am definitely not going to re-invent the wheel with my own
	 * code.
	 */
	command = strtok(line," ");
	parameter1 = strtok(NULL," ");

	if (!strcmp(command, "help") || !strcmp(command, "?")) {
		// help
		send_help(conn);
	} else if (!strcmp(command, "status")) {
		// status
		show_status(conn);
	} else if (!strcmp(command, "copyright")) {	// copyright
		netprintf(conn,
		"  Copyright (c) 2008 by Hamish Coleman <hamish@zot.org>\r\n"
		"                2003 by Benjamin Schweizer <gopher at h07 dot org>\r\n"
		"                1998 by Stephen Early <Stephen.Early@cl.cam.ac.uk>\r\n"
		"\r\n"
		"\r\n"
		"  This program is free software; you can redistribute it and/or modify\r\n"
		"  it under the terms of the GNU General Public License as published by\r\n"
		"  the Free Software Foundation; either version 2 of the License, or\r\n"
		"  (at your option) any later version.\r\n"
		"\r\n"
		"  This program is distributed in the hope that it will be useful,\r\n"
		"  but WITHOUT ANY WARRANTY; without even the implied warranty of\r\n"
		"  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\r\n"
		"  GNU General Public License for more details.\r\n"
		"\r\n"
		"  You should have received a copy of the GNU General Public License\r\n"
		"  along with this program; if not, write to the Free Software\r\n"
		"  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.\r\n"
		"\n");
	} else if (!strcmp(command, "port")) {		// port
		int new = check_atoi(parameter1,com_port,conn,"must specify a port\r\n");

		if (new >= 1 && new <= 16) {
			com_port=new;
		}
	} else if (!strcmp(command, "speed")) {		// speed
		com_speed = check_atoi(parameter1,com_port,conn,"must specify a speed\r\n");
	} else if (!strcmp(command, "data")) {		// data
		if (!strcmp(parameter1, "5")) {
			com_data=5;
		} else if (!strcmp(parameter1, "6")) {
			com_data=6;
		} else if (!strcmp(parameter1, "7")) {
			com_data=7;
		} else if (!strcmp(parameter1, "8")) {
			com_data=8;
		}
		show_status(conn);
	} else if (!strcmp(command, "parity")) {	// parity
		if (!strcmp(parameter1, "no") || !strcmp(parameter1, "0")) {
			com_parity=NOPARITY;
		} else if (!strcmp(parameter1, "even") || !strcmp(parameter1, "2")) {
			com_parity=EVENPARITY;
		} else if (!strcmp(parameter1, "odd") || !strcmp(parameter1, "1")) {
			com_parity=ODDPARITY;
		} else if (!strcmp(parameter1, "mark")) {
			com_parity=MARKPARITY;
		} else if (!strcmp(parameter1, "space")) {
			com_parity=SPACEPARITY;
		}
		show_status(conn);
	} else if (!strcmp(command, "stop")) {
		if (!strcmp(parameter1, "one") || !strcmp(parameter1, "1")) {
			com_stop=ONESTOPBIT;
		} else if (!strcmp(parameter1, "one5") || !strcmp(parameter1, "1.5")) {
			com_stop=ONE5STOPBITS;
		} else if (!strcmp(parameter1, "two") || !strcmp(parameter1, "2")) {
			com_stop=TWOSTOPBITS;
		}
		show_status(conn);
	} else if (!strcmp(command, "open")) {		// open
		DWORD errcode;
		int new = check_atoi(parameter1,com_port,conn,"open default port\r\n");

		if (new >= 1 && new <= 16) {
			com_port=new;
		}
		if (conn->serialconnected) {
			/* port ist still open */
			netprintf(conn,"\r\n\n");
			/* signal to quit the menu */
			conn->menuactive=0;
			/* and return still running */
			return 1;
		}

		if (!open_com_port(conn,&errcode)) {
			netprintf(conn,"\r\n\n");
			// signal to quit the menu
			conn->menuactive=0;
			/* and return still running */
			return 1;
		} else {
			netprintf(conn,"error: cannot open port\r\n\n");
		}
	} else if (!strcmp(command, "close")) {			// close
		close_com_port(conn);
		netprintf(conn,"info: actual com port closed\r\n\n");
	} else if (!strcmp(command, "autoclose")) {		// autoclose
		if (!strcmp(parameter1, "true") || !strcmp(parameter1, "1") || !strcmp(parameter1, "yes")) {
			com_autoclose=TRUE;
		} else if (!strcmp(parameter1, "false") || !strcmp(parameter1, "0") || !strcmp(parameter1, "no")) {
			com_autoclose=FALSE;
		}
		show_status(conn);
	} else if (!strcmp(command, "quit")) {
		// quit the connection
		conn->menuactive=0;
		closesocket(conn->net);
		conn->net=INVALID_SOCKET;
		conn->netconnected=0;
		/* and return not running */
		return 0;
	} else if (!strcmp(command, "show_conn_table")) {
		int i;
		netprintf(conn,
				"slot A id M mThr N net  netTh S serial serialTh E netrx nettx\r\n");
		netprintf(conn,
				"---- - -- - ---- - ---- ----- - ------ -------- - ----- -----\r\n");
		for (i=0;i<MAXCONNECTIONS;i++) {
			netprintf(conn,
				"%-4i %i %2i %i %4i %i %4i %5i %i %6i %8i %i %5i %5i\r\n",
				i, connection[i].active, connection[i].id,
				connection[i].menuactive, connection[i].menuThread,
				connection[i].netconnected, connection[i].net, connection[i].netThread,
				connection[i].serialconnected, connection[i].serial, connection[i].serialThread,
				connection[i].option_echo,
				connection[i].net_bytes_rx, connection[i].net_bytes_tx
				);
		}
	} else {
		/* other, unknown commands */
		netprintf(conn,"debug: line='%s', command='%s'\r\n\n",line,command);
	}
	/* return still running */
	return 1;
}

int run_menu(struct connection * conn) {
	unsigned char buf[BUFSIZE+5];	/* ensure there is room for our kludge telnet options */
	unsigned char line[MAXLEN];
	DWORD size, linelen=0;
	WORD i;

	unsigned long zero=0;
	fd_set set_read;

	/* IAC WILL ECHO */
	/* IAC WILL suppress go ahead */
	/* IAC WILL status */
	netprintf(conn,"\xff\xfb\x01\xff\xfb\x03\xff\xfb\x05");

	netprintf(conn,"\r\nwconsd serial port server (version %s)\r\n\r\n",VERSION);
	send_help(conn);
	netprintf(conn,"> ");

	FD_ZERO(&set_read);
	while (conn->menuactive && conn->netconnected) {
		FD_SET(conn->net,&set_read);
		select(conn->net+1,&set_read,NULL,NULL,NULL);
		size=recv(conn->net,(void*)&buf,BUFSIZE,0);

		if (size==0) {
			closesocket(conn->net);
			conn->net=INVALID_SOCKET;
			conn->netconnected=0;
			return 0; /* signal running=0 */
		}
		if (size==SOCKET_ERROR) {
			dprintf(1,"wconsd[%i]: socket error\n",conn->id);
			/* General paranoia about blocking sockets */
			ioctlsocket(conn->net,FIONBIO,&zero);
			continue;
		}
		conn->net_bytes_rx+=size;

		for (i = 0; i < size; i++) {
			if (buf[i]==0) {
				// strange, I'm not expecting nul bytes !
				continue;
			} else if (buf[i] == 127 || buf[i]==8) {
				// backspace
				if (linelen > 0) {
					netprintf(conn,"\x08 \x08");
					linelen--;
				} else {
					/* if the linebuf is empty, ring the bell */
					netprintf(conn,"\x07");
				}
				continue;
			} else if (buf[i] == 0x0d || buf[i]==0x0a) {
				// detected cr or lf

				if (i+1<size && (buf[i+1]==0x0a || buf[i+1]==0)) {
					i++;
				}

				if (conn->option_echo)
					/* echo the endofline */
					netprintf(conn,"\r\n");

				if (linelen!=0) {
					int running;
					line[linelen]=0;	// ensure string is terminated

					running = process_menu_line(conn,(char*)line);
					if (!conn->menuactive) {
						/* exiting the menu.. */
						return running;
					}
				}

				netprintf(conn,"> ");
				linelen=0;
				continue;
			} else if (buf[i] <0x20) {
				/* ignore other ctrl chars */
				continue;
			} else if (buf[i]==0xff) {
				// telnet option packet

				/*
				 * some magic to ignore quoted quotes
				 * this will go away when we use net_to_com
				 * to give us our data buffer
				 */
				if (buf[i+1]==0xff) {
					i++;
					continue;
				}

				/*
				 * Note that we avoid accessing outside the buffer
				 * by ensuring that the buffer is larger than
				 * the largest recv
				 *
				 * this doesnt help us if there is a split buffer from
				 * the client
				 */
				/*
				 * our for loop is already performing one increment,
				 * so subtract one from the returned value
				 */
				i+=process_telnet_option(conn,&buf[i])-1;
				continue;
			} else {
				// other chars

				if (linelen < MAXLEN - 1) {
					line[linelen] = buf[i];
					linelen++;
					if (conn->option_echo) {
						netprintf(conn,"%c",buf[i]);	/* echo */
					}
				} else {
					netprintf(conn,"\x07"); /* linebuf full bell */
				}
				continue;
			}
		}
	}

	/* not reached */
	return 0;
}

DWORD WINAPI thread_new_connection(LPVOID lpParam) {
	struct connection * conn = (struct connection*)lpParam;

	while(conn->netconnected) {
		dprintf(1,"wconsd[%i]: top of menu thread running loop\n",conn->id);

		/* basically, if we have exited the net_to_com thread and have
		 * still got net connectivity, we are in the menu
		 */
		conn->menuactive=1;

		if ((conn->menuactive && conn->netconnected) || !conn->serialconnected) {
			/* run the menu to ask the user questions */
			run_menu(conn);
		}
		if (!conn->menuactive && conn->netconnected && conn->serialconnected) {
			/* they must have opened the com port, so start the threads */
			PurgeComm(conn->serial,PURGE_RXCLEAR|PURGE_RXABORT);
			conn->netThread=CreateThread(NULL,0,wconsd_net_to_com,conn,0,NULL);
			if (conn->serialThread==NULL) {
				/* we might already have a com_to_net thread */
				conn->serialThread=CreateThread(NULL,0,wconsd_com_to_net,conn,0,NULL);
			}

			WaitForSingleObject(conn->netThread,INFINITE);
			CloseHandle(conn->netThread);
			conn->netThread=NULL;

		}
	}

	/* cleanup */
	dprintf(1,"wconsd[%i]: connection closing\n",conn->id);

	/* TODO print bytecounts */
	/* maybe close file descriptors? */
	shutdown(conn->net,SD_BOTH);
	closesocket(conn->net);

	close_com_port(conn);
	WaitForSingleObject(conn->serialThread,INFINITE);
	CloseHandle(conn->serialThread);

	conn->active=0;

	/* TODO - who closes menuThread ? */
	return 0;
}

static void wconsd_main(void)
{
	HANDLE wait_array[2];
	BOOL run=TRUE;
	DWORD o;
	SOCKET as;
	unsigned long zero=0;

	struct sockaddr_in sa;
	int salen;

	int i;
	int count;

	/* clear out any bogus data in the connections table */
	for (i=0;i<MAXCONNECTIONS;i++) {
		connection[i].active = 0;
	}

	/* Main loop: wait for a connection, service it, repeat
	 * until signalled that the service is terminating */
	wait_array[0]=stopEvent;
	wait_array[1]=listenSocketEvent;

	while (run) {
		dprintf(1,"wconsd: top of wconsd_main run loop\n");

		o=WaitForMultipleObjects(2,wait_array,FALSE,INFINITE);

		switch (o-WAIT_OBJECT_0) {
		case 0: /* stopEvent */
			run=FALSE;
			ResetEvent(stopEvent);
			break;
		case 1: /* listenSocketEvent */
			/* There is an incoming connection */
			WSAResetEvent(listenSocketEvent);
			salen = sizeof(sa);
			as=accept(ls,(struct sockaddr*)&sa,&salen);

			if (as==INVALID_SOCKET) {
				break;
			}

/* getnameinfo does not appear to be supported in my windows build environment */
#ifdef GETNAMEINFO
			char buf[MAXLEN];
			if (!getnameinfo((struct sockaddr*)&sa,salen,buf,sizeof(buf),NULL,0,0)) {
#endif
				dprintf(1,"wconsd: new connection from %08x\n",
					htonl(sa.sin_addr.s_addr));
#ifdef GETNAMEINFO
			} else {
				dprintf(1,"wconsd: new connection from %s\n",
					&buf);
			}
#endif

			/* search for an empty connection slot */
			i=next_connection_slot%MAXCONNECTIONS;
			count=0;
			while(connection[i].active && count<MAXCONNECTIONS) {
				count++;
				i = (i+1)%MAXCONNECTIONS;
			}
			if (i==MAXCONNECTIONS) {
				dprintf(1,"wconsd: connection table overflow\n");
				/* FIXME - properly reject the incoming connection */
				/* for now, just close the socket */
				closesocket(as);
				break;
			}
			next_connection_slot = (next_connection_slot+1)%MAXCONNECTIONS;
			connection[i].active=1;	/* mark this entry busy */
			connection[i].id = next_connection_id++;
			connection[i].menuactive=1;	/* start in the menu */
			connection[i].menuThread=NULL;
			connection[i].netconnected=1;
			connection[i].net=as;
			connection[i].netThread=NULL;
			connection[i].serialconnected=0;
			connection[i].serial=INVALID_HANDLE_VALUE;
			connection[i].serialThread=NULL;
			connection[i].option_echo=0;
			connection[i].net_bytes_rx=0;
			connection[i].net_bytes_tx=0;

			dprintf(1,"wconsd[%i]: accepted new connection slot=%i\n",connection[i].id,i);


			/* we successfully accepted the connection */

			ioctlsocket(connection[i].net,FIONBIO,&zero);

			connection[i].menuThread = CreateThread(NULL,0,thread_new_connection,&connection[i],0,NULL);

			break;
		default:
			run=FALSE; // Stop the service - I want to get off!
			break;
		}
	}

	/* TODO - look through the connection table and close everything */

	closesocket(ls);
	WSACleanup();
}

VOID WINAPI MyServiceCtrlHandler(DWORD opcode)
{
	DWORD status;

	switch(opcode) {
	case SERVICE_CONTROL_STOP:
		wconsd_status.dwWin32ExitCode = 0;
		wconsd_status.dwCurrentState = SERVICE_STOP_PENDING;
		wconsd_status.dwCheckPoint = 0;
		wconsd_status.dwWaitHint = 0;

		if (!SetServiceStatus(wconsd_statusHandle, &wconsd_status)) {
			status = GetLastError();
			dprintf(1,"wconsd: SetServiceStatus error %ld\n",status);
		}

		SetEvent(stopEvent);
		break;

	case SERVICE_CONTROL_INTERROGATE:
		// fall through to send current status
		break;

	default:
		dprintf(1,"wconsd: unrecognised opcode %ld\n",opcode);
		break;
	}

	// Send current status
	if (!SetServiceStatus(wconsd_statusHandle, &wconsd_status)) {
		status = GetLastError();
		dprintf(1,"wconsd: SetServiceStatus error %ld\n",status);
	}
	return;
}

VOID WINAPI ServiceStart(DWORD argc, LPSTR *argv)
{
	DWORD status;
	DWORD specificError;

	wconsd_status.dwServiceType = SERVICE_WIN32;
	wconsd_status.dwCurrentState = SERVICE_START_PENDING;
	wconsd_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	wconsd_status.dwWin32ExitCode = 0;
	wconsd_status.dwServiceSpecificExitCode = 0;
	wconsd_status.dwCheckPoint = 0;
	wconsd_status.dwWaitHint = 0;
	wconsd_statusHandle = RegisterServiceCtrlHandler(TEXT("wconsd"),MyServiceCtrlHandler);

	if (wconsd_statusHandle == (SERVICE_STATUS_HANDLE)0) {
		dprintf(1,"wconsd: RegisterServiceCtrlHandler failed %d\n", GetLastError());
		return;
	}

	status = wconsd_init(argc, argv, &specificError);

	if (status != NO_ERROR) {
		wconsd_status.dwCurrentState = SERVICE_STOPPED;
		wconsd_status.dwCheckPoint = 0;
		wconsd_status.dwWaitHint = 0;
		wconsd_status.dwWin32ExitCode = status;
		wconsd_status.dwServiceSpecificExitCode = specificError;

		SetServiceStatus(wconsd_statusHandle, &wconsd_status);
		return;
	}

	/* Initialisation complete - report running status */
	wconsd_status.dwCurrentState = SERVICE_RUNNING;
	wconsd_status.dwCheckPoint = 0;
	wconsd_status.dwWaitHint = 0;

	if (!SetServiceStatus(wconsd_statusHandle, &wconsd_status)) {
		status = GetLastError();
		dprintf(1,"wconsd: SetServiceStatus error %ld\n",status);
	}

	wconsd_main();

	wconsd_status.dwCurrentState = SERVICE_STOPPED;
	wconsd_status.dwCheckPoint = 0;
	wconsd_status.dwWaitHint = 0;
	wconsd_status.dwWin32ExitCode = 0;
	wconsd_status.dwServiceSpecificExitCode = 0;

	SetServiceStatus(wconsd_statusHandle, &wconsd_status);

	return;
}

static void RegisterService(LPSTR path)
{
	SC_HANDLE schSCManager, schService;

	schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	schService = CreateService(
		schSCManager,
		TEXT("wconsd"),
		TEXT("wconsd - a serial port server"),
		SERVICE_ALL_ACCESS,
		SERVICE_WIN32_OWN_PROCESS,
		SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL,
		path,
		NULL, NULL, NULL, NULL, NULL);

	if (schService == NULL) {
		printf("CreateService failed\n");
	} else {
		printf("Created service 'wconsd', binary path %s\n",path);
		printf("You should now start the service using the service manager.\n");
	}
	CloseServiceHandle(schService);
}

static void RemoveService(void)
{
	SC_HANDLE schSCManager, schService;

	schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (schSCManager == NULL) {
		printf("Couldn't open service manager\n");
		return;
	}

	schService = OpenService(schSCManager, TEXT("wconsd"), DELETE);

	if (schService == NULL) {
		printf("Couldn't open wconsd service\n");
		return;
	}

	if (!DeleteService(schService)) {
		printf("Couldn't delete wconsd service\n");
		return;
	}

	printf("Deleted service 'wconsd'\n");

	CloseServiceHandle(schService);
}

static void usage(void)
{
	printf("Usage: wconsd [-i pathname | -r | -d]\n");
	printf("   -i pathname     install service 'wconsd'; pathname\n");
	printf("                   must be the full path to the binary\n");
	printf("   -r              remove service 'wconsd'\n");
	printf("   -d              run wconsd in debug mode (in the foreground)\n");
}

int main(int argc, char **argv)
{
	DWORD err;
	int console_application=0;
	SERVICE_TABLE_ENTRY DispatchTable[] =
	{
		{ "wconsd", ServiceStart },
		{ NULL, NULL }
	};

	// debug info for when I test this as a service
	dprintf(1,"wconsd: started with argc==%i\n",argc);

	if (argc==1 || argc==0) {

		// assume that our messages are going to the debug log
		debug_mode=0;

		// start by trying to run as a service
		if (StartServiceCtrlDispatcher(DispatchTable)==0) {
			err = GetLastError();
			dprintf(1,"wconsd: StartServiceCtrlDispatcher error = %d\n", err);

			if (err != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
				// any other error, assume fatal
				return 1;
			}
		}

		// fall through and try running as a command-line application
		console_application=1;
	}

	// We are running in debug mode (or any other command-line mode)
	debug_mode=1;

	dprintf(1,"wconsd: Serial Console server\n");

	if (argc>1) {
		if (strcmp(argv[1],"-i")==0) {
			// request service installation
			if (argc!=3) {
				usage();
				return 1;
			}
			RegisterService(argv[2]);
			return 0;
		} else if (strcmp(argv[1],"-r")==0) {
			// request service removal
			RemoveService();
			return 0;
		} else if (strcmp(argv[1],"-p")==0) {
			console_application=1;
			default_tcpport = atoi(argv[2]);
		} else if (strcmp(argv[1],"-d")==0) {
			console_application=1;
		} else {
			usage();
			return 1;
		}
	}

	dprintf(1,"wconsd: listen on port %i\n",default_tcpport);

	// if we have decided to run as a console app..
	if (console_application) {
		int r;
		dprintf(1,"wconsd: Console Application Mode (version %s)\n",VERSION);
		r=wconsd_init(argc,argv,&err);
		if (r!=0) {
			dprintf(1,"wconsd: wconsd_init failed, return code %d [%l]\n",r, err);
			return 1;
		}
		wconsd_main();
	}

	return 0;
}

