/*
 *
 *
 *
 */

struct module_def {
	struct module_def *next;
	char *name;
	char *desc;
	int (*showrun)(struct cli_def *);
};

int register_module(struct module_def *);

int register_parent(char *, struct cli_command *);
struct cli_command *lookup_parent(char *);

