/* mechanism for the engine to communicate files
 * to the client without hitting the ipcmsg length cap
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "workerd.h"

struct wbfile {
	uint64_t		fileid;
	STAILQ_ENTRY(wbfile)	entries;
};

STAILQ_HEAD(wbfilelist, wbfile);

static char	*wbfile_reservepath(void);
static void	 wbfile_releasepath(char *);

static struct wbfilelist	freefiles = STAILQ_HEAD_INITIALIZER(freefiles);
static uint64_t			maxfileid = 0;

static char *
wbfile_reservepath(void)
{
	struct wbfile	*newfile;
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

	asprintf(&newpath, "%s/%llu", WRITEBACK, newid);
end:
	return newpath;
}

static void
wbfile_releasepath(char *oldpath)
{
	struct wbfile	*freefile;
	char		*pattern;
	uint64_t	 oldid;

	if (asprintf(&pattern, "%s/%%llu", WRITEBACK) < 0)
		log_fatal("wbfile_releasepath: asprintf for writeback file path");

	if (sscanf(oldpath, pattern, &oldid) != 1)
		log_fatalx("wbfile_releasepath: sscanf on %s failed to extract file id");

	free(pattern);
	free(oldpath);

	freefile = malloc(sizeof(struct wbfile));
	if (freefile == NULL)
		log_fatalx("file_releasepath: failed to allocate free file description");

	freefile->fileid = oldid;
	STAILQ_INSERT_HEAD(&freefiles, freefile, entries);
}

char *
wbfile_writeback(char *name, char *data, size_t datasize)
{
	char		*path;
	size_t		 namesize;
	uint64_t	 benamesize, bedatasize;

	char		*writebuffer, *p;
	size_t		 writebuffersize;

	int	 	 fd;
	int	 	 flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
	int	 	 mode = S_IRUSR | S_IWUSR | S_IRGRP;

	namesize = strlen(name);
	benamesize = htobe64((uint64_t)namesize);
	bedatasize = htobe64((uint64_t)datasize);

	if (namesize > MAXNAMESIZE)
		log_fatalx("wbfile_writeback: passed name %s too long (length %lu)", name, namesize);
	else if (datasize > MAXFILESIZE)
		log_fatalx("wbfile_writeback: passed data too long (length %lu)", datasize);

	writebuffersize = 2 * sizeof(uint64_t) + namesize + datasize;

	p = writebuffer = reallocarray(NULL, writebuffersize, sizeof(char));
	if (writebuffer == NULL) log_fatal("wbfile_writeback: reallocarray write buffer");

	memcpy(p, &benamesize, sizeof(uint64_t));
	p += sizeof(uint64_t);

	memcpy(p, name, namesize);
	p += namesize;

	memcpy(p, &bedatasize, sizeof(uint64_t));
	p += sizeof(uint64_t);

	memcpy(p, data, datasize);

	path = wbfile_reservepath();
	if (path == NULL) log_fatal("wbfile_writeback: wbfile_reservepath");

	if ((fd = open(path, flags, mode)) < 0)
		log_fatal("wbfile_writeback: open %s for writing", path);

	if (write(fd, writebuffer, writebuffersize) != (ssize_t)writebuffersize)
		log_fatal("wbfile_writeback: write writebuffer to disk");

	close(fd);
	free(writebuffer);

	return path;
}

void
wbfile_readout(char *path, char **name, char **data, size_t *datasize)
{
	uint64_t	besize;
	size_t		namesize;

	int	fd;
	int	flags = O_RDONLY | O_CLOEXEC;

	if ((fd = open(path, flags)) < 0)
		log_fatal("wbfile_readout: open %s for reading", path);

	if (read(fd, &besize, sizeof(uint64_t)) < (ssize_t)sizeof(uint64_t))
		log_fatal("wbfile_readout: read name size");

	namesize = be64toh(besize);
	*name = reallocarray(NULL, namesize + 1, sizeof(char));
	if (*name == NULL) log_fatal("wbfile_readout: reallocarray name buffer");

	if (read(fd, *name, namesize) < (ssize_t)namesize)
		log_fatal("wbfile_readout: read name");

	(*name)[namesize] = '\0';

	if (read(fd, &besize, sizeof(uint64_t)) < (ssize_t)sizeof(uint64_t))
		log_fatal("wbfile_readout: read data size");

	*datasize = be64toh(besize);
	*data = reallocarray(NULL, *datasize, sizeof(char));
	if (*data == NULL) log_fatal("wbfile_readout: reallocarray data buffer");

	if (read(fd, *data, *datasize) < *(ssize_t *)datasize)
		log_fatal("wbfile_readout: read data");

	close(fd);
}

void
wbfile_teardown(char *path)
{
	if (unlink(path) < 0) log_fatal("wbfile_teardown: unlink %s", path);
	wbfile_releasepath(path);
}
