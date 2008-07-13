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

#include "scm.h"

#define VERSION "0.2.6"

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

int   default_tcpport = 23;

/* TODO - these buffers are ugly and large */
char *hostname[BUFSIZE];
struct hostent *host_entry;

/* Service status: our current status, and handle on service manager */
SERVICE_STATUS wconsd_status;
SERVICE_STATUS_HANDLE wconsd_statusHandle;

int debug_mode = 0;

/* these match the official telnet codes */
#define TELNET_OPTION_SB	0xfa
#define TELNET_OPTION_WILL	0xfb
#define TELNET_OPTION_WONT	0xfc
#define TELNET_OPTION_DO	0xfd
#define TELNET_OPTION_DONT	0xfe
#define TELNET_OPTION_IAC	0xff

/* these are my local state-tracking codes */
#define TELNET_OPTION_SBXX	0xfa00	/* received IAC SB xx */

#define MAXCONNECTIONS	8

int next_connection_id = 1;	/* lifetime unique connection id */
int next_connection_slot = 0;	/* next slot to look at for new connection */
struct connection {
	int active;		/* an active entry cannot be reused */
	int id;			/* connection identifier */
	HANDLE menuThread;
	SOCKET net;
	int serialconnected;
	HANDLE serial;
	HANDLE serialThread;
	int option_runmenu;	/* are we at the menu? */
	int option_binary;	/* binary transmission requested */
	int option_echo;	/* will we echo chars received? */
	int option_keepalive;	/* will we send IAC NOPs all the time? */
	int net_bytes_rx;
	int net_bytes_tx;
	struct sockaddr *sa;
	int telnet_option;	/* Set to indicate option processing status */
	int telnet_option_param;/* saved parameters from telnet options */
};
struct connection connection[MAXCONNECTIONS];


int wconsd_init(int argc, char **argv);
int wconsd_main(int param1);
int wconsd_stop(int param1);
struct SCM_def sd = {
	"wconsd","wconsd - Telnet to Serial server",
	wconsd_init, wconsd_main, wconsd_stop
};

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
int open_com_port(struct connection *conn) {
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
		return -1;
	}

	if (!GetCommState(conn->serial, &dcb)) {
		return -1;
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
		return -1;
	}

	/* FIXME - these values need much more tuning */
	timeouts.ReadIntervalTimeout=20;
	timeouts.ReadTotalTimeoutMultiplier=0;
	/*
	 * Note that this means that the serial to net thread wakes
	 * each and ever 50 milliseconds
	 */
	timeouts.ReadTotalTimeoutConstant=50;
	timeouts.WriteTotalTimeoutMultiplier=0;
	timeouts.WriteTotalTimeoutConstant=0;
	if (!SetCommTimeouts(conn->serial, &timeouts)) {
		return -1;
	}
	conn->serialconnected=1;
	return 0;
}

/* close the com port */
void close_com_port(struct connection *conn) {
	CloseHandle(conn->serial);
	conn->serial=INVALID_HANDLE_VALUE;
	conn->serialconnected=0;
}

/*
 * Given an active connection, force close its
 * serial port. waiting for all relevant resources
 */
void close_serial_connection(struct connection *conn) {
	if (!conn->active) {
		dprintf(1,"wconsd: closing closed connection %i\n",conn->id);
		return;
	}
	conn->option_runmenu=1;
	close_com_port(conn);
	WaitForSingleObject(conn->serialThread,INFINITE);
	CloseHandle(conn->serialThread);
	conn->serialThread=NULL;
}

int wconsd_stop(int param1) {
	SetEvent(stopEvent);
	return 0;
}

/* Initialise wconsd: open a listening socket and the COM port, and
 * create lots of event objects. */
