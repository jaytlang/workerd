/* workerd connection management
 * (c) jay lang, 2023
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/tree.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <errno.h>
#include <event.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tls.h>
#include <unistd.h>

#include "workerd.h"

#define CONN_LISTENBACKLOG	128
#define CONN_MTU		1500

struct globalcontext {
	int			 mode;
	uint8_t			*tls_key;	
	size_t 			 tls_keysize;

	struct tls_config	*tls_globalcfg;
	struct tls		*tls_serverctx;

	int			 listen_fd;
	struct event		 listen_event;
};

static void	globalcontext_init(int);
static void	globalcontext_teardown(void);

static void	globalcontext_listen(void (*)(struct conn *), uint16_t);
static void	globalcontext_accept(int, short, void *);
static void	globalcontext_stoplistening(void);

static int	globalcontext_initialized = 0;

static struct globalcontext	globalcontext;

struct conn {
	int			  sockfd;
	struct sockaddr_in	  peer;

	struct tls		 *tls_context;
	struct event		  event_receive;
	struct timeval		  timeout;

	struct netmsg		 *incoming_message;
	struct msgqueue		 *outgoing;

	void			(*cb_receive)(struct conn *, struct netmsg *);
	void			(*cb_timeout)(struct conn *);
	void			(*cb_teardown)(struct conn *);

	RB_ENTRY(conn)		  entries;
};

RB_HEAD(conntree, conn);

static struct conn		*conn_new(int, struct sockaddr_in *, struct tls *);
static int			 conn_compare(struct conn *, struct conn *);

static void			 conn_doreceive(int, short, void *);
static void			 conn_dosend(struct msgqueue *, struct conn *);

static struct conntree allcons = RB_INITIALIZER(&allcons);

RB_PROTOTYPE_STATIC(conntree, conn, entries, conn_compare)
RB_GENERATE_STATIC(conntree, conn, entries, conn_compare)

static void
globalcontext_init(int mode)
{
	struct tls_config	*globalcfg;
	struct tls		*serverctx;

	uint8_t			*key;
	size_t			 keysize;

	if (mode < 0 || mode > CONN_MODE_MAX)
		log_fatalx("globalcontext_init: bug - specified invalid mode %d", mode);

	if (mode == CONN_MODE_TLS) {
		globalcfg = tls_config_new();
		if (globalcfg == NULL)
			log_fatalx("globalcontext_init: can't allocate tls globalcfg");
	
		if (tls_config_set_ca_path(globalcfg, CONN_CA_PATH) < 0) {
			tls_config_free(globalcfg);
			log_fatalx("globalcontext_init: can't set ca path to %s", CONN_CA_PATH);
		}
	
		if (tls_config_set_cert_file(globalcfg, CONN_CERT) < 0) {
			tls_config_free(globalcfg);
			log_fatalx("globalcontext_init: can't set cert file to %s", CONN_CERT);
		}
	
		if ((key = tls_load_file(CONN_KEY, &keysize, NULL)) == NULL) {
			tls_config_free(globalcfg);
			log_fatalx("globalcontext_init: can't load keyfile %s", CONN_KEY);
		}
	
		if (tls_config_set_key_mem(globalcfg, key, keysize) < 0) {
			tls_unload_file(key, keysize);
			tls_config_free(globalcfg);
			log_fatalx("globalcontext_init: can't set key memory");
		}

		tls_config_verify_client(globalcfg);

		serverctx = tls_server();
		if (serverctx == NULL)
			log_fatalx("globalcontext_init: can't allocate tls serverctx");
	
		if (tls_configure(serverctx, globalcfg) < 0)
			log_fatalx("globalcontext_init: can't configure server: %s",
				tls_error(serverctx));

		globalcontext.tls_key = key;
		globalcontext.tls_keysize = keysize;
	
		globalcontext.tls_globalcfg = globalcfg;
		globalcontext.tls_serverctx = serverctx;
	}

	globalcontext.mode = mode;
	globalcontext.listen_fd = -1;
	globalcontext_initialized = 1;

	bzero(&globalcontext.listen_event, sizeof(struct event));
}

static void
globalcontext_teardown(void)
{
	if (globalcontext.listen_fd > 0)
		log_fatalx("globalcontext_teardown: prematurely tore down listener");

	close(globalcontext.listen_fd);
	globalcontext.listen_fd = -1;

	if (globalcontext.mode == CONN_MODE_TLS) {
		tls_free(globalcontext.tls_serverctx);
		tls_config_free(globalcontext.tls_globalcfg);
		tls_unload_file(globalcontext.tls_key, globalcontext.tls_keysize);
	}

	bzero(&globalcontext, sizeof(struct globalcontext));	
	globalcontext_initialized = 0;
}

static void
globalcontext_listen(void (*cb)(struct conn *), uint16_t port)
{
	struct sockaddr_in	sa;
	int			lfd, enable = 1;

	lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (lfd < 0) log_fatal("globalcontext_listen: socket");

	if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
		log_fatal("globalcontext_listen: enable SO_REUSEADDR");

	bzero(&sa, sizeof(struct sockaddr_in));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(lfd, (struct sockaddr *)&sa, sizeof(struct sockaddr_in)) < 0)
		log_fatal("globalcontext_listen: bind");

	if (listen(lfd, CONN_LISTENBACKLOG) < 0)
		log_fatal("globalcontext_listen: listen");

	event_set(&globalcontext.listen_event, lfd, EV_READ | EV_PERSIST,
		globalcontext_accept, (void *)cb);

	if (event_add(&globalcontext.listen_event, NULL) < 0) {
		bzero(&globalcontext.listen_event, sizeof(struct event));
		log_fatal("globalcontext_listen: event_add");
	}

	globalcontext.listen_fd = lfd;
}

static void
globalcontext_accept(int fd, short event, void *arg)
{
	struct sockaddr_in	 peer;
	struct tls		 *connctx = NULL;
	struct conn		 *newconn;

	socklen_t	  	  addrlen;
	int			  newfd;	

	void			(*cb)(struct conn *) = (void (*)(struct conn *))arg;

	/* XXX: will this always work? i.e. could we get a RST immediately
	 * after an incoming SYN that breaks everything and causes an error here
	 *
	 * a month later: i think this is probably fine, but like who knows
	 * one heck of a corner case, but a good stackoverflow question
	 */
	newfd = accept4(fd, (struct sockaddr *)&peer, &addrlen,
		SOCK_NONBLOCK | SOCK_CLOEXEC);

	if (newfd < 0) log_fatal("globalcontext_accept: accept");
	
	if (globalcontext.mode == CONN_MODE_TLS)
		if (tls_accept_socket(globalcontext.tls_serverctx, &connctx, newfd) < 0)
			log_fatalx("tls_accept_socket: %s", tls_error(globalcontext.tls_serverctx));

	newconn = conn_new(newfd, &peer, connctx);
	cb(newconn);

	(void)event;
}

