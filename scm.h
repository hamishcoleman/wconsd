/*
 *
 *
 *
 */

struct SCM_def {
	unsigned char *name;
	unsigned char *desc;
	int (*init)(int, char **);
	int (*main)(int);
	int (*stop)(int);
};

int SCM_Start(struct SCM_def *);
int SCM_Install(struct SCM_def *);
int SCM_Remove(struct SCM_def *);

#define SVC_OK		0
#define	SVC_FAIL	-1
#define SVC_CONSOLE	1