int wconsd_init(int argc, char **argv) {
	struct sockaddr_in sin;
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;

	/* Start up sockets */
	wVersionRequested = MAKEWORD( 2, 2 );
	
	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 ) {
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
		return 3;
	}
	// Event objects for overlapped IO
	readEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (readEvent==NULL) {
		return 6;
	}
	writeEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (writeEvent==NULL) {
		return 7;
	}
	// Create the event object for socket operations
	listenSocketEvent = WSACreateEvent();
	if (listenSocketEvent==WSA_INVALID_EVENT) {
		return 8;
	}

	if (gethostname((char *)hostname,sizeof(hostname))==SOCKET_ERROR) {
		return 1;
	}
	dprintf(1,"wconsd: Hostname is %s\n",hostname);

	host_entry=gethostbyname((char *)hostname);
	if (host_entry->h_addrtype==AF_INET) {
		dprintf(1,"wconsd: IP Address is %s\n",inet_ntoa (*(struct in_addr *)*host_entry->h_addr_list));
		/* FIXME - enumerate all the IP addresses from the list */
	} else {
		host_entry=0;
		return 0;
	}

	/* Create a socket to listen for connections. */
	memset(&sin,0,sizeof(sin));
	sin.sin_family=AF_INET;
	sin.sin_port=htons(default_tcpport);
	ls=socket(AF_INET,SOCK_STREAM,0);
	if (ls==INVALID_SOCKET) {
		return 9;
	}
#ifndef MS_WINDOWS
	{
	int one=1;
	setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,(void*)&one,sizeof(one));
	}
#endif
	if (bind(ls,(struct sockaddr *)&sin,sizeof(sin))==SOCKET_ERROR) {
		dprintf(1,"wconsd: wconsd_init: failed to bind socket\n");
		return 10;
	}
	if (listen(ls,1)==SOCKET_ERROR) {
		return 11;
	}
	dprintf(1,"wconsd: listening on port %i\n",default_tcpport);


	/* Mark the socket as non-blocking */
	if (WSAEventSelect(ls,listenSocketEvent,FD_ACCEPT)==SOCKET_ERROR) {
		return 12;
	}

	return 0;
}

/*
 * telnet option receiver state machine.
 * Called with the current char, it saves the intermediate state in the
 * conn struct.
 *
 * Returns 0 to indicate no echo of this char, or 0xff to indicate that the
 * char should be echoed.
 */
