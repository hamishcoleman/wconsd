/*
 *
 *
 *
 */

struct SCM_def {
	char *name;
	char *desc;
	int mode;			/* set to SVC_CONSOLE by the *-scm.c code */
	int (*init)(int, char **);	/* called before main */
	int (*main)(int);		/* called to run the service */
	int (*stop)(int);		/* called by scm to tell the service to stop */
};

int SCM_Start(struct SCM_def *, int argc, char **argv);
char *SCM_Install(struct SCM_def *);
int SCM_Remove(struct SCM_def *);

#define SVC_OK		0
#define	SVC_FAIL	-1
#define SVC_CONSOLE	1

