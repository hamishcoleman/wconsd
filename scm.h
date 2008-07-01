/*
 *
 *
 *
 */

struct servicedef {
	unsigned char *name;
	unsigned char *desc;
	int (*init)(int, char **);
	int (*main)(int);
	HANDLE stopEvent;
};

int SCM_Start(struct servicedef *);
int SCM_Install(struct servicedef *);
int SCM_Remove(struct servicedef *);

#define SVC_OK		0
#define	SVC_FAIL	-1
#define SVC_CONSOLE	1

