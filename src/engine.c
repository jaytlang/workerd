/* workerd true engine
 * (c) jay lang, 2023ish
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>

#include <errno.h>
#include <event.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "workerd.h"

static void	engine_sendtofrontend(int, uint32_t, char *);
static void	proc_getmsgfromfrontend(int, int, struct ipcmsg *);

static void	vm_print(uint32_t, char *);
static void	vm_readline(uint32_t);
static void	vm_commitfile(uint32_t, char *, char *, size_t);
static void	vm_signaldone(uint32_t);
static void	vm_reporterror(uint32_t, char *);

static struct vm_interface vmi = {	.print = vm_print,
					.readline = vm_readline,
					.commitfile = vm_commitfile,
					.signaldone = vm_signaldone,
					.reporterror = vm_reporterror };

static void
engine_sendtofrontend(int type, uint32_t key, char *data)
{
	struct ipcmsg	*response;

	response = ipcmsg_new(key, data);
	if (response == NULL) log_fatal("engine_sendtofrontend: ipcmsg_new");

	myproc_send(PROC_FRONTEND, type, -1, response);
	ipcmsg_teardown(response);
}

static void
vm_print(uint32_t key, char *msg)
{
	engine_sendtofrontend(IMSG_SENDLINE, key, msg);
}

static void
vm_readline(uint32_t key)
{
	engine_sendtofrontend(IMSG_REQUESTLINE, key, NULL);
}

static void
vm_commitfile(uint32_t key, char *fname, char *fdata, size_t fdatasize)
{
	struct vm	*v;
	char		*wbpath;

	if ((v = vm_fromkey(key)) == NULL)
		if (v == NULL) log_fatal("vm_commitfile: vm_fromkey");

	log_writex(LOGTYPE_DEBUG, "committing file %s!", fname);
	wbpath = wbfile_writeback(fname, fdata, fdatasize);
	vm_setaux(v, wbpath);
	engine_sendtofrontend(IMSG_SENDFILE, key, wbpath);
}

static void
vm_signaldone(uint32_t key)
{
	log_writex(LOGTYPE_DEBUG, "requesting termination");
	engine_sendtofrontend(IMSG_REQUESTTERM, key, NULL);
}

static void
vm_reporterror(uint32_t key, char *error)
{
	engine_sendtofrontend(IMSG_ERROR, key, error);
}

static void
proc_getmsgfromfrontend(int type, int fd, struct ipcmsg *msg)
{
	struct netmsg	*weakmsg;
	struct vm	*v;

	char		*msgtext;
	char		*wbfile;
	char		*fname, *fdata;

	uint64_t	 fdatasize;
	uint32_t	 key;

	msgtext = ipcmsg_getmsg(msg);
	key = ipcmsg_getkey(msg);

	log_writex(LOGTYPE_DEBUG, "message type %d -> key %u", type, key);

	if (type != IMSG_PUTARCHIVE)
		if ((v = vm_fromkey(key)) == NULL)
			log_fatal("proc_getmsgfromfrontend: vm_fromkey");

	switch (type) {
	case IMSG_PUTARCHIVE:
		v = vm_claim(key, vmi);
		if (v == NULL) {
			engine_sendtofrontend(IMSG_ERROR, key,
				"no worker machines are available right now, try again later");
			break;
		}

		weakmsg = netmsg_loadweakly(msgtext);

		/* XXX: same race condition as in bundled. if the frontend tears
		 * down the netmsg before we are able to load it, do nothing
		 */
		if (weakmsg == NULL) {
			if (errno == ENOENT) break;
			else log_fatal("proc_getmsgfromfrontend: netmsg_loadweakly");
		}

		fname = netmsg_getlabel(weakmsg);
		if (fname == NULL)
			log_fatalx("proc_getmsgfromfrontend: netmsg_getlabel: %s",
				netmsg_error(weakmsg));

		fdata = netmsg_getdata(weakmsg, &fdatasize);
		if (fdata == NULL)
			log_fatalx("proc_getmsgfromfrontend: netmsg_getdata: %s",
				netmsg_error(weakmsg));
		
		vm_injectfile(v, fname, fdata, (size_t)fdatasize);

		engine_sendtofrontend(IMSG_INITIALIZED, key, NULL);

		free(fname);
		free(fdata);
		netmsg_teardown(weakmsg);
		break;

	case IMSG_SENDLINE:
		vm_injectline(v, msgtext);
		break;

	case IMSG_CLIENTACK:
		wbfile = (char *)vm_clearaux(v);
		if (wbfile != NULL) {
			log_writex(LOGTYPE_DEBUG, "td");
			wbfile_teardown(wbfile);
		}

		vm_injectack(v);
		break;

	case IMSG_TERMINATE:
		wbfile = (char *)vm_clearaux(v);
		if (wbfile != NULL) {
			log_writex(LOGTYPE_DEBUG, "td");
			wbfile_teardown(wbfile);
		}

		vm_release(v);
		break;

	default:
		log_fatalx("proc_getmsgfromfrontend: bad message received from frontend: %d", type);
	}

	free(msgtext);
	(void)fd;
}

void
engine_launch(void)
{
	if (unveil(WRITEBACK, "rwc") < 0)
		log_fatal("unveil %s", WRITEBACK);
	else if (unveil(FRONTEND_MESSAGES, "r") < 0)
		log_fatal("unveil %s", FRONTEND_MESSAGES);
	else if (unveil(ENGINE_MESSAGES, "rwc") < 0)
		log_fatal("unveil %s", ENGINE_MESSAGES);
	else if (unveil(DISKS, "c") < 0)
		log_fatal("unveil %s", DISKS);

	if (unveil(VMCTL_PATH, "x") < 0)
		log_fatal("unveil %s", VMCTL_PATH);
	else if (unveil("/usr/libexec/ld.so", "r") < 0)
		log_fatal("unveil ld.so");

	if (pledge("stdio rpath wpath cpath proc exec inet", NULL) < 0)
		log_fatal("pledge");

	vm_init();

	myproc_listen(PROC_PARENT, nothing);
	myproc_listen(PROC_FRONTEND, proc_getmsgfromfrontend);

	event_dispatch();
	vm_killall();
}

__dead void
engine_signal(int signal, short event, void *arg)
{
	vm_killall();
	exit(0);

	(void)signal;
	(void)event;
	(void)arg;
}
