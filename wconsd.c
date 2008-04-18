/*
 * wconsd.c - serial port server service for Windows NT
 *
 * Copyright (c) 2003 Benjamin Schweizer <gopher at h07 dot org>
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
#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>

#define VERSION "0.1.1"

#define HEADER "wconsd VERSION a serial port server\r\n\r\n"
#define HELP "available commands:\r\n\n  port, speed, data, parity, stop\r\n  help, status, copyright\r\n  open, close, autoclose\r\n"

/* Size of buffers for send and receive */
#define BUFSIZE 1024
#define MAXLEN 1024

/* End of user-serviceable parts */

/* Sockets for listening and communicating */
SOCKET ls=INVALID_SOCKET,cs=INVALID_SOCKET;

/* Event objects */
HANDLE stopEvent, connectionCloseEvent, threadTermEvent;
HANDLE readEvent, writeEvent;
WSAEVENT listenSocketEvent;

/* COM port */
HANDLE hCom;
DCB dcb;
COMMTIMEOUTS timeouts;

/* Port default settings are here */
int   com_port=1;
DWORD com_speed=9600;
BYTE  com_data=8;
BYTE  com_parity=NOPARITY;
BYTE  com_stop=ONESTOPBIT;
BOOL  com_autoclose=TRUE;
BOOL  com_state=FALSE; // FALSE=closed,TRUE=open

#define TCPPORT 9600

/* Service status: our current status, and handle on service manager */
SERVICE_STATUS wconsd_status;
SERVICE_STATUS_HANDLE wconsd_statusHandle;

/* Code starts here */

/* 
 * Debug output can be seen using sysinternals debugview
 * http://technet.microsoft.com/en-us/sysinternals/bb896647.aspx
 */
VOID SvcDebugOut(LPSTR String, DWORD Status)
{
	CHAR Buffer[1024];
	if (strlen(String)<1000) {
		sprintf(Buffer, String, Status);
		OutputDebugStringA(Buffer);
	}
}

