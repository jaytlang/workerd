/* workerd true engine
 * (c) jay lang, 2023ish
 */

#include <sys/types.h>
#include <sys/time.h>

#include <errno.h>
#include <stdlib.h>

#include "workerd.h"

static void	engine_sendtofrontend(int, uint32_t, char *);

static void	proc_getmsgfromparent(int, int, struct ipcmsg *);
static void	proc_getmsgfromfrontend(int, int, struct ipcmsg *);

static void	vm_print(uint32_t, char *);
static void	vm_readline(uint32_t);
static void	vm_loadfile(uint32_t, char *);
static void	vm_commitfile(uint32_t, char *, char *, size_t);
static void	vm_signaldone(uint32_t);
static void	vm_reporterror(uint32_t, char *);

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
proc_getmsgfromfrontend(int type, int fd, struct ipcmsg *msg)
{
	struct netmsg	*weakmsg;
	struct vm	*v;

	char		*msgtext;
	char		*fname, fdata;
	uint64_t	 fdatasize;
	uint32_t	 key;

	msgtext = ipcmsg_getmsg(msg);
	key = ipcmsg_getkey(msg);
	v = vm_fromkey(key);

	if (v == NULL) log_fatal("proc_getmsgfromfrontend: vm_fromkey");

	switch (type) {
	case IMSG_SENDLINE:
		vm_injectline(v, msgtext);
		break;

	case IMSG_SENDFILE:
		/* XXX: same race condition as in bundled. if the frontend tears
		 * down the netmsg before we are able to load it, as part of killing
		 * the associated connection, reply to the frontend with an ENGINEERROR
		 * that will be silently ignored. an IMSG_TERMINATE message is on the way.
		 */
		weakmsg = netmsg_loadweakly(msgfile);
		if (weakmsg == NULL) {
			if (errno == ENOENT) {
				engine_sendtofrontend(IMSG_ENGINEERROR, key, strerror(errno));
				break;
			}

			log_fatal("proc_getmsgfromfrontend: netmsg_loadweakly");
		}

		fname = netmsg_getlabel(weakmsg);
		if (fname == NULL)
			log_fatalx("proc_getmsgfromfrontend: netmsg_getlabel: %s",
				netmsg_error(weakmsg));

		fdata = netmsg_getdata(weakmsg, &fdatasize);
		if (fdata == NULL)
			log_fatalx("proc_getmsgfromfrontend: netmsg_getdata: %s",
				netmsg_error(weakmsg));
		
		vm_injectfile(v, fname, fdata, (size_t)fdatasize) < 0);
		free(fname);
		free(fdata);
		break;

	case IMSG_CLIENTACK:
		vm_injectack(v);
		break;

	case IMSG_TERMINATE:
		vm_release(v);
		break;

	default:
		log_fatalx("proc_getmsgfromfrontend: bad message received from frontend: %d", type);
	}

	free(msgtext);
	(void)fd;
}

static void
proc_getmsgfromparent(int type, int fd, struct ipcmsg *msg)
{
	struct vm	*v;
	struct archive	*archive;
	char		*archivepath;
	uint32_t	 key;

	archivepath = ipcmsg_getmsg(msg);
	key = ipcmsg_getkey(msg);

	if (type != IMSG_NEWJOB)
		log_fatalx("proc_getmsgfromparent: bad message received from parent: %d", type);

	archive = archive_fromfile(key, archivepath);
	if (archive == NULL)
		log_fatal("proc_getmsgfromparent: archive_fromfile");

	
}
