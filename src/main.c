#include "commands.h"
#include "util.h"

#include <curl/curl.h>
#include <signal.h>
#include <string.h>

static int cmd_noop(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

static const struct {
	const char *name;
	int (*fn)(int, char **);
} commands[] = {
	{ "clean", cmd_clean },
	{ "smudge", cmd_smudge },
	{ "filter-process", cmd_filter_process },
	{ "install", cmd_install },
	{ "uninstall", cmd_uninstall },
	{ "track", cmd_track },
	{ "untrack", cmd_untrack },
	{ "version", cmd_version },
	{ "fetch", cmd_fetch },
	{ "checkout", cmd_checkout },
	{ "pull", cmd_pull },
	{ "push", cmd_push },
	{ "prune", cmd_prune },
	{ "ls-files", cmd_ls_files },
	{ "pre-push", cmd_pre_push },
	{ "status", cmd_status },
	{ "env", cmd_env },
	{ "lock", cmd_lock },
	{ "unlock", cmd_unlock },
	{ "locks", cmd_locks },
	{ "migrate", cmd_migrate },
	/* хуки от гошного git-lfs могут вызывать их; locking мы не поддерживаем */
	{ "post-checkout", cmd_noop },
	{ "post-commit", cmd_noop },
	{ "post-merge", cmd_noop },
};

static void usage(void)
{
	size_t i;

	fputs("usage: git lfs <command> [<args>]\n\ncommands:", stderr);
	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		if (strncmp(commands[i].name, "post-", 5))
			fprintf(stderr, " %s", commands[i].name);
	fputc('\n', stderr);
}

int main(int argc, char **argv)
{
	size_t i;

	signal(SIGPIPE, SIG_IGN);
	if (argc < 2) {
		usage();
		return 2;
	}
	if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))
		return cmd_version(argc - 1, argv + 1);
	if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "help")) {
		usage();
		return 0;
	}
	if (curl_global_init(CURL_GLOBAL_DEFAULT))
		die("curl_global_init failed");
	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		if (!strcmp(argv[1], commands[i].name))
			return commands[i].fn(argc - 1, argv + 1);
	warnf("'%s' is not supported by this git-lfs implementation", argv[1]);
	usage();
	return 2;
}
