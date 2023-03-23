#include <sys/types.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <stdlib.h>

#include "workerd.h"

#define TEST_TIMEOUT		30
#define TEST_POLL_INTERVAL	1
#define TEST_KEY		69420

static void	killtest(int, short, void *);
static void	bootpoll(int, short, void *);

static struct event     boottimer;
static struct event	endtimer;

static struct vm_interface vmi = { 0 };

int	debug = 1, verbose = 1;

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
		warnx("noticed vm online, test ok");

		/* delete this line to sanity check vm killall cleanup */
		vm_release(new);

		vm_killall();
		exit(0);

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
