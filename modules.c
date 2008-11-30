/*
 *
 *
 *
 */

#include <sys/time.h>
#include <string.h>
#include <stdlib.h>

#include "libcli/libcli.h"
#include "module.h"

/* head of the list of all modules */
static struct module_def *module_list;

/* register a new module - used by all other modules */
int register_module(struct module_def *module) {
	/* FIXME - not thread safe */
	module->next = module_list;
	module_list = module;
	return 0;
}

/*
 * Because I want a flexible, modular architecture,
 * I want to be able to register child commands from
 * wildly different locations, thus I need some way
 * to use the same parents.
 *
 * The 'best' way would be auto-vivification, but libcli
 * is not constructed to allow that.  A better way would
 * be for libcli to have a command lookup function.
 * It is also possible to just register the parent every
 * time it is needed, but that clutters up the libcli
 * namespace with duplicates
 *
 * for the moment, I am just going to create a list and
 * some accessor functions.
 *
 */
/* TODO - implement cli_command_lookup */
struct parent_def {
	struct parent_def *next;
	char *name;
	struct cli_command *cmd;
};
static struct parent_def *parent_list;

struct cli_command *lookup_parent(char *name) {
	struct parent_def *p = parent_list;
	while(p) {
		if (!strcmp(name,p->name)) {
			return p->cmd;
		}
		p=p->next;
	}
	return NULL;
}

int register_parent(char *name, struct cli_command *cmd) {
	if (lookup_parent(name)) {
		return -1;
	}

	struct parent_def *p = malloc(sizeof(struct parent_def));
	if (!p) {
		return -1;
	}
	p->name=name;
	p->cmd=cmd;

	/* FIXME - not thread safe */
	p->next = parent_list;
	parent_list = p;
	return 0;
}


/* command used for showing all modules config */
static int cmd_showrun(struct cli_def *cli, char *command, char *argv[], int argc) {
	cli_print(cli,"!");
	cli_print(cli,"! show run");
	cli_print(cli,"!");

	struct module_def *p = module_list;
	while (p) {
		cli_print(cli,"! module: %s",p->name);
		if(p->showrun) {
			p->showrun(cli);
		}
		cli_print(cli,"!");
		p=p->next;
	}
	return CLI_OK;
}

/* command used for showing the loaded modules list */
static int cmd_showmodules(struct cli_def *cli, char *command, char *argv[], int argc) {
	cli_print(cli, "Module list:");
	struct module_def *p = module_list;
	while (p) {
		cli_print(cli,"\t%s\t%s",p->name,p->desc);
		p=p->next;
	}
	return CLI_OK;
}

/* dump the parents table for debugging */
static int cmd_debugparents(struct cli_def *cli, char *command, char *argv[], int argc) {
	cli_print(cli, "registered parents list:");
	struct parent_def *p = parent_list;
	while (p) {
		cli_print(cli,"\t%s\t%p",p->name,p->cmd);
		p=p->next;
	}
	return CLI_OK;
}


/* show the config for this module */
static int this_showrun(struct cli_def *cli) {
	cli_print(cli, "!");
	return CLI_OK;
}

/* Our local module definition */
static struct module_def this_module = {
	.name = "modules",
	.desc = "Module handling library",
	.showrun = this_showrun,
};

/* initialise and register this module */
int modules_init(struct cli_def *cli) {

	/* create the well known parent prefixes */
	register_parent("show",
		cli_register_command(cli, NULL, "show", NULL, PRIVILEGE_UNPRIVILEGED,
		MODE_EXEC, "Show information about system"));
	register_parent("debug",
		cli_register_command(cli, NULL, "debug", NULL, PRIVILEGE_PRIVILEGED,
		MODE_EXEC, "Commands used for debugging"));

	/* register the commands from this module */
	cli_register_command(cli, lookup_parent("show"), "running-config", cmd_showrun,
		PRIVILEGE_PRIVILEGED, MODE_EXEC, "Current configuration");

	cli_register_command(cli, lookup_parent("show"), "modules", cmd_showmodules,
		PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "Registered modules");

	cli_register_command(cli, lookup_parent("debug"), "parents", cmd_debugparents,
		PRIVILEGE_PRIVILEGED, MODE_EXEC, "Internal parent cmd list");

	register_module(&this_module);
	return 0;
}

