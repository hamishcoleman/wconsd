/*
 * enumerate windows serial ports

Resources:

http://www.lookrs232.com/com_port_programming/api_enumports.htm
http://forum.sysinternals.com/forum_posts.asp?TID=4544
http://www.naughter.com/enumser.html
http://msdn.microsoft.com/en-us/library/ms793116.aspx
http://msdn.microsoft.com/en-us/library/bb663174.aspx


HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM

	key = device driver name
	val = COMn name


HKLM \ SYSTEM \ CurrentControlSet\Enum\ * *
	PortName

http://msdn.microsoft.com/en-us/library/ms800601.aspx

 */

#include <windows.h>
#include <stdio.h>
#include <setupapi.h>

int enumports_level1() {
	unsigned char *buf;
	DWORD size=0;
	DWORD nrports=0;
	PORT_INFO_1 *pi1;

	printf("getting level 1 information\n\n");

	EnumPorts(NULL,1,NULL,0,&size,&nrports);
	/* size now contains the required number of bytes */

	if(!size) {
		printf("size==0!!\n");
		return 1;
	}

	buf=malloc(size);
	if (!buf) {
		printf("malloc fail\n");
		return 1;
	}

	if(!EnumPorts(NULL,1,buf,size,&size,&nrports)) {
		printf("enumports fail\n");
		free(buf);
		return 1;
	}

	printf("nrports=%lu\n",nrports);

	pi1 = (PORT_INFO_1*)buf;
	while(nrports) {
		printf("port = %s\n",pi1->pName);
		pi1++;
		nrports--;
	}

	free(buf);
	return 0;
}

int enumports_level2() {
	unsigned char *buf;
	DWORD size=0;
	DWORD nrports=0;
	PORT_INFO_2 *pi2;

	printf("\n\ngetting level 2 information\n\n");

	EnumPorts(NULL,2,NULL,0,&size,&nrports);

	if(!size) {
		printf("size==0!!\n");
		return 1;
	}

	buf=malloc(size);
	if (!buf) {
		printf("malloc fail\n");
		return 1;
	}

	if(!EnumPorts(NULL,2,buf,size,&size,&nrports)) {
		printf("enumports fail\n");
		free(buf);
		return 1;
	}
	printf("nrports=%lu\n",nrports);

	pi2 = (PORT_INFO_2*)buf;
	printf("%-10s %-15s %-15s %-4s %-8s\n",
		"Port","Monitor","Desc","Type","Reserved"
	);
	while(nrports) {
		printf("%-10s %-15s %-15s %4lu %8lu\n",
			pi2->pPortName, pi2->pMonitorName, pi2->pDescription,
			pi2->fPortType, pi2->Reserved
		);
		pi2++;
		nrports--;
	}

	free(buf);
	return 0;
}

int m3() {
	long unsigned int bufsize;
	GUID *guid;

	HDEVINFO diset;

	int i=0;

	printf("\n\nGet DiGetDeviceInterfaceDetail\n\n");

	SetupDiClassGuidsFromName("Ports",NULL,0,&bufsize);
	if(!bufsize){printf("size==0!!\n");return 1;}

	if (!(guid=malloc(bufsize))){printf("malloc fail\n");return 1;}
	if (!SetupDiClassGuidsFromName("Ports",guid,bufsize,&bufsize)) {
		printf("!SetupDiClassGuidsFromName\n");
		return 1;
	}

	if ((diset = SetupDiGetClassDevs(guid,NULL,0,DIGCF_DEVICEINTERFACE))==INVALID_HANDLE_VALUE) {
		printf("SetupDiGetClassDevs fail %lu\n",GetLastError());
		return 1;
	}

	while(1) {
		SP_DEVICE_INTERFACE_DATA didata;
		SP_DEVICE_INTERFACE_DETAIL_DATA *didatad;
		SP_DEVINFO_DATA dedata;
		/* Hello, I am a retardo windows interface */
		didata.cbSize=sizeof(didata);
		dedata.cbSize=sizeof(dedata);

		if (!SetupDiEnumDeviceInterfaces(diset,NULL,guid,i,&didata)) {
			/* end of list? */
			if (GetLastError()!=ERROR_NO_MORE_ITEMS) {
				printf("!SetupDiEnumDeviceInterfaces %lu\n",GetLastError());
			}
			break;
		}

		SetupDiGetDeviceInterfaceDetail(diset,&didata,NULL,0,&bufsize,NULL);
		if(!bufsize){printf("size==0!!\n");return 1;}

		if (!(didatad=malloc(bufsize))){printf("malloc fail\n");return 1;}
		/* GAH, stoopid interface */
		didatad->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		if (!SetupDiGetDeviceInterfaceDetail(diset,&didata,didatad,bufsize,&bufsize,&dedata)) {
			printf("!SetupDiGetDeviceInterfaceDetail %lu\n",GetLastError());
			return 1;
		}

		printf("DevicePath=%s\n",didatad->DevicePath);

//		reg = SetupDiOpenDevRegKey(diset,&didata,DICS_FLAG_GLOBAL,0,DIREG_DEV, KEY_ALL_ACCESS);

		printf("\n");
	
		free(didatad);	
		i++;
	}

	SetupDiDestroyDeviceInfoList(diset);
	free(guid);

	return 0;
}

int qdosdev() {
	static char buf[65535];
	char *p;

	printf("\n\nGet QueryDosDevice\n\n");

	if(QueryDosDevice(NULL, buf, sizeof(buf)) == 0) {
		printf("QueryDosDevice: failed\n");
		return 1;
	}

	p=(char *)&buf;
	while (*p) {
		p+=printf("%s\n",p);
	}
	return 0;
}

int main(int argc, char **argv) {

	//enumports_level1();
	enumports_level2();
	m3();
	qdosdev();

	return 0;
}
