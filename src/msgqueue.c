/* in-order message queueing system
 * this is only really needed in situations
 * where there are errors or a client is spamming us,
 * and guarantees graceful shutdown etc.
 *
 * (c) jay lang 2023
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>

#include <errno.h>
#include <event.h>
#include <limits.h>
#include <stdlib.h>

#include "workerd.h"

struct queuedmsg {
	struct netmsg			*msg;
	SIMPLEQ_ENTRY(queuedmsg)	 entries;
};

SIMPLEQ_HEAD(queuehead, queuedmsg);

struct msgqueue {
	struct queuehead	  queuehead;
	struct event		  sendevent;

	size_t			  cachedoffset;

	void			(*cb)(struct msgqueue *, struct conn *);
	struct conn		 *c;
};

static void	msgqueue_tryeventing(struct msgqueue *);
static void	msgqueue_event(int, short, void *);

static void
msgqueue_tryeventing(struct msgqueue *mq)
{
	if (!event_pending(&mq->sendevent, EV_WRITE, NULL)) {
		if (!SIMPLEQ_EMPTY(&mq->queuehead))
			if (event_add(&mq->sendevent, NULL) < 0)
				log_fatal("msgqueue_tryeventing: event_add");

	} else if (SIMPLEQ_EMPTY(&mq->queuehead)) {
		if (event_del(&mq->sendevent))
			log_fatal("msgqueue_tryeventing: event_del");
	}
}

static void
msgqueue_event(int fd, short event, void *arg)
{
	struct msgqueue		*mq = (struct msgqueue *)arg;

	mq->cb(mq, mq->c);
	msgqueue_tryeventing(mq);

	(void)fd;
	(void)event;
}

struct msgqueue *
msgqueue_new(struct conn *c, void (*cb)(struct msgqueue *, struct conn *))
{
	struct msgqueue		*mq, *out = NULL;

	mq = malloc(sizeof(struct msgqueue));
	if (mq == NULL) goto end;

	SIMPLEQ_INIT(&mq->queuehead);

	event_set(&mq->sendevent, conn_getfd(c), EV_WRITE, msgqueue_event, mq);

	mq->cachedoffset = 0;
	mq->cb = cb;
	mq->c = c;

	out = mq;
end:
	return out;
}

void
msgqueue_teardown(struct msgqueue *mq)
{
	while (!SIMPLEQ_EMPTY(&mq->queuehead))
		msgqueue_deletehead(mq);

	free(mq);
}

void
msgqueue_append(struct msgqueue *mq, struct netmsg *msg)
{
	struct queuedmsg	*newentry;

	newentry = malloc(sizeof(struct queuedmsg));
	if (newentry == NULL) log_fatal("msgqueue_append: malloc");

	newentry->msg = msg;
	SIMPLEQ_INSERT_TAIL(&mq->queuehead, newentry, entries);

	msgqueue_tryeventing(mq);
}

void
msgqueue_deletehead(struct msgqueue *mq)
{
	if (!SIMPLEQ_EMPTY(&mq->queuehead)) {
		struct queuedmsg	*first;

		first = SIMPLEQ_FIRST(&mq->queuehead);
		SIMPLEQ_REMOVE_HEAD(&mq->queuehead, entries);

		netmsg_teardown(first->msg);
		free(first);
	}

	msgqueue_tryeventing(mq);
}

struct netmsg *
msgqueue_gethead(struct msgqueue *mq)
{
	struct queuedmsg	*first;
	struct netmsg		*out = NULL;

	if (SIMPLEQ_EMPTY(&mq->queuehead)) goto end;

	first = SIMPLEQ_FIRST(&mq->queuehead);
	out = first->msg;
end:
	return out;
}

size_t
msgqueue_getcachedoffset(struct msgqueue *mq)
{
	return mq->cachedoffset;
}

int
msgqueue_setcachedoffset(struct msgqueue *mq, size_t offset)
{
	int status	= -1;

	if (offset > SSIZE_MAX) {
		errno = EINVAL;
		goto end;
	}

	mq->cachedoffset = offset;
	status = 0;
end:
	return status;
}
