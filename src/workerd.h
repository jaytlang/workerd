/* workerd.h
 * (c) jay lang, 2023
 * global state, prototypes, definitions
 */

#ifndef WORKERD_H
#define WORKERD_H

#include <sys/types.h>

/* TODO: have these live in a configuration file as appropriate */
#define USER		"_workerd"
#define CHROOT		"/var/workerd"

#define ARCHIVES	CHROOT "/archives"
#define MESSAGES	CHROOT "/messages"
#define DISKS		CHROOT "/disks"

#define MAXNAMESIZE	1024
#define MAXFILESIZE	1048576
#define MAXSIGSIZE	177

#define ERRSTRSIZE	2048

extern char *__progname;
extern int debug, verbose;

struct sockaddr_in;
struct timeval;

/* log.c */

#define LOGTYPE_MSG	0
#define LOGTYPE_DEBUG	1
#define LOGTYPE_WARN	2
#define LOGTYPE_MAX	3

void		log_init(void);
void		log_write(int, const char *, ...);
void		log_writex(int, const char *, ...);
__dead void	log_fatal(const char *, ...);
__dead void	log_fatalx(const char *, ...);


/* buffer.c */

int              buffer_open(void);
int              buffer_close(int);
int              buffer_truncate(int, off_t);
ssize_t          buffer_read(int, void *, size_t);
ssize_t          buffer_write(int, const void *, size_t);
off_t            buffer_seek(int, off_t, int);


/* netmsg.c */

struct netmsg;

#define NETOP_UNUSED    	0

/* will always make it all the way
 * to the remote host; engine does not
 * block; in memory messages -> no special handling
 * can come inbound as a response to REQUESTLINE
 */
#define NETOP_SENDLINE		1

/* will always make it all the way
 * to the remote host; engine blocks
 * on frontend; in memory message
 */
#define NETOP_REQUESTLINE	2

/* if sent from vm, will make it all
 * the way to the remote host; engine
 * will block on ACK from frontend; disk
 * message -> have to retain and store a
 * handle to it until frontend gets back
 *
 * can come inbound as a response to REQUESTFILE
 * generated and handled entirely within the
 * engine. in this case, no specal handling is
 * necessary after message send
 */
#define NETOP_SENDFILE		3

/* if sent from vm, handled completely within
 * the engine. in-memory message. engine does not
 * block.
 */
#define NETOP_REQUESTFILE	4

/* makes it all the way to the remote host,
 * engine does not block, in-memory -> no special
 * handling
 */
#define NETOP_TERMINATE		5
#define NETOP_ERROR		6

/* an ack, originates from the frontend
 * engine blocks in a sendfile until this
 * message is received, since the vm might
 * send a sendfile and a terminate back to back
 * and we need to make sure the sendfile goes through
 */
#define NETOP_ACK		7

/* timeout-prevention message, only passes
 * between frontend-client and vm-engine
 */
#define NETOP_HEARTBEAT		8

#define NETOP_MAX       	9


struct netmsg   *netmsg_new(uint8_t);
struct netmsg   *netmsg_loadweakly(char *);

void             netmsg_retain(struct netmsg *);
void             netmsg_teardown(struct netmsg *);

const char      *netmsg_error(struct netmsg *);
void             netmsg_clearerror(struct netmsg *);

ssize_t          netmsg_write(struct netmsg *, void *, size_t);
ssize_t          netmsg_read(struct netmsg *, void *, size_t);
ssize_t          netmsg_seek(struct netmsg *, ssize_t, int);
int              netmsg_truncate(struct netmsg *, ssize_t);

uint8_t          netmsg_gettype(struct netmsg *);
char            *netmsg_getpath(struct netmsg *);

char            *netmsg_getlabel(struct netmsg *);
int              netmsg_setlabel(struct netmsg *, char *);

char            *netmsg_getdata(struct netmsg *, uint64_t *);
int              netmsg_setdata(struct netmsg *, char *, uint64_t);

int              netmsg_isvalid(struct netmsg *, int *);


/* conn.c */

#define CONN_MODE_TCP	0
#define CONN_MODE_TLS	1
#define CONN_MODE_MAX	2

#define FRONTEND_CONN_PORT	443
#define VM_CONN_PORT		8123

#define CONN_CA_PATH    "/etc/ssl/authority"
#define CONN_CERT       "/etc/ssl/serverchain.pem"
#define CONN_KEY        "/etc/ssl/private/server.key"

struct conn;

void                     conn_listen(void (*)(struct conn *), uint16_t, int);
void                     conn_teardown(struct conn *);
void                     conn_teardownall(void);

void                     conn_receive(struct conn *, void (*)(struct conn *, struct netmsg *));
void                     conn_stopreceiving(struct conn *);

void                     conn_setteardowncb(struct conn *, void (*)(struct conn *));

void                     conn_settimeout(struct conn *, struct timeval *, void (*)(struct conn *));
void                     conn_canceltimeout(struct conn *);

void                     conn_send(struct conn *, struct netmsg *);

int                      conn_getfd(struct conn *);
struct sockaddr_in      *conn_getsockpeer(struct conn *);


/* msgqueue.c */

struct msgqueue;

struct msgqueue *msgqueue_new(struct conn *, void (*)(struct msgqueue *, struct conn *));
void             msgqueue_teardown(struct msgqueue *);

void             msgqueue_append(struct msgqueue *, struct netmsg *);
void             msgqueue_deletehead(struct msgqueue *);

struct netmsg   *msgqueue_gethead(struct msgqueue *);
size_t           msgqueue_getcachedoffset(struct msgqueue *);
int              msgqueue_setcachedoffset(struct msgqueue *, size_t);


/* vm.c */

/* should be small - constrained by core count */
#define VM_MAXCOUNT	4
#define VM_TIMEOUT	1

#define VM_TEMPLATENAME	"template"

#define VM_BASEIMAGE	"/home/" USER "/base.qcow2"
#define VM_VIVADOIMAGE	"/home/" USER "/vivado.qcow2"

struct vm;

struct vm_interface {
	void	(*print)(uint32_t, char *);
	void	(*readline)(uint32_t);
	void	(*loadfile)(uint32_t, char *);
	void	(*commitfile)(uint32_t, char *, char *, size_t);

	void	(*signaldone)(uint32_t);
	void	(*reporterror)(uint32_t, char *);
};

void		 vm_init(void);
void		 vm_killall(void);

struct vm	*vm_claim(uint32_t, struct vm_interface);
struct vm	*vm_fromkey(uint32_t);
void		 vm_release(struct vm *);

void		 vm_injectfile(struct vm *, char *, char *, size_t);
void		 vm_injectline(struct vm *, char *);
void		 vm_injectack(struct vm *);

#endif /* WORKERD_H */
