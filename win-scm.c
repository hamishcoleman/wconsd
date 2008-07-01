/*
 * win-scm.c - windows service control manager functions
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

#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>

#include "scm.h"

/* Service status: our current status, and handle on service manager */
SERVICE_STATUS svcStatus;
SERVICE_STATUS_HANDLE svcHandle;

/* global pointer to our service definition */
struct servicedef *global_sd;

VOID WINAPI ServiceCtrlHandler(DWORD opcode) {
	svcStatus.dwWin32ExitCode = NO_ERROR;
	if (opcode == SERVICE_CONTROL_STOP) {
		svcStatus.dwCurrentState = SERVICE_STOP_PENDING;
		SetServiceStatus( svcHandle, &svcStatus );
		SetEvent(global_sd->stopEvent);
		return;
	}
	SetServiceStatus( svcHandle, &svcStatus );
}

VOID WINAPI ServiceMain(DWORD argc, LPSTR *argv)
{
	int err;
	struct servicedef *sd = global_sd;

	svcHandle = RegisterServiceCtrlHandler(sd->name,ServiceCtrlHandler);
	if (!svcHandle) {
		/* FIXME - use SvcReportEvent() */
		printf("RegisterServiceCtrlHandler failed %u\n", GetLastError());
		return;
	}

	svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	svcStatus.dwCurrentState = 0;
	svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	svcStatus.dwWin32ExitCode = NO_ERROR;
	svcStatus.dwServiceSpecificExitCode = 0;
	svcStatus.dwCheckPoint = 0;
	svcStatus.dwWaitHint = 5000;

	/* Report initial status to the SCM. */
	svcStatus.dwCurrentState = SERVICE_START_PENDING;
	SetServiceStatus( svcHandle, &svcStatus );

	/* Create the event object used to signal service shutdown */
	sd->stopEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
	if (sd->stopEvent==NULL) {
		svcStatus.dwCurrentState = SERVICE_STOPPED;
		SetServiceStatus( svcHandle, &svcStatus );
		return;
	}

	if ((err=sd->init(argc,argv))!=0) {
		svcStatus.dwCurrentState = SERVICE_STOPPED;
		svcStatus.dwWin32ExitCode = err;
		SetServiceStatus( svcHandle, &svcStatus );
		return;
	}

	svcStatus.dwCurrentState = SERVICE_RUNNING;
	svcStatus.dwWin32ExitCode = NO_ERROR;
	SetServiceStatus( svcHandle, &svcStatus );

	/* Loop until we are stopping */
	while(WaitForSingleObject(sd->stopEvent, 0)==WAIT_TIMEOUT) {
		err=sd->main(0);
	}

	svcStatus.dwCurrentState = SERVICE_STOPPED;
	svcStatus.dwWin32ExitCode = NO_ERROR;
	SetServiceStatus( svcHandle, &svcStatus );
	return;
}

int SCM_Start(struct servicedef *sd) {
	SERVICE_TABLE_ENTRY ServiceTable[] = {
		{ "", ServiceMain },
		{ NULL, NULL }
	};

	global_sd = sd;

	/* try to run as a service */
	if (StartServiceCtrlDispatcher(ServiceTable)==0) {
		int err = GetLastError();
		printf("StartServiceCtrlDispatcher error = %d\n", err);

		if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
			/* TODO - could run the console service from here */
			return SVC_CONSOLE;
		}

		/* any other error, assume fatal */
		return SVC_FAIL;
	}
	return SVC_OK;
}

int SCM_Install(struct servicedef *sd) {
	SC_HANDLE schSCManager, schService;

	char path[MAX_PATH];

	if( !GetModuleFileName( NULL, path, MAX_PATH ) ) {
		printf("Cannot install service (%u)\n", GetLastError());
		return -1;
	}

	schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	schService = CreateService(
		schSCManager,
		sd->name,
		sd->desc,
		SERVICE_ALL_ACCESS,
		SERVICE_WIN32_OWN_PROCESS,
		SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL,
		path,
		NULL, NULL, NULL, NULL, NULL);

	if (schService == NULL) {
		printf("CreateService failed\n");
		CloseServiceHandle(schService);
		return -1;
	} else {
		printf("Created service '%s', binary path %s\n",sd->name,path);
		printf("You should now start the service using the service manager.\n");
		CloseServiceHandle(schService);
		return 0;
	}
}

int SCM_Remove(struct servicedef *sd) {
	SC_HANDLE schSCManager, schService;

	schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (schSCManager == NULL) {
		printf("Couldn't open service manager\n");
		return -1;
	}

	schService = OpenService(schSCManager, sd->name, DELETE);
	if (schService == NULL) {
		printf("Couldn't open %s service\n",sd->name);
		return -1;
	}

	if (!DeleteService(schService)) {
		printf("Couldn't delete %s service\n",sd->name);
		return -1;
	}

	printf("Deleted service '%s'\n",sd->name);

	CloseServiceHandle(schService);
	return 0;
}

