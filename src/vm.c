/* vm.c
 * virtual machine management
 * though vmctl(8)
 *
 * NOTE: there's a pf rule you need
 * for this to be fully secure, see notes
 * in the install for details
 *
 * (c) jay lang, 2023
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/wait.h>

#include <errno.h>
#include <event.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "workerd.h"

#define VM_BOOTSTATE	0
#define VM_READYSTATE	1
#define VM_WORKSTATE	2
#define VM_ZOMBIESTATE	3
#define VM_MAXSTATE	4

#define VM_NOKEY	-1

#define VMCTL_PATH	"/usr/sbin/vmctl"

#define VMCTL(ASSERT, ...) do {						\
	int	wstatus;						\
	pid_t	pid;							\
									\
	if ((pid = fork()) < 0)						\
		log_fatal("VMCTL: fork");				\
									\
	if (pid == 0) {							\
		if (!debug) {						\
                	freopen("/dev/null", "a", stdout);		\
			freopen("/dev/null", "a", stderr);		\
		}							\
									\
		execl(VMCTL_PATH, VMCTL_PATH, __VA_ARGS__, NULL);	\
		log_fatal("VMCTL: execl");				\
	}								\
									\
	do {								\
		if (wait(&wstatus) < 0)					\
			log_fatal("VMCTL: wait");			\
	} while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));		\
									\
	if (WIFSIGNALED(wstatus))					\
		log_fatalx("VMCTL: terminated by signal %d",		\
			WTERMSIG(wstatus));				\
									\
	else if ((ASSERT) && WEXITSTATUS(wstatus) != 0)			\
		log_fatalx("VMCTL: exited with status %d",		\
			WEXITSTATUS(wstatus));				\
} while (0)

struct vm {
	int	 	 initialized;
	int	 	 state;	
	uint32_t	 key;

	int		 shouldheartbeat;

	char		*basedisk;
	char		*vivadodisk;

	char		*name;

	struct conn		*conn;
	struct vm_interface	 callbacks;

	SIMPLEQ_ENTRY(vm)	 entries;
};

SIMPLEQ_HEAD(vmqueue, vm);

static struct vmqueue	 bootqueue = SIMPLEQ_HEAD_INITIALIZER(bootqueue);

static void		 bootqueue_enqmachine(struct vm *);
static void	 	 bootqueue_bootfirst(void);
static struct vm	*bootqueue_popfirst(void);
static void		 bootqueue_clear(void);

static struct vm	 allvms[VM_MAXCOUNT] = { 0 };

static int		 allvms_getvmindex(struct vm *);

static void	 	 vm_reset(struct vm *);
static struct vm	*vm_byconn(struct conn *);

static void		 vm_reporterror(struct vm *, const char *, ...);
static void		 vm_reap(struct vm *, int);

static void		 vm_handleteardown(struct conn *);
static void		 vm_accept(struct conn *);
static void		 vm_timeout(struct conn *);
static void		 vm_getmsg(struct conn *, struct netmsg *);

static void		 signaldone_annuled(uint32_t);

static int
allvms_getvmindex(struct vm *v)
{
	return v - (struct vm *)allvms;
}

static void
bootqueue_enqboot(struct vm *v)
{
	SIMPLEQ_INSERT_TAIL(&bootqueue, v, entries);
	if (SIMPLEQ_FIRST(&bootqueue) == v) 
		bootqueue_bootfirst();
}

static void
bootqueue_bootfirst(void)
{
	struct vm	*v;

	v = SIMPLEQ_FIRST(&bootqueue);

	VMCTL(1, "start", "-t", VM_TEMPLATENAME,
		"-d", v->basedisk,
		"-d", v->vivadodisk,
		v->name);
}

static struct vm *
bootqueue_popfirst(void)
{
	struct vm	*v;

	v = SIMPLEQ_FIRST(&bootqueue);
	SIMPLEQ_REMOVE_HEAD(&bootqueue, entries);

	if (!SIMPLEQ_EMPTY(&bootqueue)) bootqueue_bootfirst();

	return v;
}

static void
bootqueue_clear(void)
{
	while (!SIMPLEQ_EMPTY(&bootqueue))
		SIMPLEQ_REMOVE_HEAD(&bootqueue, entries);
}

static void
vm_reporterror(struct vm *v, const char *fmt, ...)
{
	char		*label;
	va_list		 ap;

	va_start(ap, fmt);

	if (vasprintf(&label, fmt, ap) < 0)
		log_fatal("vm_reporterror: vasprintf");

	v->callbacks.reporterror(v->key, label);
	free(label);

	va_end(ap);
}

/* move to the zombie state, clean up connection,
 * and shut down the underlying VM
 * be careful! this code is used by lots of different callers
 * since the song and dance to reset VMs optimistically is complex
 */