/* open the com port */
DWORD open_com_port(DWORD *specificError) {
	/* Open the COM port */
	char portstr[12];
	sprintf(portstr, "\\\\.\\COM%d", com_port);
	hCom = CreateFile(portstr,
		GENERIC_READ | GENERIC_WRITE,
		0, // Exclusive access
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,
		NULL);
	if (hCom == INVALID_HANDLE_VALUE) {
		*specificError=GetLastError();
		return 13;
	}

	if (!GetCommState(hCom, &dcb)) {
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

	if (!SetCommState(hCom, &dcb)) {
		*specificError=GetLastError();
		return 15;
	}

	timeouts.ReadIntervalTimeout=20;
	timeouts.ReadTotalTimeoutMultiplier=0;
	timeouts.ReadTotalTimeoutConstant=50;
	timeouts.WriteTotalTimeoutMultiplier=0;
	timeouts.WriteTotalTimeoutConstant=0;
	if (!SetCommTimeouts(hCom, &timeouts)) {
		*specificError=GetLastError();
		return 16;
	}
	com_state=TRUE;
	return 0;
}

/* close the com port */
void close_com_port() {
	CloseHandle(hCom);
	com_state=FALSE;
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
	// Create the event object used to signal connection close
	connectionCloseEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (connectionCloseEvent==NULL) {
		*specificError=GetLastError();
		return 4;
	}
	threadTermEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (threadTermEvent==NULL) {
		*specificError=GetLastError();
		return 5;
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
	sin.sin_port=htons(TCPPORT);
	ls=socket(AF_INET,SOCK_STREAM,0);
	if (ls==INVALID_SOCKET) {
		*specificError=WSAGetLastError();
		return 9;
	}
	if (bind(ls,(struct sockaddr *)&sin,sizeof(sin))==SOCKET_ERROR) {
		*specificError=WSAGetLastError();
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

DWORD WINAPI wconsd_net_to_com(LPVOID lpParam)
{
	BYTE buf[BUFSIZE];
	DWORD size,wsize;
	unsigned long zero=0;
	fd_set s;
	OVERLAPPED o={0};

	o.hEvent = writeEvent;
	while (WaitForSingleObject(threadTermEvent,0)!=WAIT_OBJECT_0) {
		/* There's a bug in some versions of Windows which leads
		 * to recv() returning -1 and indicating error WSAEWOULDBLOCK,
		 * even on a blocking socket. This select() is here to work
		 * around that bug. */
		FD_SET(cs,&s);
		select(0,&s,NULL,NULL,NULL);
		size=recv(cs,buf,BUFSIZE,0);
		if (size==0) {
			SetEvent(connectionCloseEvent);
			return 0;
		}
		if (size==SOCKET_ERROR) {
			/* General paranoia about blocking sockets */
			ioctlsocket(cs,FIONBIO,&zero);
		}
		if (size!=SOCKET_ERROR) {
			if (!WriteFile(hCom,buf,size,&wsize,&o)) {
				if (GetLastError()==ERROR_IO_PENDING) {
					// Wait for it...
					if (!GetOverlappedResult(hCom,&o,&wsize,TRUE)) {
						printf("Error %d (overlapped) writing to COM port\n",GetLastError());
					}
				} else {
					printf("Error %d writing to COM port\n",GetLastError());
				}
			}
			if (wsize!=size) {
				printf("Eeek! WriteFile: wrote %d of %d\n",wsize,size);
			}
		}
	}
	return 0;
}

DWORD WINAPI wconsd_com_to_net(LPVOID lpParam)
{
	BYTE buf[BUFSIZE];
	DWORD size;
	OVERLAPPED o={0};

	o.hEvent=readEvent;

	while (WaitForSingleObject(threadTermEvent,0)!=WAIT_OBJECT_0) {
		if (!ReadFile(hCom,buf,BUFSIZE,&size,&o)) {
			if (GetLastError()==ERROR_IO_PENDING) {
				// Wait for overlapped operation to complete
				if (!GetOverlappedResult(hCom,&o,&size,TRUE)) {
					printf("Error %d (overlapped) reading from COM port\n",GetLastError());
				}
			} else {
				printf("Error %d reading from COM port\n",GetLastError());
				SetEvent(connectionCloseEvent);
				return 0;
			}
		}
		if (size>0) {
			send(cs,buf,size,0);
		}
	}
	return 0;
}

int run_menu() {
	/* no comment */
	BYTE buf[BUFSIZE], msg[MAXLEN], line[MAXLEN], command[MAXLEN], parameter[MAXLEN];
	DWORD errcode;
	DWORD size, linelen=0;
	char *buf2split=0, *cmdsplit=0;
	BOOL menu=TRUE;
	WORD i;
	char *linep;

	unsigned long zero=0;
	
	send(cs,HEADER,strlen(HEADER),0);
	send(cs,HELP,strlen(HELP),0);
	send(cs,"> ",2,0);
	while (menu) {
		size=recv(cs,buf,BUFSIZE,0);
		if (size==0) {
			SetEvent(connectionCloseEvent);
			return 1;
		}
		if (size==SOCKET_ERROR) {
			/* General paranoia about blocking sockets */
			ioctlsocket(cs,FIONBIO,&zero);
		}
		if (size!=SOCKET_ERROR) {
			for (i = 0; i < size; i++) {
				if (buf[i] == 127) {		// backspace
					if (linelen > 0) {
						send(cs,"\x7f",1,0);
						linelen--;
					} else {
						send(cs,"\x07",1,0); // bell
					}
				} else if (buf[i] == 13) {	// cr
					send(cs,HEADER,strlen(HEADER),0);
					line[linelen]=32; // ensure that there is
					line[linelen+1]=0;    // at least one seperator
					linep=line;
					memcpy(command,line, strchr(linep, 32) - linep); // seperator
					memcpy(parameter,strchr(line, 32)+1, linep+linelen-strchr(line, 32)); // seperator
					command[strchr(line, 32) - linep]=0;
					if (linep+linelen-strchr(line, 32)-1 < 0) {
						parameter[0]=0;
					} else {
						parameter[linep+linelen-strchr(line, 32)-1]=0;
					}
					if (!strcmp(command, "help") || !strcmp(command, "?")) {				// help
						send(cs,HELP,strlen(HELP),0);
					} else if (!strcmp(command, "status")) {		// status
						sprintf(msg, "status:\r\n\n  port=%d\r\n  speed=%d\r\n  data=%d\r\n  parity=%d\r\n  stop=%d\r\n\n", com_port, com_speed, com_data, com_parity, com_stop);
						send(cs,msg,strlen(msg),0);
						if(com_state) {
							sprintf(msg, "  state=open\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						} else {
							sprintf(msg, "  state=closed\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						}
						send(cs,msg,strlen(msg),0);
					} else if (!strcmp(command, "copyright")) {	// copyright
						sprintf(msg, "  Copyright (c) 2003 by Benjamin Schweizer <gopher at h07 dot org>\r\n                1998 by Stephen Early <Stephen.Early@cl.cam.ac.uk>\r\n\r\n\r\n  This program is free software; you can redistribute it and/or modify\r\n  it under the terms of the GNU General Public License as published by\r\n  the Free Software Foundation; either version 2 of the License, or\r\n  (at your option) any later version.\r\n \r\n  This program is distributed in the hope that it will be useful,\r\n  but WITHOUT ANY WARRANTY; without even the implied warranty of\r\n  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\r\n  GNU General Public License for more details.\r\n \r\n  You should have received a copy of the GNU General Public License\r\n  along with this program; if not, write to the Free Software\r\n  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.\r\n\n");
						send(cs,msg,strlen(msg),0);

					} else if (!strcmp(command, "port")) {		// port
						if (atoi(parameter) >= 1 && atoi(parameter) <= 16) {
							com_port=atoi(parameter);
						}
						sprintf(msg, "status:\r\n\n  port=%d\r\n  speed=%d\r\n  data=%d\r\n  parity=%d\r\n  stop=%d\r\n\n", com_port, com_speed, com_data, com_parity, com_stop);
						send(cs,msg,strlen(msg),0);
						if(com_state) {
							sprintf(msg, "  state=open\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						} else {
							sprintf(msg, "  state=closed\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						}
						send(cs,msg,strlen(msg),0);
					} else if (!strcmp(command, "speed")) {		// speed
						if (atoi(parameter) > 0) {
							com_speed=atoi(parameter);
						}
						sprintf(msg, "status:\r\n\n  port=%d\r\n  speed=%d\r\n  data=%d\r\n  parity=%d\r\n  stop=%d\r\n\n", com_port, com_speed, com_data, com_parity, com_stop);
						send(cs,msg,strlen(msg),0);
						if(com_state) {
							sprintf(msg, "  state=open\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						} else {
							sprintf(msg, "  state=closed\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						}
						send(cs,msg,strlen(msg),0);
					} else if (!strcmp(command, "data")) {		// data
						if (!strcmp(parameter, "5")) {
							com_data=5;
						} else if (!strcmp(parameter, "6")) {
							com_data=6;
						} else if (!strcmp(parameter, "7")) {
							com_data=7;
						} else if (!strcmp(parameter, "8")) {
							com_data=8;
						}
						sprintf(msg, "status:\r\n\n  port=%d\r\n  speed=%d\r\n  data=%d\r\n  parity=%d\r\n  stop=%d\r\n\n", com_port, com_speed, com_data, com_parity, com_stop);
						send(cs,msg,strlen(msg),0);
						if(com_state) {
							sprintf(msg, "  state=open\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						} else {
							sprintf(msg, "  state=closed\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						}
						send(cs,msg,strlen(msg),0);
					} else if (!strcmp(command, "parity")) {	// parity
						if (!strcmp(parameter, "no") || !strcmp(parameter, "0")) {
							com_parity=NOPARITY;
						} else if (!strcmp(parameter, "even") || !strcmp(parameter, "2")) {
							com_parity=EVENPARITY;
						} else if (!strcmp(parameter, "odd") || !strcmp(parameter, "1")) {
							com_parity=ODDPARITY;
						} else if (!strcmp(parameter, "mark")) {
							com_parity=MARKPARITY;
						} else if (!strcmp(parameter, "space")) {
							com_parity=SPACEPARITY;
						}
						sprintf(msg, "status:\r\n\n  port=%d\r\n  speed=%d\r\n  data=%d\r\n  parity=%d\r\n  stop=%d\r\n\n", com_port, com_speed, com_data, com_parity, com_stop);
						send(cs,msg,strlen(msg),0);
						if(com_state) {
							sprintf(msg, "  state=open\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						} else {
							sprintf(msg, "  state=closed\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						}
						send(cs,msg,strlen(msg),0);
					} else if (!strcmp(command, "stop")) {
						if (!strcmp(parameter, "one") || !strcmp(parameter, "1")) {
							com_stop=ONESTOPBIT;
						} else if (!strcmp(parameter, "one5") || !strcmp(parameter, "1.5")) {
							com_stop=ONE5STOPBITS;
						} else if (!strcmp(parameter, "two") || !strcmp(parameter, "2")) {
							com_stop=TWOSTOPBITS;
						}
						sprintf(msg, "status:\r\n\n  port=%d\r\n  speed=%d\r\n  data=%d\r\n  parity=%d\r\n  stop=%d\r\n\n", com_port, com_speed, com_data, com_parity, com_stop);
						send(cs,msg,strlen(msg),0);
						if(com_state) {
							sprintf(msg, "  state=open\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						} else {
							sprintf(msg, "  state=closed\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						}
						send(cs,msg,strlen(msg),0);
					} else if (!strcmp(command, "open")) {		// open
						if (atoi(parameter) > 0) {	// optional port parameter
							com_port=atoi(parameter);
						}
						if (!com_state) {
							if (!open_com_port(&errcode)) {
								send(cs,"\r\n\f",3,0);
								menu=FALSE;
								return 0;
							} else {
								sprintf(msg, "error:\r\n\n  can't open port.\r\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
								send(cs,msg,strlen(msg),0);
							}
						} else {	// port ist still open
							send(cs,"\r\n\f",3,0);
							menu=FALSE;
							return 0;
						}
					} else if (!strcmp(command, "close")) {			// close
						close_com_port();
						sprintf(msg, "info:\r\n\n  actual com port closed.\r\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
						send(cs,msg,strlen(msg),0);
					} else if (!strcmp(command, "autoclose")) {		// autoclose
						if (!strcmp(parameter, "true") || !strcmp(parameter, "1") || !strcmp(parameter, "yes")) {
							com_autoclose=TRUE;
						} else if (!strcmp(parameter, "false") || !strcmp(parameter, "0") || !strcmp(parameter, "no")) {
							com_autoclose=FALSE;
						}
						sprintf(msg, "status:\r\n\n  port=%d\r\n  speed=%d\r\n  data=%d\r\n  parity=%d\r\n  stop=%d\r\n\n", com_port, com_speed, com_data, com_parity, com_stop);
						send(cs,msg,strlen(msg),0);
						if(com_state) {
							sprintf(msg, "  state=open\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						} else {
							sprintf(msg, "  state=closed\r\n  autoclose=%d\r\n\n\n\n\n\n\n\n\n", com_autoclose);
						}
						send(cs,msg,strlen(msg),0);
					} else {								// else
							sprintf(msg, "debug:\r\n\n  command: '%s'        \r\n  parameter: '%s'        \r\n", command, parameter);
							send(cs,msg,strlen(msg),0);
							sprintf(msg, "\r\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
							send(cs,msg,strlen(msg),0);
					}
					send(cs,"> ",2,0);
					linelen=0;
				} else {					// other chars
					if (linelen < MAXLEN - 1) {
						line[linelen] = buf[i];
						linelen++;
						send(cs,buf+i,1,0);
					} else {
						send(cs,"\x07",1,0); // bell
					}
				}
			}
		}
	}
	return 0;
}

static void wconsd_main(void)
{
	HANDLE wait_array[3];
	BOOL run=TRUE;
	DWORD o;
	SOCKET as;
	HANDLE netThread=NULL, comThread=NULL;
	long zero;

	/* Main loop: wait for a connection, service it, repeat
	 * until signalled that the service is terminating */
	wait_array[0]=stopEvent;
	wait_array[1]=listenSocketEvent;
	wait_array[2]=connectionCloseEvent;

	while (run) {
		o=WaitForMultipleObjects(3,wait_array,FALSE,INFINITE);

		switch (o-WAIT_OBJECT_0) {
		case 0:
			run=FALSE;
			ResetEvent(stopEvent);
			break;
		case 1: /* There is an incoming connection */
			WSAResetEvent(listenSocketEvent);
			as=accept(ls,NULL,NULL);

			if (as!=INVALID_SOCKET) {
				if (cs!=INVALID_SOCKET) {
					/* Close down the existing connection and let the new one through */
					SetEvent(threadTermEvent);
					shutdown(cs,SD_BOTH);
					WaitForSingleObject(netThread,INFINITE);
					WaitForSingleObject(comThread,INFINITE);
					CloseHandle(netThread);
					CloseHandle(comThread);
					closesocket(cs);
					ResetEvent(connectionCloseEvent);
					ResetEvent(threadTermEvent);
				}
				cs=as;
				zero=0;
				ioctlsocket(cs,FIONBIO,&zero);
				run_menu();
				PurgeComm(hCom,PURGE_RXCLEAR|PURGE_RXABORT);
				netThread=CreateThread(NULL,0,wconsd_net_to_com,NULL,0,NULL);
				comThread=CreateThread(NULL,0,wconsd_com_to_net,NULL,0,NULL);
			}
			break;
		case 2: /* The data connection has been broken */
			SetEvent(threadTermEvent);
			WaitForSingleObject(netThread,INFINITE);
			WaitForSingleObject(comThread,INFINITE);
			CloseHandle(netThread);
			CloseHandle(comThread);
			netThread=NULL;
			comThread=NULL;
			if (cs!=INVALID_SOCKET) {
				shutdown(cs,SD_BOTH);
				closesocket(cs);
				cs=INVALID_SOCKET;
			}
			ResetEvent(connectionCloseEvent);
			ResetEvent(threadTermEvent);
			if (com_autoclose) {
				close_com_port();
			}
			break;
		default:
			run=FALSE; // Stop the service - I want to get off!
			break;
		}
	}

	if (cs!=INVALID_SOCKET) {
		shutdown(cs,SD_BOTH);
		closesocket(cs);
	}

	if (netThread || comThread) {
		SetEvent(threadTermEvent);
		WaitForSingleObject(netThread,INFINITE);
		WaitForSingleObject(comThread,INFINITE);
		CloseHandle(netThread);
		CloseHandle(comThread);
	}

	CloseHandle(hCom);
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
			SvcDebugOut(" [wconsd] SetServiceStatus error %ld\n",status);
		}

		SetEvent(stopEvent);
		break;

	case SERVICE_CONTROL_INTERROGATE:
		// fall through to send current status
		break;

	default:
		SvcDebugOut(" [wconsd] unrecognised opcode %ld\n",opcode);
		break;
	}

	// Send current status
	if (!SetServiceStatus(wconsd_statusHandle, &wconsd_status)) {
		status = GetLastError();
		SvcDebugOut(" [wconsd] SetServiceStatus error %ld\n",status);
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
	wconsd_statusHandle = RegisterServiceCtrlHandler(TEXT("wconsd_com1[8n1,9k6] at 9600"),MyServiceCtrlHandler);

	if (wconsd_statusHandle == (SERVICE_STATUS_HANDLE)0) {
		SvcDebugOut(" [wconsd] RegisterServiceCtrlHandler failed %d\n", GetLastError());
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
		SvcDebugOut(" [wconsd] SetServiceStatus error %ld\n",status);
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
	SERVICE_TABLE_ENTRY DispatchTable[] =
	{
		{ "wconsd", ServiceStart },
		{ NULL, NULL }
	};

	if (argc==1 || argc==0) {
		if (!StartServiceCtrlDispatcher(DispatchTable)) {
			SvcDebugOut(" [wconsd] StartServiceCtrlDispatcher error = %d\n", GetLastError());
		}
		return 0;
	}

	if (strcmp(argv[1],"-i")==0) {
		if (argc!=3) {
			usage();
			return 1;
		}
		RegisterService(argv[2]);
	} else if (strcmp(argv[1],"-r")==0) {
		RemoveService();
	} else if (strcmp(argv[1],"-d")==0) {
		int r;
		printf("wconsd: running in debug mode\n");
		r=wconsd_init(argc,argv,&err);
		if (r!=0) {
			printf("wconsd: debug: init failed, return code %d [%s]\n",r, err);
			return 1;
		}
		wconsd_main();
		return 0;
	} else usage();
	
	return 1;
}
