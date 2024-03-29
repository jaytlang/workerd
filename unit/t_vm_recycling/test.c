#include <sys/types.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <event.h>
#include <stdlib.h>
#include <unistd.h>

#include "workerd.h"

#define TEST_TIMEOUT		600
#define TEST_POLL_INTERVAL	1

#define TEST_BUNDLENAME		"build.bundle"
#define TEST_BUNDLEMAXSIZE	10240
#define TEST_ROUNDS		5

static void	print(uint32_t, char *);
static void	fail(uint32_t, char *);
static void	ackdone(uint32_t);

static void	killtest(int, short, void *);
static void	bootpoll(int, short, void *);

static struct event     boottimer;
static struct event	endtimer;

static struct vm_interface vmi = { .print = print, .signaldone = ackdone, .reporterror = fail };

static int	key = 0, roundsdone = 0;
int		debug = 1, verbose = 1;

int myproc() { return PROC_ENGINE; }

static void
print(uint32_t key, char *msg)
{
	warnx("print from vm %u: %s", key, msg);
	vm_injectack(vm_fromkey(key));
}

static void
fail(uint32_t key, char *msg)
{
	vm_killall();
	errx(1, "error callback from vm %u: %s", key, msg);	
}

static void
ackdone(uint32_t key)
{
	struct vm	*new;

	warnx("job %u is done", key);
	roundsdone++;

	if (roundsdone == TEST_ROUNDS) {
		vm_killall();
		warnx("all rounds complete!");
		exit(0);
	}

	/* the jobs take 30 seconds each, and
	 * vms take less than that to spin, so we should
	 * be ready with more vms by the time a job finishes
	 */
	vm_release(vm_fromkey(key));
	new = vm_claim(++key, vmi);

	if (new == NULL)
		err(1, "vm_claim returned error when it shouldn't have");

	else {
		char	 data[10240];
		size_t	 datasize;
		int	 fd;

		warnx("next job provisioned out, key = %u", key);

		if ((fd = open(TEST_BUNDLENAME, O_RDONLY)) < 0)
			err(1, "open %s", TEST_BUNDLENAME);

		if ((datasize = read(fd, data, TEST_BUNDLEMAXSIZE)) < 0)
			err(1, "read %s", TEST_BUNDLENAME);

		vm_injectfile(new, TEST_BUNDLENAME, data, datasize);
		close(fd);
	}
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

	new = vm_claim(key, vmi);

	if (new != NULL) {
		char	 data[10240];
		size_t	 datasize;
		int	 fd;

		warnx("first job provisioned, key = %u", key);

		if ((fd = open(TEST_BUNDLENAME, O_RDONLY)) < 0)
			err(1, "open %s", TEST_BUNDLENAME);

		if ((datasize = read(fd, data, TEST_BUNDLEMAXSIZE)) < 0)
			err(1, "read %s", TEST_BUNDLENAME);

		vm_injectfile(new, TEST_BUNDLENAME, data, datasize);
		close(fd);

	} else if (errno != EAGAIN)  {
		err(1, "vm_claim returned unexpected error");

	} else {
		tv.tv_sec = TEST_POLL_INTERVAL;
		tv.tv_usec = 0;
		evtimer_add(&boottimer, &tv);
	}

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