static void
vm_reap(struct vm *v, int graceful)
{
	if (v->state == VM_ZOMBIESTATE)
		log_fatalx("vm_reap: tried to reap vm twice");

	if (v->state == VM_BOOTSTATE)
		bootqueue_popfirst();

	if (v->conn != NULL) {
		conn_setteardowncb(v->conn, NULL);		
		conn_teardown(v->conn);
		v->conn = NULL;
	}

	VMCTL(v->state == VM_BOOTSTATE, "stop", "-fw", v->name);

	if (unlink(v->basedisk) < 0)
		log_fatal("vm_reap: unlink vm base image");
	else if (unlink(v->vivadodisk) < 0)
		log_fatal("vm_reap: unlink vm vivado image");

	free(v->basedisk);
	free(v->vivadodisk);
	free(v->name);

	/* if we're in the work state, we have to wait
	 * to be released by our caller. otherwise, we can
	 * recycle ourself
	 */
	if (v->state != VM_WORKSTATE) {
		v->state = VM_ZOMBIESTATE;
		vm_reset(v);
	} else {
		v->state = VM_ZOMBIESTATE;

		if (graceful) v->callbacks.signaldone(v->key);
		else v->callbacks.reporterror(v->key, "connection to vm terminated unexpectedly");
	}
}


static void
vm_reset(struct vm *v)
{
	int	vmid;

	vmid = allvms_getvmindex(v);

	if (!v->initialized) {
		v->initialized = 1;
		v->conn = NULL;

	} else if (v->state != VM_ZOMBIESTATE)
		log_fatalx("vm_reset: bug: tried to reset vm in non-zombie state");

	v->state = VM_BOOTSTATE;
	v->key = VM_NOKEY;

	v->shouldheartbeat = 0;

	memset(&v->callbacks, 0, sizeof(struct vm_interface));

	if (asprintf(&v->basedisk, "%s/base%d.qcow2", DISKS, vmid) < 0)
		log_fatal("vm_init: asprintf base disk name");

	if (asprintf(&v->vivadodisk, "%s/vivado%d.qcow2", DISKS, vmid) < 0)
		log_fatal("vm_init: asprintf vivado disk name");

	if (asprintf(&v->name, "vm%d", vmid) < 0)
		log_fatal("vm_init: asprintf vm name");

	VMCTL(1, "create", "-b", VM_BASEIMAGE, v->basedisk);	
	VMCTL(1, "create", "-b", VM_VIVADOIMAGE, v->vivadodisk);

	bootqueue_enqboot(v);
}

static struct vm *
vm_byconn(struct conn *c)
{
	int i;

	for (i = 0; i < VM_MAXCOUNT; i++) 
		if (allvms[i].conn == c) return &allvms[i];

	log_fatalx("vm_byconn: no such conn %p", c);
}

/* VM connection blew up on us; ungraceful teardown */
static void
vm_handleteardown(struct conn *c)
{
	struct vm	*dead;
	
	dead = vm_byconn(c);

	dead->conn = NULL;
	vm_reap(dead, 0);
}

static void
vm_accept(struct conn *c)
{
	struct vm	*new;
	struct timeval	 tv;

	log_writex(LOGTYPE_DEBUG, "accepted connection from new vm");

	new = bootqueue_popfirst();	
	new->state = VM_READYSTATE;
	new->conn = c;	

	tv.tv_sec = VM_TIMEOUT;
	tv.tv_usec = 0;

	conn_settimeout(new->conn, &tv, vm_timeout);
	conn_setteardowncb(new->conn, vm_handleteardown);
	conn_receive(new->conn, vm_getmsg);
}

static void
vm_timeout(struct conn *c)
{
	struct vm	*v;
	struct netmsg	*heartbeat;

	v = vm_byconn(c);

	if (v->shouldheartbeat) {
		/* line is unresponsive, kill it */
		log_writex(LOGTYPE_DEBUG, "vm_timeout: vm heartbeat timeout");
		vm_reap(v, 0);
	} else {
		log_writex(LOGTYPE_DEBUG, "vm_timeout: vm should heartbeat");
		v->shouldheartbeat = 1;	
		
		heartbeat = netmsg_new(NETOP_HEARTBEAT);
		if (heartbeat == NULL) log_fatal("vm_timeout: netmsg_new");

		conn_send(c, heartbeat);

		conn_stopreceiving(c);
		conn_receive(c, vm_getmsg);
	}
}

