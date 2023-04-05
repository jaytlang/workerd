/* workerd 
 * (c) jay lang, 2022
 */

#include <sys/types.h>
#include <sys/time.h>

#include <dirent.h>
#include <err.h>
#include <event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "workerd.h"

__dead static void	parent_signal(int, short, void *);
__dead static void	usage(void);
static void		empty_directory(char *);

int		 debug = 0;
int		 verbose = 0;

__dead static void
parent_signal(int signal, short event, void *arg)
{
	log_writex(LOGTYPE_WARN, "clean shutdown (signal %d)", signal);
	exit(0);

	(void)event;
	(void)arg;
}

__dead static void
usage(void)
{
	fprintf(stderr, "usage: %s [-dhv]\n", __progname);
	exit(1);
}

static void
empty_directory(char *dir)
{
	struct dirent	*dp;
	DIR		*dirp;	

	dirp = opendir(dir);
	if (dirp == NULL)
		log_fatal("empty_directory: open directory %s", dir);

	while ((dp = readdir(dirp)) != NULL) {
		char *fpath;

		if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
			continue;

		if (asprintf(&fpath, "%s/%s", dir, dp->d_name) < 0)
			log_fatal("empty_directory: asprintf");

		if (unlink(fpath) < 0)
			log_fatal("empty_directory: unlink %s", fpath);

		free(fpath);
	}

	closedir(dirp);
}

int
main(int argc, char *argv[])
{
	struct proc	*parent, *frontend, *engine;
	int		 ch;	

	while ((ch = getopt(argc, argv, "dhv")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'v':
			verbose = 1;
			break;

		case 'h':
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 0) usage();
	else if (geteuid() != 0) errx(1, "need root privileges");

	empty_directory(DISKS);
	empty_directory(FRONTEND_MESSAGES);
	empty_directory(ENGINE_MESSAGES);
	empty_directory(WRITEBACK);

	parent = proc_new(PROC_PARENT);
	if (parent == NULL) err(1, "proc_new -> parent process");

	proc_handlesigev(parent, SIGEV_INT, parent_signal);
	proc_handlesigev(parent, SIGEV_TERM, parent_signal);
	proc_setuser(parent, USER);
	proc_setchroot(parent, "/var/empty");

	/* XXX: defer frontend privilege drop until after it launches,
	 * because we have to load privileged data e.g. tls context
	 */
	frontend = proc_new(PROC_FRONTEND);
	if (frontend == NULL) err(1, "proc_new -> frontend process");

	proc_handlesigev(frontend, SIGEV_INT, frontend_signal);
	proc_handlesigev(frontend, SIGEV_TERM, frontend_signal);

	engine = proc_new(PROC_ENGINE);
	if (engine == NULL) err(1, "proc_new -> engine process");

	proc_handlesigev(engine, SIGEV_INT, engine_signal);
	proc_handlesigev(engine, SIGEV_TERM, engine_signal);
	proc_setuser(engine, USER);

	log_init();
	log_writex(LOGTYPE_DEBUG, "verbose logging enabled");

	/* drop the solid rocket boosters... */
	if (!debug && daemon(0, 0) < 0) err(1, "daemonize");

	/* and fire the main engines */
	proc_startall(parent, frontend, engine);

	log_writex(LOGTYPE_MSG, "startup");

	if (pledge("stdio", NULL) < 0)
		log_fatal("pledge");

	myproc_listen(PROC_FRONTEND, nothing);
	myproc_listen(PROC_ENGINE, nothing);

	event_dispatch();

	/* never hit */
	return 0;
}