unsigned char  process_telnet_option(struct connection*conn, unsigned char ch) {
	dprintf(2,"wconsd[%i]: debug option (0x%02x) 0x%02x\n",conn->id,conn->telnet_option,ch);

	switch (conn->telnet_option) {
		case 0: /* received nothing */
			if (ch==TELNET_OPTION_IAC) {
				conn->telnet_option=ch;
				return 0; /* dont echo */
			}
			return 0xff; /* ECHO */

		case TELNET_OPTION_IAC:	/* recived IAC */
			switch(ch) {
				case 0xf0:	/* suboption end */
				case 0xf1:	/* NOP */
				case 0xf2:	/* Data Mark */
				case 0xf5:	/* abort output */
				case 0xf7:	/* erase character */
				case 0xf8:	/* erase line */
				case 0xf9:	/* go ahead */
					dprintf(1,"wconsd[%i]: option IAC %i\n",conn->id,ch);
					conn->telnet_option=0;
					return 0; /* dont echo */

				case 0xf3:	/* Break */
					if (conn->serialconnected) {
						dprintf(2,"wconsd[%i]: send break\n",conn->id);
						Sleep(1000);
						SetCommBreak(conn->serial);
						Sleep(1000);
						ClearCommBreak(conn->serial);
					}
					conn->telnet_option=0;
					return 0; /* dont echo */

				case 0xf4:	/* Interrupt */
					conn->option_runmenu=1;
					conn->telnet_option=0;
					return 0; /* dont echo */

				case 0xf6:	/* are you there */
					dprintf(1,"wconsd[%i]: option IAC AYT\n",conn->id);
					netprintf(conn,"yes\r\n");
					conn->telnet_option=0;
					return 0; /* dont echo */

				case TELNET_OPTION_SB:
				case TELNET_OPTION_WILL:
				case TELNET_OPTION_WONT:
				case TELNET_OPTION_DO:
				case TELNET_OPTION_DONT:
					conn->telnet_option=ch;
					return 0; /* dont echo */

				case 0xff:	/* send ff */
					conn->telnet_option=0;
					return ch; /* ECHO */

				default:
					dprintf(1,"wconsd[%i]: option IAC %i invalid\n",conn->id,ch);
					conn->telnet_option=0;
					return 0; /* dont echo */
			}

		case TELNET_OPTION_SB:	/* received IAC SB 	0xfa */
			conn->telnet_option=TELNET_OPTION_SBXX;
			conn->telnet_option_param=ch;
			return 0; /* dont echo */

		case TELNET_OPTION_WILL: /* received IAC WILL 	0xfb */
			dprintf(2,"wconsd[%i]: option IAC WILL %i\n",conn->id,ch);
			conn->telnet_option=0;
			return 0; /* dont echo */
		case TELNET_OPTION_WONT: /* received IAC WONT 	0xfc */
			dprintf(1,"wconsd[%i]: option IAC WONT %i\n",conn->id,ch);
			conn->telnet_option=0;
			return 0; /* dont echo */
		case TELNET_OPTION_DO: /* received IAC DO 	0xfd */
			switch (ch) {
				case 0x00:	/* Binary */
					dprintf(2,"wconsd[%i]: DO Binary\n",conn->id);
					conn->option_binary=1;
				case 0x01:	/* ECHO */
					dprintf(2,"wconsd[%i]: DO ECHO\n",conn->id);
					conn->option_echo=1;
					break;
				default:
					dprintf(2,"wconsd[%i]: option IAC DO %i\n",conn->id,ch);
					break;
			}
			conn->telnet_option=0;
			return 0; /* dont echo */
		case TELNET_OPTION_DONT: /* received IAC DONT	0xfe */
			switch (ch) {
				case 0x00:	/* Binary */
					dprintf(2,"wconsd[%i]: DONT Binary\n",conn->id);
					conn->option_binary=0;
				case 0x01:	/* ECHO */
					dprintf(1,"wconsd[%i]: DONT ECHO\n",conn->id);
					conn->option_echo=0;
					break;
				default:
					dprintf(2,"wconsd[%i]: option IAC DONT %i\n",conn->id,ch);
					break;
			}
			conn->telnet_option=0;
			return 0; /* dont echo */

		default:
			if (conn->telnet_option==TELNET_OPTION_SBXX) {
				/* received IAC SB x */
				int option = conn->telnet_option_param;

				if (ch==0) {
					/* IS */
					dprintf(1,"wconsd[%i]: option IAC SB %i IS\n",
						conn->id,option);
					/*
					 * TODO - stay in telnet option mode while
					 * absorbing the IS buffer
					 */
					conn->telnet_option=0;
					return 0; /* dont echo */
				} else if (ch>1) {
					/* error ? */
					dprintf(1,"wconsd[%i]: option IAC SB %i %i\n",
						conn->id,option,ch);
					conn->telnet_option=0;
					return 0; /* dont echo */
				}

				/* SEND */
				if (option == 5) {
					dprintf(1,"wconsd[%i]: option IAC SB 5 SEND\n",conn->id);
					/* FIXME - add option_binary */
					netprintf(conn,"%s%c%s%s%s",
						"\xff\xfa\x05",
						0,
						"\xfb\x05",
						conn->option_echo?"\xfb\x01":"",
						"\xff\xf0");
					conn->telnet_option=0;
					return 0; /* dont echo */
				} else {
					/* error ? */
					dprintf(1,"wconsd[%i]: option IAC SB %i %i\n",
						conn->id,option,ch);
					conn->telnet_option=0;
					return 0; /* dont echo */
				}
			}

			dprintf(1,"wconsd[%i]: invalid conn->telnet_option==%i \n",conn->id,conn->telnet_option);

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

	dprintf(1,"wconsd[%i]: debug: start wconsd_net_to_com\n",conn->id);

	o.hEvent = writeEvent;
	while (conn->serialconnected) {
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
		/* TODO - examine the retval for the select */
		size=recv(conn->net,(void*)&buf,BUFSIZE,0);
		if (size==0) {
			closesocket(conn->net);
			conn->net=INVALID_SOCKET;
			dprintf(1,"wconsd[%i]: wconsd_net_to_com size==0\n",conn->id);
			return 0;
		}
		if (size==SOCKET_ERROR) {
			int err = WSAGetLastError();
			switch (err) {
				case WSAEWOULDBLOCK:
					/* ignore */
					if (conn->option_keepalive) {
						netprintf(conn,"\xff\xf1");
					}
					continue;
				case WSAECONNRESET:
					closesocket(conn->net);
					conn->net=INVALID_SOCKET;
					return 0;
				default:
					dprintf(1,"wconsd[%i]: net_to_com socket error (%i)\n",conn->id,err);
					/* General paranoia about blocking sockets */
					ioctlsocket(conn->net,FIONBIO,&zero);
			}
			continue;
		}
		conn->net_bytes_rx+=size;

		/*
		 * Scan for telnet options and process then remove them
		 * This loop is reasonably fast if there are no options,
		 * but recursively slow if there are options
		 *
		 * TODO - if an option is mid packet, this could change
		 * the semantics of processing at the wrong point
		 */
		pbuf=buf;
		bytes_to_scan=size;
		while(bytes_to_scan--) {
			/* TODO - use ->telnet_option || 0xff to chose to call process_telnet_option */
			if(!process_telnet_option(conn,*pbuf)) {
				/* remove this byte from the buffer */
				memmove(pbuf,pbuf+1,bytes_to_scan);
				size--;
				continue;
			}
			pbuf++;
		}

		/*
		 * Scan for CR NUL sequences and uncook them
		 * it also appears that I need to uncook CR LF sequences
		 */
		if (!conn->option_binary) {
			pbuf=buf;
			bytes_to_scan=size;
			while ((pbuf=memchr(pbuf,0x0d,bytes_to_scan))!=NULL) {
				pbuf++;
				if (*pbuf!=0x00&&*pbuf!=0x0a) {
					continue;
				}

				bytes_to_scan = size-(pbuf-buf)+1;
				size -= 1;
				memmove(pbuf,pbuf+1,bytes_to_scan);
			}
			/* TODO - emulate cisco's ctrl-^,x sequence for exit to menu */
		}


		if (conn->option_runmenu) {
			/*
			 * If processing the telnet options has caused
			 * runmenu to be set, we exit the loop here
			 */
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
	dprintf(1,"wconsd[%i]: debug: finish wconsd_net_to_com\n",conn->id);
	return 0;
}

DWORD WINAPI wconsd_com_to_net(LPVOID lpParam)
{
	struct connection * conn = (struct connection*)lpParam;
	unsigned char buf[BUFSIZE];
	DWORD size;
	OVERLAPPED o={0};

	o.hEvent=readEvent;

	dprintf(1,"wconsd[%i]: debug: start wconsd_com_to_net\n",conn->id);

	while (conn->serialconnected) {
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
			if (send(conn->net,(void*)&buf,size,0)==-1) {
				dprintf(1,"wconsd[%i]: wconsd_com_to_net send failed\n",conn->id);
				return 0;
			}
			conn->net_bytes_tx+=size;
		}
	}
	dprintf(1,"wconsd[%i]: debug: finish wconsd_com_to_net\n",conn->id);
	return 0;
}

void cmd_open(struct connection *conn) {
	dprintf(1,"wconsd[%i]: debug: start cmd_open\n",conn->id);

	if (!conn->serialconnected) {
		if (open_com_port(conn)) {
			netprintf(conn,"error: cannot open port\r\n\n");
			return;
		}
	}

	netprintf(conn,"\r\n\n");

	PurgeComm(conn->serial,PURGE_RXCLEAR|PURGE_RXABORT);
	if (conn->serialThread==NULL) {
		/* we might already have a com_to_net thread */
		conn->serialThread=CreateThread(NULL,0,wconsd_com_to_net,conn,0,NULL);
	}

	conn->option_runmenu=0;
	wconsd_net_to_com(conn);
	conn->option_runmenu=1;
}

void send_help(struct connection *conn) {
	netprintf(conn,
		"NOTE: the commands will be changing in the next version\r\n"
		"\r\n"
		"available commands:\r\n"
		"\r\n"
		"binary          - toggle the binary comms mode\r\n"
		"close           - Stop serial communications\r\n"
		"copyright       - Print the copyright notice\r\n"
		"data            - Set number of data bits\r\n"
		"help            - This guff\r\n"
		"kill_conn       - Stop a given connection's serial communications\r\n"
		"keepalive       - toggle the generation of keepalive packets\r\n"
		"open            - Connect or resume communications with a serial port\r\n"
		"parity          - Set the serial parity\r\n"
		"port            - Set serial port number\r\n"
		"quit            - exit from this session\r\n"
		"show_conn_table - Show the connections table\r\n"
		"speed           - Set serial port speed\r\n"
		"status          - Show current serial port status\r\n"
		"stop            - Set number of stop bits\r\n"
		"\r\n"
		"see http://wob.zot.org/2/wiki/wconsd for more information\r\n"
		"\r\n");
}

void show_status(struct connection* conn) {
	/* print the status to the net connection */

	netprintf(conn, "status:\r\n\n"
			"  port=%d  speed=%d  data=%d  parity=%d  stop=%d\r\n",
			com_port, com_speed, com_data, com_parity, com_stop);

	if(conn->serialconnected) {
		netprintf(conn, "  state=open\r\n\n");
	} else {
		netprintf(conn, "  state=closed\r\n\n");
	}
	netprintf(conn,"  connectionid=%i  hostname=%s\r\n",conn->id,hostname);
	netprintf(conn,"  echo=%i  binary=%i  keepalive=%i\r\n",
		conn->option_echo,conn->option_binary,conn->option_keepalive);
	netprintf(conn,"\r\n");
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

void process_menu_line(struct connection*conn, char *line) {
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
		if (!parameter1) {
			netprintf(conn,"Please specify number of data bits {5,6,7,8}\r\n");
			return;
		}
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
		if (!parameter1) {
			netprintf(conn,"Please specify the parity {no,even,odd,mark,space}\r\n");
			return;
		}
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
		if (!parameter1) {
			netprintf(conn,"Please specify the number of stop bits {1,1.5,2}\r\n");
			return;
		}
		if (!strcmp(parameter1, "one") || !strcmp(parameter1, "1")) {
			com_stop=ONESTOPBIT;
		} else if (!strcmp(parameter1, "one5") || !strcmp(parameter1, "1.5")) {
			com_stop=ONE5STOPBITS;
		} else if (!strcmp(parameter1, "two") || !strcmp(parameter1, "2")) {
			com_stop=TWOSTOPBITS;
		}
		show_status(conn);
	} else if (!strcmp(command, "open")) {		// open
		int new = check_atoi(parameter1,com_port,conn,"Opening default port\r\n");

		if (new >= 1 && new <= 16) {
			com_port=new;
		}
		cmd_open(conn);
	} else if (!strcmp(command, "close")) {			// close
		close_serial_connection(conn);
		netprintf(conn,"info: actual com port closed\r\n\n");
	} else if (!strcmp(command, "quit")) {
		// quit the connection
		conn->option_runmenu=0;
		closesocket(conn->net);
		conn->net=INVALID_SOCKET;
		return;
	} else if (!strcmp(command, "keepalive")) {
		conn->option_keepalive=!conn->option_keepalive;
		return;
	} else if (!strcmp(command, "binary")) {
		conn->option_binary=!conn->option_binary;
		return;
	} else if (!strcmp(command, "show_conn_table")) {
		int i;
		netprintf(conn,
			"Flags: A - Active Slot, S - Serial active,\r\n"
			"       M - Run Menu, B - Binary transmission, E - Echo enabled,\r\n"
			"       K - Telnet Keepalives, * - This connection\r\n"
			"\r\n");
		netprintf(conn,
				"s flags  id mThr net  serial serialTh netrx nettx peer address\r\n");
		netprintf(conn,
				"- ------ -- ---- ---- ------ -------- ----- ----- ------------\r\n");
		for (i=0;i<MAXCONNECTIONS;i++) {
			netprintf(conn,"%i%c%c%c%c%c%c%c %2i %4i ",
				i,
				&connection[i]==conn?'*':' ',
				connection[i].active?'A':' ',
				connection[i].serialconnected?'S':' ',
				connection[i].option_runmenu?'M':' ',
				connection[i].option_binary?'B':' ',
				connection[i].option_echo?'E':' ',
				connection[i].option_keepalive?'K':' ',
				connection[i].id,

				connection[i].menuThread);
			netprintf(conn,"%4i ", connection[i].net);

			if (connection[i].serialconnected) {
				netprintf(conn,"%6i %8i ",
					connection[i].serial,
					connection[i].serialThread);
			} else {
				netprintf(conn,"                ");
			}
			netprintf(conn, "%5i %5i ",
				connection[i].net_bytes_rx,
				connection[i].net_bytes_tx);
			if (connection[i].sa) {
				/* FIXME - IPv4 Specific */
				netprintf(conn,"%s:%i",
					inet_ntoa(((struct sockaddr_in*)connection[i].sa)->sin_addr),
					htons(((struct sockaddr_in*)connection[i].sa)->sin_port));
			}
			netprintf(conn, "\r\n");
		}
	} else if (!strcmp(command, "kill_conn")) {
		int connid = check_atoi(parameter1,0,conn,"must specify a connection id\r\n");
		if (connid==0 || connid>next_connection_id) {
			netprintf(conn,"Connection ID %i out of range\r\n",connid);
			return;
		}

		int i=0;
		while(connection[i].id!=connid && i<MAXCONNECTIONS) {
			i++;
		}
		if (i>=MAXCONNECTIONS) {
			netprintf(conn,"Connection ID %i not found\r\n",connid);
			return;
		}
		netprintf(&connection[i],"Serial Connection Closed by Connection ID %i\r\n",conn->id);
		close_serial_connection(&connection[i]);
		netprintf(conn,"Connection ID %i serial port closed\r\n",connid);
	} else {
		/* other, unknown commands */
		netprintf(conn,"\r\nInvalid Command: '%s'\r\n\r\n",line);
	}
}

void show_prompt(struct connection *conn) {
	netprintf(conn,"%s> ",hostname);
}

void run_menu(struct connection * conn) {
	unsigned char buf[BUFSIZE+5];	/* ensure there is room for our kludge telnet options */
	unsigned char line[MAXLEN];
	DWORD size, linelen=0;
	WORD i;

	unsigned long zero=0;
	fd_set set_read;
	struct timeval tv;

	unsigned char last_ch;
	unsigned char ch;

	/* IAC WILL ECHO */
	/* IAC WILL suppress go ahead */
	/* IAC WILL status */
	/* IAC WONT linemode */
	netprintf(conn,"\xff\xfb\x01\xff\xfb\x03\xff\xfb\x05\xff\xfc\x22");

	netprintf(conn,"\r\nwconsd serial port server (version %s)\r\n\r\n",VERSION);
	send_help(conn);
	show_prompt(conn);

	FD_ZERO(&set_read);
	while (conn->option_runmenu) {
		FD_SET(conn->net,&set_read);
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		select(conn->net+1,&set_read,NULL,NULL,&tv);
		/* TODO - examine the retval for the select */
		size=recv(conn->net,(void*)&buf,BUFSIZE,0);

		if (size==0) {
			closesocket(conn->net);
			conn->net=INVALID_SOCKET;
			return;
		}
		if (size==SOCKET_ERROR) {
			int err = WSAGetLastError();
			switch (err) {
				case WSAEWOULDBLOCK:
					/* ignore */
					if (conn->option_keepalive) {
						netprintf(conn,"\xff\xf1");
					}
					continue;
				case WSAECONNRESET:
					closesocket(conn->net);
					conn->net=INVALID_SOCKET;
					return;
				default:
					dprintf(1,"wconsd[%i]: run_menu socket error (%i)\n",conn->id,WSAGetLastError());
					/* General paranoia about blocking sockets */
					ioctlsocket(conn->net,FIONBIO,&zero);
			}
			continue;
		}
		conn->net_bytes_rx+=size;

		for (i = 0; i < size; i++) {
			last_ch=ch;
			ch = buf[i];

			/* TODO - remove the second call to process_telnet_option */
			if (conn->telnet_option) {
				/* We are in the middle of handling an option */
				process_telnet_option(conn,ch);
				/*
				 * return value ignored, since we dont care to
				 * print any telnet option values in the menu.
				 */
				continue;
			}
			if (ch==0) {
				/*
				 * NULLs could occur as the second char in a
				 * CR NUL telnet sequence
				 */
				continue;
			} else if (ch==127 || ch==8) {
				// backspace
				if (linelen > 0) {
					netprintf(conn,"\x08 \x08");
					linelen--;
				} else {
					/* if the linebuf is empty, ring the bell */
					netprintf(conn,"\x07");
				}
				continue;
			} else if (ch==0x0d || ch==0x0a) {
				// detected cr or lf

				if (last_ch == 0x0d && ch==0x0a) {
					/* skip the second char in CR LF */
					continue;
				}

				if (conn->option_echo)
					/* echo the endofline */
					netprintf(conn,"\r\n");

				if (linelen!=0) {
					line[linelen]=0;	// ensure string is terminated

					process_menu_line(conn,(char*)line);
					if (!conn->option_runmenu) {
						/* exiting the menu.. */
						return;
					}
				}

				show_prompt(conn);
				linelen=0;
				continue;
			} else if (ch<0x20) {
				/* ignore other ctrl chars */
				continue;
			} else if (ch==TELNET_OPTION_IAC) {
				/* start a telnet option packet */
				process_telnet_option(conn,ch);
				/*
				 * no possible return values, since IAC is
				 * just the beginning of a telnet packet
				 */
				continue;
			} else {
				// other chars

				if (linelen < MAXLEN - 1) {
					line[linelen] = ch;
					linelen++;
					if (conn->option_echo) {
						netprintf(conn,"%c",ch);	/* echo */
					}
				} else {
					netprintf(conn,"\x07"); /* linebuf full bell */
				}
				continue;
			}
		}
	}

	/* not reached */
}

DWORD WINAPI thread_new_connection(LPVOID lpParam) {
	struct connection * conn = (struct connection*)lpParam;

	dprintf(1,"wconsd[%i]: debug: start thread_new_connection loop\n",conn->id);
	run_menu(conn);

	/* cleanup */
	dprintf(1,"wconsd[%i]: connection closing\n",conn->id);

	/* TODO print bytecounts */
	/* maybe close file descriptors? */
	shutdown(conn->net,SD_BOTH);
	closesocket(conn->net);

	close_serial_connection(conn);

	conn->active=0;

	/* TODO - who closes menuThread ? */
	return 0;
}

int wconsd_main(int param1)
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
		dprintf(1,"wconsd: debug: start wconsd_main loop\n");

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

			dprintf(1,"wconsd: new connection from %s\n",
					inet_ntoa(sa.sin_addr));

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
			connection[i].menuThread=NULL;
			connection[i].net=as;
			connection[i].serialconnected=0;
			connection[i].serial=INVALID_HANDLE_VALUE;
			connection[i].serialThread=NULL;
			connection[i].option_runmenu=1;	/* start in the menu */
			connection[i].option_binary=0;
			connection[i].option_echo=0;
			connection[i].option_keepalive=0;
			connection[i].net_bytes_rx=0;
			connection[i].net_bytes_tx=0;
			connection[i].telnet_option=0;
			connection[i].telnet_option_param=0;

			if (connection[i].sa) {
				/* Do lazy de-allocation so that the info is
				 * still visible to show conn table */
				free(connection[i].sa);
			}
			if ((connection[i].sa=malloc(salen))==NULL) {
				dprintf(1,"wconsd[%i]: malloc failed\n",
					connection[i].id);
			} else {
				memcpy(connection[i].sa,&sa,salen);
			}

			dprintf(1,"wconsd[%i]: accepted new connection in slot %i\n",connection[i].id,i);


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
	return 0;
}

static void usage(void)
{
	printf("Usage: wconsd [-i pathname | -r | -d | -p port ]\n");
	printf("Just start with no options to start server\n");
	printf("   -i              install service 'wconsd'\n");
	printf("   -r              remove service 'wconsd'\n");
	printf("   -d              run wconsd in foreground mode\n");
	printf("   -p port         listen on the given port in foreground mode\n");
}

int main(int argc, char **argv)
{
	DWORD err;
	int console_application=0;

	// debug info for when I test this as a service
	dprintf(1,"wconsd: started with argc==%i\n",argc);

	if (argc==1 || argc==0) {

		// assume that our messages are going to the debug log
		debug_mode=0;

		err = SCM_Start(&sd);
		if (err!=SVC_CONSOLE) {
			return 0;
		}

		// fall through and try running as a command-line application
		console_application=1;
	}

	// We are running in debug mode (or any other command-line mode)
	debug_mode=1;

	dprintf(1,"\n"
		"wconsd: Serial Console server (version %s)\n",VERSION);
	dprintf(1,
		"        (see http://wob.zot.org/2/wiki/wconsd for more info)\n\n");

	if (argc>1) {
		if (strcmp(argv[1],"-i")==0) {
			/* request service installation */
			char *path = SCM_Install(&sd);
			if (!path) {
				printf("Service installation failed\n");
				return 1;
			}
			printf("Service '%s' installed, binary path '%s'\n",sd.name,path);
			printf("You should now start the service using the service manager.\n");
			return 0;
		} else if (strcmp(argv[1],"-r")==0) {
			// request service removal
			if (SCM_Remove(&sd)==0) {
				printf("Deleted service '%s'\n",sd.name);
			} else {
				printf("Service removal failed\n");
				return 1;
			}
			return 0;
		} else if (strcmp(argv[1],"-p")==0) {
			console_application=1;
			if (argc>2) {
				default_tcpport = atoi(argv[2]);
			}
		} else if (strcmp(argv[1],"-d")==0) {
			console_application=1;
			if (argc>2) {
				dprintf_level = atoi(argv[2]);
			}
		} else {
			usage();
			return 1;
		}
	}

	// if we have decided to run as a console app..
	if (console_application) {
		int r;
		dprintf(1,"wconsd: Foreground mode\n");

		r=wconsd_init(argc,argv);
		if (r!=0) {
			dprintf(1,"wconsd: wconsd_init failed, return code %d\n",r);
			return 1;
		}
		wconsd_main(0);
	}

	return 0;
}