static void
globalcontext_stoplistening(void)
{
	if (event_del(&globalcontext.listen_event) < 0)
		log_fatal("globalcontext_stoplistening: event_del");

	globalcontext.listen_fd = -1;
	bzero(&globalcontext.listen_event, sizeof(struct event));
}

static struct conn *
conn_new(int fd, struct sockaddr_in *peer, struct tls *connctx)
{
	struct conn	*out;	

	out = calloc(1, sizeof(struct conn));
	if (out == NULL) log_fatal("conn_new: calloc struct conn");

	out->sockfd = fd;
	out->tls_context = connctx;

	memcpy(&out->peer, peer, sizeof(struct sockaddr_in));

	out->outgoing = msgqueue_new(out, conn_dosend);
	if (out->outgoing == NULL) log_fatal("conn_new: msgqueue_new");

	RB_INSERT(conntree, &allcons, out);
	return out;
}

static int
conn_compare(struct conn *a, struct conn *b)
{
	int	result = 0;

	if (a->sockfd > b->sockfd) result = 1;
	if (a->sockfd < b->sockfd) result = -1;

	return result;
}

static void
conn_doreceive(int fd, short event, void *arg)
{
	struct conn	*c = (struct conn *)arg;
	char		*receivebuf = NULL;

	ssize_t		 receivesize = 0;
	int		 unrecoverable, willteardown = 0;

	if (event & EV_TIMEOUT) {
		c->cb_timeout(c);
		return;
	}

	if (event & EV_TIMEOUT) {
		c->cb_timeout(c);
		return;
	}

	for (;;) {
		ssize_t		 thispacketsize;
		char		*newreceivebuf;

		newreceivebuf = reallocarray(receivebuf, receivesize + CONN_MTU, sizeof(char));
		if (newreceivebuf == NULL)
			log_fatal("conn_doreceive: reallocarray");

		receivebuf = newreceivebuf;

		if (globalcontext.mode == CONN_MODE_TLS)
			thispacketsize = tls_read(c->tls_context, receivebuf + receivesize, CONN_MTU);
		else {
			thispacketsize = read(c->sockfd, receivebuf + receivesize, CONN_MTU);
			if (thispacketsize < 0 || errno == EAGAIN)
				thispacketsize = TLS_WANT_POLLIN;
		}
		
		if (thispacketsize == -1 || thispacketsize == 0) {
			log_writex(LOGTYPE_DEBUG, "client eof it seems");
			willteardown = 1;
			break;	
		} else if (thispacketsize == TLS_WANT_POLLIN || thispacketsize == TLS_WANT_POLLOUT) {
			log_writex(LOGTYPE_DEBUG, "waiting for poll");
			break;
		}

		receivesize += thispacketsize;
	}

	if (receivesize > 0) {

		/* first, reboot the connection so that if our client doesn't
		 * turn off reception (e.g. to flight an engine request), timeouts
		 * will occur appropriately
		 */
		conn_stopreceiving(c);
		conn_receive(c, c->cb_receive);

		if (c->incoming_message == NULL) {
			uint8_t	opcode;
	
			opcode = *(uint8_t *)receivebuf;
			c->incoming_message = netmsg_new(opcode);
	
			/* invalid argument -> bad opcode */
			if (c->incoming_message == NULL) {
				if (errno == EINVAL) {
					c->cb_receive(c, NULL);
					goto end;

				} else log_fatal("conn_doreceive: netmsg_new");
			}
		}
	
		if (netmsg_write(c->incoming_message, receivebuf, receivesize) != receivesize)
			log_fatalx("conn_doreceive: netmsg_write: %s",
				netmsg_error(c->incoming_message));
		
		if (!netmsg_isvalid(c->incoming_message, &unrecoverable)) {

			if (unrecoverable) {
				/* deliver as is, caller checks for validity and
				 * can discover errstr + work with it as desired
				 */
				c->cb_receive(c, c->incoming_message);

				netmsg_teardown(c->incoming_message);
				c->incoming_message = NULL;
			}

		} else {
			netmsg_clearerror(c->incoming_message);
			c->cb_receive(c, c->incoming_message);
			netmsg_teardown(c->incoming_message);
			c->incoming_message = NULL;
		}
	}

end:	
	free(receivebuf);
	if (willteardown)
		conn_teardown(c);

	(void)fd;
}

