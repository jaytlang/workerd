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

#define ARCHIVES	"/archives"
#define MESSAGES	"/messages"
#define DISKS		"/disks"

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
#define NETOP_SENDLINE		1
#define NETOP_REQUESTLINE	2

#define NETOP_SENDFILE		3
#define NETOP_REQUESTFILE	4

#define NETOP_TERMINATE		5

#define NETOP_ACK		6
#define NETOP_ERROR		7
#define NETOP_MAX       	8


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

#define CONN_PORT       8123

#define CONN_CA_PATH    "/etc/ssl/authority"
#define CONN_CERT       "/etc/ssl/serverchain.pem"
#define CONN_KEY        "/etc/ssl/private/server.key"

struct conn;

void                     conn_listen(void (*)(struct conn *));
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

#define VM_MAXCOUNT	8
#define VM_TIMEOUT	1

#define VM_TEMPLATE	"template"

#define VM_BASEIMAGE	"/home/" USER "/ubuntu.qcow2"
#define VM_VIVADOIMAGE	"/home/" USER "/vivado.qcow2"

struct vm;

struct vm_interface {
	void	 (*print)(uint32_t, char *);
	char	*(*readline)(uint32_t);
	char	*(*loadfile)(uint32_t, size_t *);
	void	 (*commitfile)(uint32_t, char *, size_t);

	void	 (*signalready)(uint32_t);
	void	 (*signaldone)(uint32_t);
};

struct vm	*vm_claim(uint32_t, struct vm_interface);
struct vm	*vm_fromkey(uint32_t);
void		 vm_release(struct vm *);
void		 vm_releaseall(void);



#endif /* WORKERD_H */
