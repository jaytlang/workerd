/* bundled network rpc receipt system
 * (c) jay lang 2023
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "workerd.h"

/* disk file metadata */

struct msgfile {
	uint64_t		 fileid;
	STAILQ_ENTRY(msgfile)	 entries;
};

STAILQ_HEAD(msgfilelist, msgfile);

static char	*msgfile_reservepath(void);
static void	 msgfile_releasepath(char *);

static struct msgfilelist	freefiles = STAILQ_HEAD_INITIALIZER(freefiles);
static uint64_t			maxfileid = 0;


/* netmsg proper */


struct netmsg {
	uint8_t	 	  opcode;
	char		 *path;
	int	 	  descriptor;

	int		  retain;

	int		(*closestorage)(int);
	ssize_t		(*readstorage)(int, void *, size_t);
	ssize_t		(*writestorage)(int, const void *, size_t);
	off_t		(*seekstorage)(int, off_t, int);
	int		(*truncatestorage)(int, off_t);

	char		  errstr[ERRSTRSIZE];
};

static int	netmsg_getclaimedlabelsize(struct netmsg *, uint64_t *);
static int	netmsg_getclaimeddatasize(struct netmsg *, uint64_t *);
static ssize_t	netmsg_getexpectedsizeifvalid(struct netmsg *);

static void	netmsg_committype(struct netmsg *);

static char *
msgfile_reservepath(void)
{
	struct msgfile	*newfile;
	char		*newpath = NULL;
	uint64_t	 newid;

	if (STAILQ_EMPTY(&freefiles)) {
		if (maxfileid == UINT64_MAX) {
			errno = EMFILE;
			goto end;
		}

		newid = maxfileid++;

	} else {
		newfile = STAILQ_FIRST(&freefiles);
		STAILQ_REMOVE_HEAD(&freefiles, entries);

		newid = newfile->fileid;
		free(newfile);
	}

	asprintf(&newpath, "%s/%llu", MESSAGES, newid);
end:
	return newpath;
}

static void
msgfile_releasepath(char *oldpath)
{
	struct msgfile	*freefile;
	char		*pattern;
	uint64_t	 oldid;

	if (asprintf(&pattern, "%s/%%llu", MESSAGES) < 0)
		log_fatal("msgfile_releasepath: asprintf for message path");

	if (sscanf(oldpath, pattern, &oldid) != 1)
		log_fatalx("msgfile_releasepath: sscanf on %s failed to extract file id");

	free(pattern);
	free(oldpath);

	freefile = malloc(sizeof(struct msgfile));
	if (freefile == NULL)
		log_fatalx("msgfile_releasepath: failed to allocate free file description");

	freefile->fileid = oldid;
	STAILQ_INSERT_HEAD(&freefiles, freefile, entries);
}

struct netmsg *
netmsg_new(uint8_t opcode)
{
	struct netmsg	*out = NULL;
	char		*path = NULL;
	int		 descriptor = -1;
	int		 error = 0;

	int		 diskmsg = 0;
	int		 flags = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC;
	mode_t		 mode = S_IRUSR | S_IWUSR | S_IRGRP;

	switch (opcode) {
	case NETOP_SENDFILE:
		diskmsg = 1;
		path = msgfile_reservepath();

		if (path == NULL) {
			error = 1;
			goto end;
		}

		descriptor = open(path, flags, mode);
		if (descriptor < 0) {
			error = 1;
			goto end;
		}

		break;

	case NETOP_SENDLINE:
	case NETOP_REQUESTLINE:
	case NETOP_TERMINATE:
	case NETOP_ERROR:
	case NETOP_ACK:
	case NETOP_HEARTBEAT:
		descriptor = buffer_open();
		break;

	default:
		errno = EINVAL;
		goto end;
	}

	out = calloc(1, sizeof(struct netmsg));
	if (out == NULL) goto end;

	out->opcode = opcode;
	out->descriptor = descriptor;
	out->path = path;

	if (diskmsg) {
		out->closestorage = close;
		out->readstorage = read;
		out->writestorage = write;
		out->seekstorage = lseek;
		out->truncatestorage = ftruncate;
	} else {
		out->closestorage = buffer_close;
		out->readstorage = buffer_read;
		out->writestorage = buffer_write;
		out->seekstorage = buffer_seek;
		out->truncatestorage = buffer_truncate;
	}

	/* ensure that the struct stays consistent
	 * with the marshalled in-memory data
	 */
	netmsg_committype(out);

end:
	if (error) {
		if (descriptor >= 0) {
			if (diskmsg) close(descriptor);
			else buffer_close(descriptor);
		}

		if (path != NULL) {
			unlink(path);
			msgfile_releasepath(path);
		}
	}

	return out;
}