static void
conn_dosend(struct msgqueue *mq, struct conn *c)
{
	char		*rawmsg;
	struct netmsg	*sendmsg;	
	ssize_t		 sendsize, sendoffset, written;

	sendmsg = msgqueue_gethead(mq);
	if (sendmsg == NULL)
		log_fatalx("conn_dosend: fired when msgqueue empty somehow");

	sendsize = netmsg_seek(sendmsg, 0, SEEK_END);
	if (sendsize < 0) log_fatal("conn_dosend: netmsg_seek to end");

	sendoffset = (ssize_t)msgqueue_getcachedoffset(mq);
	sendsize -= sendoffset;

	if (netmsg_seek(sendmsg, sendoffset, SEEK_SET) < 0)
		log_fatal("conn_dosend: netmsg_seek to cached offset");

	rawmsg = reallocarray(NULL, sendsize, sizeof(char));
	if (rawmsg == NULL) log_fatal("conn_dosend: reallocarray");

	if (netmsg_read(sendmsg, rawmsg, sendsize) != sendsize)
		log_fatal("conn_dosend: netmsg_read failed to read %ld bytes", sendsize);

	if (globalcontext.mode == CONN_MODE_TLS)
		written = tls_write(c->tls_context, rawmsg, sendsize);
	else 
		written = write(c->sockfd, rawmsg, sendsize);

	if (written == -1 || written == 0)
		conn_teardown(c);

	else if (written == TLS_WANT_POLLIN || written == TLS_WANT_POLLOUT)
		msgqueue_setcachedoffset(mq, (size_t)sendoffset);

	else if (written < sendsize)
		msgqueue_setcachedoffset(mq, (size_t)(sendoffset + written));

	else msgqueue_deletehead(mq);

	free(rawmsg);
}