static void
vm_getmsg(struct conn *c, struct netmsg *m)
{
	struct vm	*v;
	char		*label, *data;
	size_t		 datasize;
		
	v = vm_byconn(c);
	v->shouldheartbeat = 0;

	if (m == NULL || strlen(netmsg_error(m)) > 0) {
		if (v->state == VM_WORKSTATE)
			vm_reporterror(v, "vm_getmsg: received bad message: %s",
				(m == NULL) ? "unintelligble" : netmsg_error(m));
		return;
	}

	if (v->state != VM_WORKSTATE && netmsg_gettype(m) != NETOP_HEARTBEAT) {
		log_writex(LOGTYPE_DEBUG, "WARNING: ignoring unsolicited message of type %u", netmsg_gettype(m));
		return;
	}

	switch (netmsg_gettype(m)) {
	case NETOP_SENDLINE:
		label = netmsg_getlabel(m);	

		conn_stopreceiving(v->conn);
		v->callbacks.print(v->key, label);			

		free(label);
		break;

	case NETOP_REQUESTLINE:
		conn_stopreceiving(v->conn);
		v->callbacks.readline(v->key);
		break;

	case NETOP_SENDFILE:
		label = netmsg_getlabel(m);
		data = netmsg_getdata(m, (uint64_t *)(&datasize));

		conn_stopreceiving(v->conn);
		v->callbacks.commitfile(v->key, label, data, datasize);

		free(data);
		free(label);
		break;

	case NETOP_REQUESTFILE:
		label = netmsg_getlabel(m);
		conn_stopreceiving(v->conn);
		v->callbacks.loadfile(v->key, label);

		free(label);
		break;

	case NETOP_ERROR:
		/* propagate the error, don't reap yet */
		label = netmsg_getlabel(m);
		conn_stopreceiving(v->conn);
		v->callbacks.reporterror(v->key, label);

		free(label);
		break;

	case NETOP_TERMINATE:
		/* will call signaldone as needed, move us
		 * to zombie state for eventual release
		 * connection stays up for now; this is a graceful
		 * teardown
		 */
		vm_reap(v, 1);
		break;

	case NETOP_HEARTBEAT:
		break;

	/* don't expect to receive acks from the VM */
	default:
		log_writex(LOGTYPE_WARN,
			  "vm_getmsg: vm %s sent unexpected message type %u",
			  v->name,			
			  netmsg_gettype(m));

		vm_reporterror(v, "vm_getmsg: received unexpected message type %u",
			netmsg_gettype(m));
	}
}

void
vm_init(void)
{
	int	i;

	conn_listen(vm_accept, VM_CONN_PORT, CONN_MODE_TCP);
	for (i = 0; i < VM_MAXCOUNT; i++) vm_reset(&allvms[i]);
}

void
vm_killall(void)
{
	struct vm	*subject;
	int		 i;

	bootqueue_clear();

	/* - put all VMs that are initialized into the work state;
	 *	they work for us now. this ensures they won't reset...
	 * - annul the callback for signaldone
	 * - reap each VM gracefully
	 * - you are now safe to exit
	 */
	for (i = 0; i < VM_MAXCOUNT; i++) {
		subject = &allvms[i];

		if (subject->initialized && subject->state != VM_ZOMBIESTATE) {
			subject->state = VM_WORKSTATE;
			subject->callbacks.signaldone = signaldone_annuled;
			vm_reap(subject, 1);
		}
	}
}

struct vm *
vm_claim(uint32_t key, struct vm_interface vmi)
{
	struct vm	*subject;
	int 		 i;

	for (i = 0; i < VM_MAXCOUNT; i++) {
		subject = &allvms[i];

		if (subject->state == VM_READYSTATE) {
			subject->state = VM_WORKSTATE;
			subject->key = key;
			subject->callbacks = vmi;

			return subject;
		}
	}	

	errno = EAGAIN;
	return NULL;
}

struct vm *
vm_fromkey(uint32_t key)
{
	int i;

	for (i = 0; i < VM_MAXCOUNT; i++)
		if (allvms[i].key == key) return &allvms[i];

	errno = EINVAL;
	return NULL;
}

static void
signaldone_annuled(uint32_t k)
{
	(void)k;
}

void
vm_release(struct vm *v)
{
	/* if we are still in a working state, this is basically
	 * asking us to reap. client will 
	 */
	if (v->state != VM_ZOMBIESTATE) {
		v->callbacks.signaldone = signaldone_annuled;
		vm_reap(v, 1);
	}

	vm_reset(v);
}

void
vm_injectfile(struct vm *v, char *label, char *data, size_t datasize)
{
	struct netmsg	*response;

	response = netmsg_new(NETOP_SENDFILE);
	if (response == NULL)
		log_fatal("vm_injectfile: netmsg_new");

	if (netmsg_setlabel(response, label) < 0)
		log_fatalx("vm_injectfile: netmsg_setlabel: %s", netmsg_error(response));

	if (netmsg_setdata(response, data, datasize) < 0)
		log_fatalx("vm_injectfile: netmsg_setdata: %s", netmsg_error(response));

	conn_send(v->conn, response);
	conn_receive(v->conn, vm_getmsg);
}

void
vm_injectline(struct vm *v, char *line)
{
	struct netmsg	*response;

	response = netmsg_new(NETOP_SENDLINE);
	if (response == NULL)
		log_fatal("vm_injectline: netmsg_new");

	if (netmsg_setlabel(response, line) < 0)
		log_fatalx("vm_injectfile: netmsg_setlabel: %s", netmsg_error(response));

	conn_send(v->conn, response);
	conn_receive(v->conn, vm_getmsg);
}

void
vm_injectack(struct vm *v)
{
	struct netmsg	*response;

	response = netmsg_new(NETOP_ACK);
	if (response == NULL)
		log_fatal("vm_injectack: netmsg_new");

	conn_send(v->conn, response);
	conn_receive(v->conn, vm_getmsg);
}