struct netmsg *
netmsg_loadweakly(char *path)
{
	struct netmsg	*out = NULL;
	int		 loadfd;
	uint8_t		 opcode;

	loadfd = open(path, O_RDONLY);
	if (loadfd < 0) goto end;

	if (read(loadfd, &opcode, sizeof(uint8_t)) != sizeof(uint8_t))
		goto end;
	else if (lseek(loadfd, 0, SEEK_SET) != 0)
		goto end;

	out = calloc(1, sizeof(struct netmsg));
	if (out == NULL) goto end;

	out->opcode = opcode;
	out->descriptor = loadfd;

	out->closestorage = close;
	out->readstorage = read;
	out->writestorage = write;
	out->seekstorage = lseek;
	out->truncatestorage = ftruncate;

end:
	if (out == NULL)
		if (loadfd > 0) close(loadfd);

	return out;
}

void
netmsg_retain(struct netmsg *m)
{
	m->retain++;
}

void
netmsg_teardown(struct netmsg *m)
{
	if (m->retain > 0)
		m->retain--;

	else {
		m->closestorage(m->descriptor);

		if (m->path != NULL) {
			unlink(m->path);
			msgfile_releasepath(m->path);
		}

		free(m);
	}
}

const char *
netmsg_error(struct netmsg *m)
{
	return m->errstr;
}

void
netmsg_clearerror(struct netmsg *m)
{
	*m->errstr = '\0';
}

ssize_t
netmsg_write(struct netmsg *m, void *bytes, size_t count)
{
	ssize_t	status;

	status = m->writestorage(m->descriptor, bytes, count);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), ERRSTRSIZE);

	return status;
}

ssize_t
netmsg_read(struct netmsg *m, void *bytes, size_t count)
{
	ssize_t	status;

	status = m->readstorage(m->descriptor, bytes, count);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), ERRSTRSIZE);

	return status;
}

ssize_t
netmsg_seek(struct netmsg *m, ssize_t offset, int whence)
{
	ssize_t status;

	status = m->seekstorage(m->descriptor, offset, whence);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), ERRSTRSIZE);

	return status;
}

int
netmsg_truncate(struct netmsg *m, ssize_t offset)
{
	ssize_t status;

	status = m->truncatestorage(m->descriptor, offset);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), ERRSTRSIZE);

	return status;	
}

static int
netmsg_getclaimedlabelsize(struct netmsg *m, uint64_t *out)
{
	ssize_t		offset, bytesread;
	int		status = -1;

	offset = sizeof(uint8_t);

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) != offset)
		log_fatal("netmsg_getclaimedlabelsize: could not seek to %lu", offset);

	bytesread = m->readstorage(m->descriptor, out, sizeof(uint64_t));

	if (bytesread < 0)
		log_fatal("netmsg_getclaimedlabelsize: could not read buffer");
	else if (bytesread < (ssize_t)sizeof(uint64_t)) {
		errno = EINPROGRESS;
		goto end;
	}

	*out = be64toh(*out);

	if (*out > MAXNAMESIZE) {
		errno = ERANGE;
		goto end;
	}

	status = 0;
end:
	return status;
}

