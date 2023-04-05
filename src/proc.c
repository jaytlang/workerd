#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "workerd.h"

#define SOCKETPAIR_FLAGS	(SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC)

struct proc {
        struct imsgbuf   *ibufs;
	struct event     *readevents;
	void            (*readcbs[PROC_MAX])(int, int, struct ipcmsg *);

	struct event     *sigevents;
	void            (*sigcbs[SIGEV_MAX])(int, short, void *);

	char             *chroot;
	char             *user;
	int               mytype;

	int		  proctypecopies[PROC_MAX];
	int               didhiteof;
};

static int	proc_childforkwithnewsock(struct proc *, void (*)(void));
static void	proc_childstart(int, void (*)(void));
static void	proc_poststartsetup(char *);

static void	proc_dosend(int, short, void *);
static void	proc_dorecv(int, short, void *);

static void	proc_startcrosstalk(int, int, struct ipcmsg *);

static struct proc *p = NULL;

struct proc *
proc_new(int type)
{
	struct proc	*p, *out = NULL;
	int		 i;

	p = calloc(1, sizeof(struct proc));
	if (p == NULL) goto end;

	p->ibufs = calloc(PROC_MAX, sizeof(struct imsgbuf));
	if (p->ibufs == NULL) {
		free(p);
		goto end;
	}

	p->readevents = calloc(PROC_MAX, sizeof(struct event));
	if (p->readevents == NULL) {
		free(p->ibufs);
		free(p);
		goto end;
	}

	p->sigevents = calloc(SIGEV_MAX, sizeof(struct event));
	if (p->sigevents == NULL) {
		free(p->readevents);
		free(p->ibufs);
		free(p);
		goto end;
	}

	p->mytype = type;
	out = p;

	for (i = 0; i < PROC_MAX; i++)
		p->proctypecopies[i] = i;
end:
	return out;
}

void
proc_handlesigev(struct proc *p, int sigev, void (*cb)(int, short, void *))
{
	if (sigev >= SIGEV_MAX || sigev < 0)
		log_fatal("illegal sigev type %d specified", sigev);

	p->sigcbs[sigev] = cb;
}

void
proc_setchroot(struct proc *p, char *chroot)
{
	p->chroot = strdup(chroot);
	if (p->chroot == NULL) log_fatal("strdup chroot");
}

void
proc_setuser(struct proc *p, char *user)
{
	p->user = strdup(user);
	if (p->user == NULL) log_fatal("strdup user");
}

static int
proc_childforkwithnewsock(struct proc *np, void (*launch)(void))
{
	pid_t	pid;
	int	sock[2];

	if (socketpair(AF_UNIX, SOCKETPAIR_FLAGS, 0, sock) < 0)
		log_fatal("proc_mk: socketpair");
	
	if ((pid = fork()) < 0) log_fatal("proc_mk: fork");
	else if (pid == 0) {
		p = np;

		close(sock[0]);
		proc_childstart(sock[1], launch);
		exit(0);
	}

	close(sock[1]);
	return sock[0];
}

static void
proc_poststartsetup(char *ident)
{
	struct passwd	*userpw = NULL;
	int		 i;

	log_writex(LOGTYPE_DEBUG, "starting %s (pid=%d)", ident, getpid());

	if (p->user != NULL)
		if ((userpw = getpwnam(p->user)) == NULL)
			log_fatalx("proc_poststartsetup: no such user %s", p->user);

	if (p->chroot != NULL && chroot(p->chroot) < 0) 
		log_fatal("proc_poststartsetup: chroot %s", p->chroot);

	if (userpw != NULL) {
		if (setresgid(userpw->pw_gid, userpw->pw_gid, userpw->pw_gid) < 0)
			log_fatal("proc_poststartsetup: setresgid");
		else if (setresuid(userpw->pw_uid, userpw->pw_uid, userpw->pw_uid) < 0)
			log_fatal("proc_poststartsetup: setresuid");
	}

	for (i = 0; i < SIGEV_MAX; i++) {
		int	signalcode;

		switch (i) {
		case SIGEV_HUP: signalcode = SIGHUP; break;
		case SIGEV_INT: signalcode = SIGINT; break;
		case SIGEV_TERM: signalcode = SIGTERM; break;
		case SIGEV_PIPE: signalcode = SIGPIPE; break;
		}

		if (p->sigcbs[i] != NULL) {
			signal_set(&p->sigevents[i], signalcode, p->sigcbs[i], NULL);
			signal_add(&p->sigevents[i], NULL);

		} else signal(signalcode, SIG_IGN);
	}
}

void
proc_startall(struct proc *parentproc, struct proc *frontendproc, struct proc *engineproc)
{
	struct ipcmsg	*fdtransfermsg;
	char		*marshalledmsg;
	uint16_t	 marshalledmsgsize;

	int		 tofrontend, toengine, childtochild[2];
	int		 msgstatus;

	p = parentproc;

	tofrontend = proc_childforkwithnewsock(frontendproc, frontend_launch);
	toengine = proc_childforkwithnewsock(engineproc, engine_launch);

	event_init();

	imsg_init(&p->ibufs[PROC_FRONTEND], tofrontend);
	imsg_init(&p->ibufs[PROC_ENGINE], toengine);

	if (socketpair(AF_UNIX, SOCKETPAIR_FLAGS, 0, childtochild) < 0)
		log_fatal("socketpair for children");

	fdtransfermsg = ipcmsg_new(0, NULL);
	if (fdtransfermsg == NULL) log_fatal("ipcmsg_new");

	marshalledmsg = ipcmsg_marshal(fdtransfermsg, &marshalledmsgsize);
	if (marshalledmsg == NULL) log_fatal("ipcmsg_marshal");

	/* mail to the frontend */

	msgstatus = imsg_compose(&p->ibufs[PROC_FRONTEND], IMSG_INITFD, PROC_FRONTEND,
		PROC_PARENT, childtochild[0], marshalledmsg, marshalledmsgsize);

	if (msgstatus != 1) log_fatal("imsg_compose for sendfd");

	if (imsg_flush(&p->ibufs[PROC_FRONTEND]) < 0)
		log_fatal("imsg_flush");

	/* mail to the engine */

	msgstatus = imsg_compose(&p->ibufs[PROC_ENGINE], IMSG_INITFD, PROC_ENGINE,
		PROC_PARENT, childtochild[1], marshalledmsg, marshalledmsgsize);

	if (msgstatus != 1) log_fatal("imsg_compose for sendfd");

	if (imsg_flush(&p->ibufs[PROC_ENGINE]) < 0)
		log_fatal("imsg_flush");

	free(marshalledmsg);
	ipcmsg_teardown(fdtransfermsg);

	proc_poststartsetup("workerd parent");
}