void
conn_listen(void (*cb)(struct conn *), uint16_t port, int mode)
{
	if (!globalcontext_initialized) globalcontext_init(mode);

	if (globalcontext.listen_fd > 0)
		log_fatalx("conn_listen: tried to listen twice in a row");

	globalcontext_listen(cb, port);
}

void
conn_teardown(struct conn *c)
{
	if (c->cb_teardown != NULL)
		c->cb_teardown(c);

	RB_REMOVE(conntree, &allcons, c);

	if (c->incoming_message != NULL)
		netmsg_teardown(c->incoming_message);

	msgqueue_teardown(c->outgoing);

	conn_stopreceiving(c);
	conn_canceltimeout(c);

	shutdown(c->sockfd, SHUT_RDWR);
	close(c->sockfd);

	if (c->tls_context != NULL)
		tls_free(c->tls_context);

	free(c);

	log_writex(LOGTYPE_DEBUG, "tore down connection");
}

void
conn_teardownall(void)
{
	globalcontext_stoplistening();

	while (!RB_EMPTY(&allcons)) {
		struct conn	*toremove;

		toremove = RB_MIN(conntree, &allcons);
		conn_teardown(toremove);
	}
	
	globalcontext_teardown();
}

void
conn_receive(struct conn *c, void (*cb)(struct conn *, struct netmsg *))
{
	short			 event = EV_READ | EV_PERSIST;
	struct timeval		*timeout = NULL;

	conn_stopreceiving(c);

	c->cb_receive = cb;

	if (!event_pending(&c->event_receive, EV_READ, NULL)) {
		if (c->cb_timeout != NULL) {
			timeout = &c->timeout;
			event |= EV_TIMEOUT;
		}

		event_set(&c->event_receive, c->sockfd, event, conn_doreceive, c);

		if (event_add(&c->event_receive, timeout) < 0)
			log_fatal("conn_receive: event_add");
	}
}

void
conn_stopreceiving(struct conn *c)
{
	if (event_pending(&c->event_receive, EV_READ, NULL))
		if (event_del(&c->event_receive) < 0)
			log_fatal("conn_stopreceiving: event_del");
}

void
conn_setteardowncb(struct conn *c, void (*cb)(struct conn *))
{
	c->cb_teardown = cb;
}

void
conn_settimeout(struct conn *c, struct timeval *timeout, void (*cb)(struct conn *))
{
	c->cb_timeout = cb;
	c->timeout = *timeout;

	if (event_pending(&c->event_receive, EV_READ, NULL))
		conn_receive(c, c->cb_receive);
}

void
conn_canceltimeout(struct conn *c)
{
	c->cb_timeout = NULL;

	if (event_pending(&c->event_receive, EV_READ, NULL))
		conn_receive(c, c->cb_receive);
}

int
conn_getfd(struct conn *c)
{
	return c->sockfd;
}

void
conn_send(struct conn *c, struct netmsg *msg)
{
	msgqueue_append(c->outgoing, msg);
}

struct sockaddr_in *
conn_getsockpeer(struct conn *c)
{
	struct sockaddr_in	*peerout;

	peerout = malloc(sizeof(struct sockaddr_in));
	if (peerout == NULL) goto end;

	*peerout = c->peer;
end:
	return peerout;
}
