#include <windows.h>
#include <stdio.h>
#include <setupapi.h>

int enumports_level1() {
	char *buf;
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
	char *buf;
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
	int bufsize;
	char *guid;

	HDEVINFO diset;

	int i=0;

	printf("\n\nGet something\n\n");	

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
	
		free(didatad);	
		i++;
	}

	SetupDiDestroyDeviceInfoList(diset);
	free(guid);

	return 0;
}

int main(int argc, char **argv) {

	enumports_level1();
	enumports_level2();
	m3();

	return 0;
}