static int
netmsg_getclaimeddatasize(struct netmsg *m, uint64_t *out)
{
	uint64_t	labelsize;
	ssize_t		offset, bytesread;
	int		status = -1;

	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) goto end;
	else offset = sizeof(uint8_t) + sizeof(uint64_t) + labelsize;

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
		log_fatal("netmsg_getclaimeddatasize: could not seek to %lu", offset);

	bytesread = m->readstorage(m->descriptor, out, sizeof(uint64_t));

	if (bytesread < 0)
		log_fatal("netmsg_getclaimeddatasize: could not read buffer");
	else if (bytesread < (ssize_t)sizeof(uint64_t)) {
		errno = EINPROGRESS;
		goto end;
	}

	*out = be64toh(*out);

	if (*out > MAXFILESIZE) {
		errno = ERANGE;
		goto end;
	}

	status = 0;
end:
	return status;
}

static ssize_t
netmsg_getexpectedsizeifvalid(struct netmsg *m)
{
	uint64_t	scratchsize;
	ssize_t		total = -1;

	/* XXX: be careful, this assumes the message is valid */

	total = sizeof(uint8_t);

	if (netmsg_getclaimedlabelsize(m, &scratchsize) == 0)
		total += (ssize_t)scratchsize + sizeof(uint64_t);

	if (netmsg_getclaimeddatasize(m, &scratchsize) == 0)
		total += (ssize_t)scratchsize + sizeof(uint64_t);

	netmsg_clearerror(m);
	return total;
}

static void
netmsg_committype(struct netmsg *m)
{
	ssize_t		byteswritten;

	if (m->seekstorage(m->descriptor, 0, SEEK_SET) < 0)
		log_fatal("netmsg_committype: could not seek to start of buffer");

	byteswritten = m->writestorage(m->descriptor, &m->opcode, sizeof(uint8_t));

	if (byteswritten < 0)
		log_fatal("netmsg_committype: could not write buffer");
	else if (byteswritten < (ssize_t)sizeof(uint8_t))
		log_fatalx("netmsg_committype: could not flush opcode to buffer");

	if (m->seekstorage(m->descriptor, 0, SEEK_SET) != 0)
		log_fatal("netmsg_committype: could not seek message to start post-type-commit");
}

uint8_t
netmsg_gettype(struct netmsg *m)
{
	return m->opcode;
}

char *
netmsg_getpath(struct netmsg *m)
{
	char	*pathout;

	pathout = strdup(m->path);
	if (pathout == NULL) log_fatal("netmsg_getpath: strdup");

	return pathout;
}

char *
netmsg_getlabel(struct netmsg *m)
{
	char		*out = NULL;
	ssize_t		 bytesread, offset;
	uint64_t	 labelsize;

	offset = sizeof(uint8_t) + sizeof(uint64_t);
	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) {
		snprintf(m->errstr, ERRSTRSIZE,
			"netmsg_getlabel: netmsg_getclaimedlabelsize: %s", strerror(errno));
		goto end;
	}

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
		log_fatal("netmsg_getlabel: could not seek to %lu", offset);

	out = reallocarray(NULL, labelsize + 1, sizeof(char));
	if (out == NULL)
		log_fatal("netmsg_getlabel: reallocarray for netmsg label copy");

	bytesread = m->readstorage(m->descriptor, out, labelsize);

	if (bytesread < 0)
		log_fatal("netmsg_getlabel: could not read buffer");

	out[bytesread] = '\0';
end:
	return out;
}


