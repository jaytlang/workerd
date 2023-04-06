#include <sys/types.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <event.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "workerd.h"

#define TEST_TIMEOUT		30
#define TEST_POLL_INTERVAL	1
#define TEST_KEY		69420

#define TEST_BUNDLENAME		"build.bundle"
#define TEST_BUNDLEMAXSIZE	10240

static void	print(uint32_t, char *);
static void	fail(uint32_t, char *);
static void	ackdone(uint32_t);

static void	killtest(int, short, void *);
static void	bootpoll(int, short, void *);

static struct event     boottimer;
static struct event	endtimer;

static struct vm_interface vmi = { .print = print, .signaldone = ackdone, .reporterror = fail };

static int	printed = 0;
int		debug = 1, verbose = 1;

int myproc() { return PROC_ENGINE; }

static void
print(uint32_t key, char *msg)
{
	if (key != TEST_KEY) errx(1, "got print request from unknown vm");

	printed = 1;
	warnx("from vm: %s", msg);

	vm_injectack(vm_fromkey(key));
}

static void
fail(uint32_t key, char *msg)
{
	if (key != TEST_KEY)
		errx(1, "got error from unknown vm");

	warnx("error callback: %s", msg);	
	vm_release(vm_fromkey(key));
	vm_killall();

	if (strstr(msg, "connection to vm terminated unexpectedly") == NULL)
		errx(1, "the error we got back doesn't look like a timeout");

	exit(0);
}

static void
ackdone(uint32_t key)
{
	if (key != TEST_KEY) errx(1, "got termination notification from unknown vm");

	vm_killall();
	errx(1, "vm which should have timed out reported done successfully");
}

static void
killtest(int fd, short event, void *arg)
{
	vm_killall();
	errx(1, "test maximum duration exceeded, exiting");

	(void)fd;
	(void)event;
	(void)arg;
}

static void
bootpoll(int fd, short event, void *arg)
{
	struct timeval	 tv;
	struct vm	*new;

	new = vm_claim(TEST_KEY, vmi);

	if (new != NULL) {
		char	 data[10240];
		size_t	 datasize;
		int	 fd;

		warnx("noticed vm online");

		if ((fd = open(TEST_BUNDLENAME, O_RDONLY)) < 0)
			err(1, "open %s", TEST_BUNDLENAME);

		if ((datasize = read(fd, data, TEST_BUNDLEMAXSIZE)) < 0)
			err(1, "read %s", TEST_BUNDLENAME);

		vm_injectfile(new, TEST_BUNDLENAME, data, datasize);
		close(fd);

	} else if (errno == EAGAIN) {
		warnx("poll...");
		tv.tv_sec = TEST_POLL_INTERVAL;
		tv.tv_usec = 0;
		evtimer_add(&boottimer, &tv);

	} else err(1, "vm_claim returned unexpected error");

	(void)fd;
	(void)event;
	(void)arg;
}

int
main()
{
	struct timeval tv;

	event_init();
	vm_init();

	tv.tv_sec = TEST_TIMEOUT;
	tv.tv_usec = 0;

	evtimer_set(&endtimer, killtest, NULL);
	evtimer_add(&endtimer, &tv);

	tv.tv_sec = TEST_POLL_INTERVAL;

	evtimer_set(&boottimer, bootpoll, NULL);
	evtimer_add(&boottimer, &tv);

	event_dispatch();

	/* never reached */
	return 1;
}
