/*
 * Test framework for win-svc
 *
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include "scm.h"

/*
 * log a debug message
 */
int dprintf_level = 1;
int dprintf_to_stdout = 0;
int dprintf(unsigned char severity, const char *fmt, ...) {
        va_list args;
        char buf[1024];
        int i;

        if (severity > dprintf_level)
                return 0;

        va_start(args,fmt);
        i=vsnprintf(buf,sizeof(buf),fmt,args);
        va_end(args);

        if (dprintf_to_stdout) {
                printf("%s",buf);
        } else {
                OutputDebugStringA(buf);
        }
        return i;
}
#define trace(s) \
	dprintf(1,"%s:%i(%s) %s\n",__FILE__,__LINE__,__FUNCTION__,s)

int svctest_init(int argc, char **argv);
int svctest_main(int param1);
int svctest_stop(int param1);

struct SCM_def sd = {
        .name = "svctest",
        .desc = "svctest - test win-scm",
        .init = svctest_init,
        .main = svctest_main,
        .stop = svctest_stop,
};

static int do_getopt(const int argc, char **argv) {
	static struct option long_options[] = {
		{"install", 1, 0, 'i'},
		{"remove", 0, 0, 'r'},
		{"debug", 2, 0, 'd'},
		{0,0,0,0}
	};

	while(1) {
		int c = getopt_long(argc,argv, "ird::",
			long_options,NULL);
		if (c==-1)
			break;

		switch(c) {
			case 'i': {
				/* request service installation */
				char *path = SCM_Install(&sd,optarg);
				if (!path) {
					printf("Service installation failed\n");
					return 2;
				}
				printf("Service '%s' installed, binary path '%s'\n",sd.name,path);
				printf("You should now start the service using the service manager.\n");
				return 1;
			}
			case 'r':
				// request service removal
				if (SCM_Remove(&sd)==0) {
					printf("Deleted service '%s'\n",sd.name);
				} else {
					printf("Service removal failed\n");
				}
				return 1;
		}
	}
	return 0;
}


int svctest_init(int argc, char **argv) {
	dprintf(1,"%s:%i(%s) argc==%i\n",__FILE__,__LINE__,__FUNCTION__,argc);
	int i;
	for (i=0;i<argc;i++) {
		dprintf(1,"%s:%i: argv==%s\n",__FILE__,__LINE__,argv[i]);
	}
	dprintf(1,"%s:%i(%s) sd.mode==%i\n",__FILE__,__LINE__,__FUNCTION__,sd.mode);

	if (do_getopt(argc,argv)) {
		/* do_getopt returns nonzero if we should not continue */
		return 1;
	}

	trace("return 0");
        return 0;
}

int run = 1;
int svctest_main(int param1) {
	while(run) {
		1;
		/* FIXME - sleep */
	}
	trace("return 0");
        return 0;
}

int svctest_stop(int param1) {
	run = 0;
	trace("return 0");
        return 0;
}

int main(int argc, char **argv)
{
	dprintf(1,"%s:%i: argc==%i\n",__FILE__,__LINE__,argc);
	char **arg = argv;
	while(*arg) {
		dprintf(1,"%s:%i: argv==%s\n",__FILE__,__LINE__,*arg);
		arg++;
	}

        if (SCM_Start(&sd,argc,argv)!=SVC_OK) {
		dprintf(1,"%s:%i: SCM_Start!=SVC_OK\n",__FILE__,__LINE__);
                return 1;
        }

	trace("return 0");
        return 0;
}