int
netmsg_setlabel(struct netmsg *m, char *newlabel)
{
	char		*datacopy = NULL;
	uint64_t	 labelsize, datacopysize = 0;
	uint64_t	 newlabelsize, benewlabelsize;

	int		 status = -1;

	/* if there's already a label here, there also might be
	 * some data...
	 */

	newlabelsize = strlen(newlabel);

	if (newlabelsize > MAXNAMESIZE) {
		snprintf(m->errstr, ERRSTRSIZE,
			"new label size %llu exceeds allowed maximum", newlabelsize);
		goto end;
	}

	benewlabelsize = htobe64(newlabelsize);

	if (netmsg_getclaimedlabelsize(m, &labelsize) == 0) {
		ssize_t	totalsize, offset;

		totalsize = m->seekstorage(m->descriptor, 0, SEEK_END);
		if (totalsize < 0) log_fatal("netmsg_setlabel: failed to find eof");

		offset = labelsize + sizeof(uint64_t) + sizeof(uint8_t);
		datacopysize = totalsize - offset;

		if (datacopysize > 0) {
			datacopy = reallocarray(NULL, datacopysize, sizeof(char));
			if (datacopy == NULL) log_fatal("netmsg_setlabel: reallocarray datacopy");

			if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
				log_fatal("netmsg_setlabel: could not seek to data to be backed up");

			if (m->readstorage(m->descriptor, datacopy, datacopysize)
				!= (ssize_t)datacopysize)

				log_fatal("netmsg_setlabel: could not read out data to be backed up");
		}
	}

	if (m->truncatestorage(m->descriptor, sizeof(uint8_t)) < 0)
		log_fatal("netmsg_setlabel: failed to truncate buffer down before relabel");

	if (m->seekstorage(m->descriptor, 0, SEEK_END) < 0)
		log_fatal("netmsg_setlabel: failed to seek to end of truncated buffer");

	if (m->writestorage(m->descriptor, &benewlabelsize, sizeof(uint64_t))
		!= sizeof(uint64_t))

		log_fatal("netmsg_setlabel: failed to write new label size");

	if (m->writestorage(m->descriptor, newlabel, newlabelsize) != (ssize_t)newlabelsize)
		log_fatal("netmsg_setlabel: failed to write new label");

	if (datacopysize > 0) {
		if (m->writestorage(m->descriptor, datacopy, datacopysize) !=
			(ssize_t)datacopysize)

			log_fatal("netmsg_setlabel: failed to restore backed up data");

		free(datacopy);
	}

	status = 0;
end:
	return status;
}

char *
netmsg_getdata(struct netmsg *m, uint64_t *sizeout)
{
	char		*out = NULL;
	uint64_t	 datasize, labelsize;
	ssize_t		 bytesread, offset;

	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) {
		snprintf(m->errstr, ERRSTRSIZE,
			"netmsg_getdata: netmsg_getclaimedlabelsize: %s", strerror(errno));
		goto end;

	} else if (netmsg_getclaimeddatasize(m, &datasize) < 0) {
		snprintf(m->errstr, ERRSTRSIZE,
			"netmsg_getdata: netmsg_getclaimeddatasize: %s", strerror(errno));
		goto end;
	}

	offset = sizeof(uint8_t) + sizeof(uint64_t) + labelsize + sizeof(uint64_t);

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
		log_fatal("netmsg_getdata: failed to seek to start of data");

	out = reallocarray(NULL, datasize, sizeof(char));
	if (out == NULL)
		log_fatal("netmsg_getdata: reallocarray for read data failed");

	bytesread = m->readstorage(m->descriptor, out, datasize);

	if (bytesread < 0)
		log_fatal("netmsg_getdata: could not read buffer");

	*sizeout = bytesread;
end:
	return out;
}

int
netmsg_setdata(struct netmsg *m, char *newdata, uint64_t datasize)
{
	uint64_t	labelsize, bedatasize;
	ssize_t		offset;
	int		status = -1;	

	if (datasize > MAXFILESIZE) {
		snprintf(m->errstr, ERRSTRSIZE,
			"new data size %llu exceeds allowed maximum", datasize);
		goto end;
	}

	bedatasize = htobe64(datasize);

	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) {
		snprintf(m->errstr, ERRSTRSIZE,
			"netmsg_setdata: netmsg_getclaimedlabelsize: %s", strerror(errno));
		goto end;
	}

	offset = sizeof(uint8_t) + sizeof(uint64_t) + labelsize;

	if (m->truncatestorage(m->descriptor, offset) < 0)
		log_fatal("netmsg_setdata: failed to truncate buffer to type+label");

	if (m->seekstorage(m->descriptor, 0, SEEK_END) < 0)
		log_fatal("netmsg_setdata: failed to seek to end of label");

	if (m->writestorage(m->descriptor, &bedatasize, sizeof(uint64_t))
		!= sizeof(uint64_t))

		log_fatal("netmsg_setdata: failed to write new data size");

	if (m->writestorage(m->descriptor, newdata, datasize) != (ssize_t)datasize)
		log_fatal("netmsg_setdata: failed to write new data");

	status = 0;
