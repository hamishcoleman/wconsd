/*
 *
 *
 *
 */

struct SCM_def {
	char *name;
	char *desc;
	int mode;	/* set to SVC_CONSOLE by the *-scm.c code */
	int (*init)(int, char **);
	int (*main)(int);
	int (*stop)(int);
};

int SCM_Start(struct SCM_def *);
char *SCM_Install(struct SCM_def *);
int SCM_Remove(struct SCM_def *);

#define SVC_OK		0
#define	SVC_FAIL	-1
#define SVC_CONSOLE	1

