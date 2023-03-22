/* vm.c
 * virtual machine management
 * though vmctl(8)
 *
 * NOTE: this is _heavily_ pf assisted.
 * (c) jay lang, 2023
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/wait.h>

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
#define VM_MAXSTATE	3

#define VM_NOKEY	-1

#define VMCTL(__VA_ARGS__) do {					\
	int	wstatus;					\
	pid_t	pid;						\
								\		
	if ((pid = fork()) < 0)					\
		log_fatal("VMCTL: fork");			\
								\
	if (pid == 0) {						\
                freopen("/dev/null", "a", stdout);		\
                freopen("/dev/null", "a", stderr);		\
								\
		execl(__VA_ARGS__, NULL);			\
		log_fatal("VMCTL: execl");			\
	}							\
								\
	do {							\
		if (wait(&wstatus) < 0)				\
			log_fatal("VMCTL: wait");		\
	} while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));	\
								\
	if (WIFSIGNALED(wstatus))				\
		log_fatalx("VMCTL: terminated by signal %d",	\
			WTERMSIG(wstatus));			\
								\
	else if (WEXITSTATUS(wstatus) != 0)			\
		log_fatalx("VMCTL: exited with status %d",	\
			WEXITSTATUS(wstatus));			\
} while (0)

struct vm {
	int	 initialized;
	int	 state;	
	int	 key;

	int	 shouldheartbeat;

	char	*basedisk;
	char	*vivadodisk;

	char	*name;

	struct conn		*conn;
	struct vm_interface	 callbacks;

	SIMPLEQ_ENTRY(vm)	 entries;
};

SIMPLEQ_HEAD(vmqueue, vm);

static struct vmqueue	 bootqueue = SIMPLEQ_HEAD_INITIALIZER(bootqueue);

static void		 bootqueue_enqmachine(struct vm *);
static void	 	 bootqueue_bootfirst(void);
static struct vm	*bootqueue_popfirst(void);

static struct vm	 allvms[VM_MAXCOUNT] = { 0 };

static int		 allvms_getvmindex(struct vm *);

static void	 	 vm_reset(struct vm *);
static struct vm	*vm_bykey(uint32_t);
static struct vm	*vm_byconn(struct conn *);

static void		 vm_senderror(struct vm *, const char *, ...);

static void		 vm_handleteardown(struct conn *);
static void		 vm_accept(struct conn *);
static void		 vm_timeout(struct conn *);
static void		 vm_getmsg(struct conn *, struct netmsg *);

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

	VMCTL("start", "-t", VM_TEMPLATENAME,
		"-d", v->basedisk,
		"-d", v->vivadodisk,
		v->name);
}

static struct vm *
bootqueue_popfirst(struct vm *v)
{
	struct vm	*v;

	v = SIMPLEQ_FIRST(&bootqueue);
	SIMPLEQ_REMOVE_HEAD(&bootqueue, entries);

	if (!SIMPLEQ_EMPTY(&bootqueue)) bootqueue_bootfirst();

	return v;
}

static void
vm_senderror(struct vm *v, const char *fmt, ...)
{
	struct netmsg	*error;
	char		*label;
	va_list		 ap;

	va_start(ap, fmt);

	if (vasprintf(&label, fmt, ap) < 0)
		log_fatal("vm_senderror: vasprintf");

	error = netmsg_new(NETOP_ERROR);
	if (error == NULL)
		log_fatal("vm_senderror: netmsg_new");

	if (netmsg_setlabel(error, label) < 0)
		log_fatalx("vm_senderror: netmsg_setlabel: %s", netmsg_error(error));

	conn_send(v->conn, error);

	free(label);
	va_end(ap);
}


static void
vm_reset(struct vm *v)
{
	int	vmid;

	vmid = allvms_getvmindex(v);

	if (!v->initialized) {
		v->initialized = 1;
		v->conn = NULL;

	} else {
		if (v->state != VM_BOOTSTATE) {
			VMCTL("stop", "-fw", v->name);

			conn_teardown(v->conn);
			v->conn = NULL;
		}
			
		if (unlink(v->basedisk) < 0)
			log_fatal("vm_reset: unlink vm base image");
		else if (unlink(v->vivadodisk) < 0)
			log_fatal("vm_reset: unlink vm vivado image");

		free(v->basedisk);
		free(v->vivadodisk);
		free(v->name);
	}

	v->state = VM_BOOTSTATE;
	v->key = VM_NOKEY;

	v->shouldheartbeat = 0;

	memset(v->callbacks, 0, sizeof(struct vm_interface));

	if (asprintf(&v->basedisk, "%s/base%d.qcow2", DISKS, vmid) < 0)
		log_fatal("vm_init: asprintf base disk name");

	if (asprintf(&v->vivadodisk, "%s/vivado%d.qcow2", DISKS, vmid) < 0)
		log_fatal("vm_init: asprintf vivado disk name");

	if (asprintf(&v->name, "vm%d", vmid) < 0)
		log_fatal("vm_init: asprintf vm name");

	VMCTL("create", "-b", VM_BASEIMAGE, v->basedisk);	
	VMCTL("create", "-b", VM_BASEIMAGE, v->vivadodisk);

	bootqueue_enqboot(v);
}

static struct vm *
vm_bykey(uint32_t key)
{
	int i;

	for (i = 0; i < VM_MAXCOUNT; i++)
		if (allvms[i].key == key) return &allvms[i];

	errno = EINVAL;
	return NULL;
}

static struct vm *
vm_byconn(struct conn *c)
{
	int i;

	for (i = 0; i < VM_MAXCOUNT; i++) 
		if (allvms[i].conn == c) return &allvms[i];

	log_fatalx("vm_byconn: no such conn %p", c);
}

static void
vm_handleteardown(struct conn *c)
{
	struct vm	*dead;
	

	dead = vm_byconn(c);
	if (dead->state == VM_WORKSTATE)
		dead->callbacks.signaldone(dead->key);

	vm_reset(dead);
}

static void
vm_accept(struct conn *c)
{
	struct vm	*new;
	struct timeval	 tv;

	new = bootqueue_popfirst();	
	new->state = VM_READYSTATE;
	new->conn = c;	

	tv.tv_sec = VM_TIMEOUT;
	tv.tv_usec = 0;

	conn_settimeout(new->conn, &tv, vm_timeout);
	conn_setteardowncb(new->conn, &tv, vm_handleteardown);
	conn_receive(new->conn, vm_getmsg);
}

static void
vm_timeout(struct conn *c)
{
	struct vm	*v;
	struct netmsg	*heartbeat;

	v = vm_byconn(c);

	if (v->shouldheartbeat) {
		log_writex(LOGTYPE_DEBUG, "vm heartbeat timeout");
		conn_teardown(c);
	} else {
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
		
	v = vm_byconn(c);
	v->shouldheartbeat = 0;

	if (m == NULL || strlen(netmsg_error(m)) > 0) {
		vm_senderror(v, "received bad message: %s",
			(m == NULL) ? "unintelligble" : netmsg_error(m));
		return;
	}

	switch (netmsg_gettype(m)) {

	case NETOP_SENDLINE:

	}
}