end:
	return status;
}

int
netmsg_isvalid(struct netmsg *m, int *fatal)
{
	char		*copied;
	uint64_t	 claimedsize, copiedsize;
	int		 needlabel, needdata, status = 0;

	ssize_t		 actualtypesize;
	uint8_t		 actualtype;

	ssize_t		 actualmessagesize, calculatedmessagesize;

	/* usually, validity failures are not fatal, i.e.
	 * more data can resolve the issue at hand
	 */
	*fatal = 0;

	switch (m->opcode) {
	case NETOP_SENDFILE:
		needlabel = 1;
		needdata = 1;
		break;

	case NETOP_SENDLINE:
	case NETOP_ERROR:
		needlabel = 1;
		needdata = 0;
		break;

	case NETOP_REQUESTLINE:
	case NETOP_TERMINATE:
	case NETOP_ACK:
	case NETOP_HEARTBEAT:
		needlabel = 0;
		needdata = 0;
		break;

	default:
		snprintf(m->errstr, ERRSTRSIZE, "illegal message type %d", m->opcode);
		*fatal = 1;
		goto end;
	}

	if (m->seekstorage(m->descriptor, 0, SEEK_SET) < 0)
		log_fatal("netmsg_isvalid: failed to seek to start of message to check type");

	actualtypesize = m->readstorage(m->descriptor, &actualtype, sizeof(uint8_t));

	if (actualtypesize < 0)
		log_fatal("netmsg_isvalid: failed to pull actual type off message");

	else if (actualtypesize != sizeof(uint8_t)) {
		strncpy(m->errstr, "netmsg_isvalid: complete message type not present",
			ERRSTRSIZE);
		goto end;

	} else if (actualtype != m->opcode) {
		snprintf(m->errstr, ERRSTRSIZE,
			"cached opcode %u doesn't match marshalled opcode %u",
			m->opcode, actualtype);
		*fatal = 1;
		goto end;
	}

	if (needlabel) {
		if (netmsg_getclaimedlabelsize(m, &claimedsize) < 0) {
			snprintf(m->errstr, ERRSTRSIZE,
				"netmsg_isvalid: netmsg_getclaimedlabelsize: %s", strerror(errno));

			if (errno == ERANGE) *fatal = 1;
			goto end;
		}

		copied = netmsg_getlabel(m);
		if (copied == NULL) goto end;

		copiedsize = strlen(copied);
		free(copied);

		if (copiedsize != claimedsize) {
			snprintf(m->errstr, ERRSTRSIZE, "claimed label size %llu != actual label strlen %llu",
				claimedsize, copiedsize);
			goto end;
		}
	}

	if (needdata) {
		if (netmsg_getclaimeddatasize(m, &claimedsize) < 0) {
			snprintf(m->errstr, ERRSTRSIZE,
				"netmsg_isvalid: netmsg_getclaimeddatasize: %s", strerror(errno));

			if (errno == ERANGE) *fatal = 1;
			goto end;
		}

		copied = netmsg_getdata(m, &copiedsize);
		if (copied == NULL) goto end;

		free(copied);

		if (copiedsize != claimedsize) {
			strncpy(m->errstr, "claimed data size != actual data size",
				ERRSTRSIZE);
			goto end;
		}
	}

	calculatedmessagesize = netmsg_getexpectedsizeifvalid(m);
	actualmessagesize = m->seekstorage(m->descriptor, 0, SEEK_END);

	if (actualmessagesize < 0)
		log_fatal("netmsg_isvalid: seek for actual message size");

	else if (actualmessagesize != calculatedmessagesize) {
		snprintf(m->errstr, ERRSTRSIZE,
			"claimed message size %ld != actual message size %ld",
			calculatedmessagesize, actualmessagesize);

		*fatal = 1;
		goto end;
	}

	status = 1;
end:
	return status;
}