static void
proc_startcrosstalk(int type, int fd, struct ipcmsg *data)
{
	int	origin;

	if (type != IMSG_INITFD)
		log_fatalx("expected IMSG_INITFD from parent");

	origin = (p->mytype == PROC_FRONTEND) ? PROC_ENGINE : PROC_FRONTEND;
	imsg_init(&p->ibufs[origin], fd);

	myproc_stoplisten(PROC_PARENT);
	event_loopbreak();

	(void)fd;
	(void)data;
}

static void
proc_childstart(int parentfd, void (*launch)(void))
{
	event_init();

	imsg_init(&p->ibufs[PROC_PARENT], parentfd);
	myproc_listen(PROC_PARENT, proc_startcrosstalk);

	event_dispatch();

	if (p->didhiteof)
		log_fatalx("event_dispatch got eof on parent socket before "
			"setting up cross talk with other child");

	proc_poststartsetup((p->mytype == PROC_FRONTEND) ?
		"workerd frontend" :
		"workerd engine");

	launch();

	/* exit after this point */
}

int
myproc(void)
{
	return p->mytype;
}

void
myproc_send(int dest, int type, int fd, struct ipcmsg *msg)
{
	char		*marshalledmsg;
	uint16_t	 marshalledmsgsize;
	int		 msgstatus;

	if (dest >= PROC_MAX || dest < 0)
		log_fatalx("bad message dest %d", dest);
	else if (type >= IMSG_MAX || type < 0)
		log_fatalx("bad message type %d", type);
	else if (msg == NULL)
		log_fatalx("tried to send null message");

	marshalledmsg = ipcmsg_marshal(msg, &marshalledmsgsize);

	if (marshalledmsg == NULL)
		log_fatal("ipcmsg_marshal for interprocess send");

	msgstatus = imsg_compose(&p->ibufs[dest], (uint32_t)type, (uint32_t)dest,
		(pid_t)p->mytype, fd, marshalledmsg, marshalledmsgsize);

	if (msgstatus != 1) log_fatal("imsg_compose (message type %d)", type);

	event_once(p->ibufs[dest].fd, EV_WRITE, &proc_dosend,
		&p->ibufs[dest], NULL);

	free(marshalledmsg);
}

static void
proc_dosend(int fd, short event, void *arg)
{
	struct imsgbuf	*ibuf = (struct imsgbuf *)arg;
	ssize_t		 n;

	/* note: this returns zero on EOF condition, i.e. no data to send
	 * there doesn't seem to be a way to check into this vs. a closed
	 * socket, so rely on proc_dorecv to detect truly closed connections
	 */
	if ((n = (ssize_t)msgbuf_write(&ibuf->w)) < 0 && errno != EAGAIN)
		log_fatal("msgbuf_write");

	(void)event;
	(void)fd;
}

void
myproc_listen(int source, void (*cb)(int, int, struct ipcmsg *))
{
	if (source >= PROC_MAX || source < 0)
		log_fatalx("bad listen source %d", source);
	else if (event_initialized(&p->readevents[source]))
		log_fatalx("tried to listen twice on same fd");

	event_set(&p->readevents[source], p->ibufs[source].fd, EV_READ | EV_PERSIST,
		&proc_dorecv, &p->proctypecopies[source]);
	event_add(&p->readevents[source], NULL);

	p->readcbs[source] = cb;
}

static void
proc_dorecv(int fd, short event, void *arg)
{
	int	source = *(int *)arg;	
	ssize_t	n;

	if ((n = imsg_read(&p->ibufs[source])) < 0 && errno != EAGAIN)
		log_fatal("imsg_read");
	else if (n == 0) {
		myproc_stoplisten(source);	
		event_loopexit(NULL);
		p->didhiteof = 1;
		return;
	}

	for (;;) {
		struct imsg	 imsg;
		struct ipcmsg	*data;
		uint16_t	 datalen;

		if ((n = imsg_get(&p->ibufs[source], &imsg)) == -1)
			log_fatal("imsg_get");
		else if (n == 0) break;

		datalen = imsg.hdr.len - IMSG_HEADER_SIZE;
		data = ipcmsg_unmarshal(imsg.data, datalen);

		if (data == NULL) log_fatal("ipcmsg_unmarshal");

		p->readcbs[source]((int)imsg.hdr.type, imsg.fd, data);

		ipcmsg_teardown(data);
		imsg_free(&imsg);
	}

	(void)fd;
	(void)event;
}

void
myproc_stoplisten(int source)
{
	if (event_initialized(&p->readevents[source])) {
		event_del(&p->readevents[source]);
		bzero(&p->readevents[source], sizeof(struct event));
	}
}

int
myproc_ischrooted(void)
{
	return p->chroot != NULL;
}
