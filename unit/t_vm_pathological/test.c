#include <sys/types.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <event.h>
#include <stdlib.h>
#include <unistd.h>

#include "workerd.h"

#define TEST_TIMEOUT		90
#define TEST_POLL_INTERVAL	1
#define TEST_KEY		69420

#define TEST_SCRIPTNAME		"build.py"
#define TEST_SCRIPTMAXSIZE	10240

static void	print(uint32_t, char *);
static void	fail(uint32_t, char *);
static void	ackdone(uint32_t);

static void	killtest(int, short, void *);
static void	bootpoll(int, short, void *);

static struct event     boottimer;
static struct event	endtimer;

static struct vm_interface vmi = { .print = print, .signaldone = ackdone, .reporterror = fail };

static int	accepted = 0;
int		debug = 1, verbose = 1;

static void
print(uint32_t key, char *msg)
{
	if (key != TEST_KEY) errx(1, "got print request from unknown vm");

	warnx("from vm: %s", msg);
	vm_injectack(vm_fromkey(key));
}

static void
fail(uint32_t key, char *msg)
{
	if (key != TEST_KEY) errx(1, "got error from unknown vm");

	vm_killall();
	errx(1, "error callback: %s", msg);	
}

static void
ackdone(uint32_t key)
{
	warnx("finishing up...");
	if (key != TEST_KEY) errx(1, "got termination notification from unknown vm");

	vm_release(vm_fromkey(key));
	vm_killall();
	exit(0);
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

		if (++accepted > 1) {
			vm_killall();
			errx(1, "accepted connections from multiple vms, firewall rule not working");
		}

		if ((fd = open(TEST_SCRIPTNAME, O_RDONLY)) < 0)
			err(1, "open %s", TEST_SCRIPTNAME);

		if ((datasize = read(fd, data, TEST_SCRIPTMAXSIZE)) < 0)
			err(1, "read %s", TEST_SCRIPTNAME);

		vm_injectfile(new, TEST_SCRIPTNAME, data, datasize);
		close(fd);

	} else if (errno == EAGAIN) {
		warnx("poll...");

	} else err(1, "vm_claim returned unexpected error");

	tv.tv_sec = TEST_POLL_INTERVAL;
	tv.tv_usec = 0;
	evtimer_add(&boottimer, &tv);

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
