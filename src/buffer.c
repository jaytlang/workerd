/* workerd network rpc format
 * (c) jay lang 2023
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "workerd.h"

/* buffers */

struct buffer {
	char	*buf;
	ssize_t	 offset;
	ssize_t	 capacity;
	ssize_t	 eof;
};

static struct buffer	*buffer_new(void);
static int		 buffer_accomodate(struct buffer *b, ssize_t count);

/* buffer lookup structure */

SLIST_HEAD(bufferlist, bufferdesc);
RB_HEAD(buffertree, bufferdesc);

struct bufferdesc {
	int		 descriptor;
	struct buffer	*backing;

	SLIST_ENTRY(bufferdesc)	freelist_entries;
	RB_ENTRY(bufferdesc)	inuse_entries;
};

static struct bufferdesc 	*bufferdesc_new(struct buffer *);
static void			 bufferdesc_markfree(struct bufferdesc *);

static struct bufferdesc	*bufferdesc_bufferforkey(int);
static int			 bufferdesc_compare(struct bufferdesc *, struct bufferdesc *);


RB_PROTOTYPE_STATIC(buffertree, bufferdesc, inuse_entries, bufferdesc_compare)

static struct buffertree	 bufferdesc_inuse = RB_INITIALIZER(&bufferdesc_inuse);


static struct bufferlist	 bufferdesc_freelist = SLIST_HEAD_INITIALIZER(bufferdesc_freelist);
static int			 bufferdesc_firstfreedescriptor = 0;


RB_GENERATE_STATIC(buffertree, bufferdesc, inuse_entries, bufferdesc_compare)

static struct bufferdesc *
bufferdesc_new(struct buffer *b)
{
	struct bufferdesc	*out = NULL;

	if (SLIST_EMPTY(&bufferdesc_freelist)) {
		out = malloc(sizeof(struct bufferdesc));	
		if (out == NULL) goto end;

		out->descriptor = bufferdesc_firstfreedescriptor++;

	} else {
		out = SLIST_FIRST(&bufferdesc_freelist);
		SLIST_REMOVE_HEAD(&bufferdesc_freelist, freelist_entries);
	}

	out->backing = b;

	RB_INSERT(buffertree, &bufferdesc_inuse, out);
end:
	return out;
}

static void
bufferdesc_markfree(struct bufferdesc *bd)
{
	bd->backing = NULL;

	RB_REMOVE(buffertree, &bufferdesc_inuse, bd);
	SLIST_INSERT_HEAD(&bufferdesc_freelist, bd, freelist_entries);
}

static struct bufferdesc *
bufferdesc_bufferforkey(int key)
{
	struct bufferdesc	find, *res = NULL;

	find.descriptor = key;
	res = RB_FIND(buffertree, &bufferdesc_inuse, &find);

	if (res == NULL) errno = EBADF;
	return res;
}

static int
bufferdesc_compare(struct bufferdesc *a, struct bufferdesc *b)
{
	int	 result = 0;

	if (a->descriptor > b->descriptor) result = 1;
	if (a->descriptor < b->descriptor) result = -1;

	return result;
}


static struct buffer *
buffer_new(void)
{
	struct buffer	*out = NULL;
	char		*buf;

	buf = recallocarray(NULL, 0, 1, sizeof(char));
	if (buf == NULL) goto end;

	out = malloc(sizeof(struct buffer));
	if (out == NULL) {
		free(buf);
		goto end;
	}

	out->offset = 0;
	out->eof = 0;
	out->capacity = 1;
	out->buf = buf;
end:
	return out;
}

static int
buffer_accomodate(struct buffer *b, ssize_t count)
{
	char	*newbuf;
	int	 status = -1;

	if (count <= b->capacity) return 0;

	newbuf = recallocarray(b->buf, b->capacity, count, sizeof(char));
	if (newbuf == NULL) goto end;

	b->buf = newbuf;
	b->capacity = count;

	status = 0;
end:
	return status;
}


int
buffer_open(void)
{
	struct buffer		*newbuffer = NULL;
	struct bufferdesc	*newdescriptor = NULL;
	int			 nfd = -1;

	newbuffer = buffer_new();
	if (newbuffer == NULL) goto end;

	newdescriptor = bufferdesc_new(newbuffer);
	if (newdescriptor == NULL) {
		free(newbuffer);
		goto end;
	}

	nfd = newdescriptor->descriptor;
end:
	return nfd;
}

int
buffer_close(int key)
{
	struct bufferdesc	*thisdesc;
	int			 status = -1;

	thisdesc = bufferdesc_bufferforkey(key);
	if (thisdesc == NULL) goto end;

	free(thisdesc->backing->buf);
	free(thisdesc->backing);

	bufferdesc_markfree(thisdesc);

	status = 0;
end:
	return status;
}

ssize_t
buffer_read(int key, void *out, size_t count)
{
	struct bufferdesc	*thisdesc;
	struct buffer		*thisbuffer;

	ssize_t			 canread, shouldread, didread = -1;

	if (count > SSIZE_MAX) {
		errno = EINVAL;
		goto end;
	}

	thisdesc = bufferdesc_bufferforkey(key);
	if (thisdesc == NULL) goto end;
	else thisbuffer = thisdesc->backing;

	canread = thisbuffer->eof - thisbuffer->offset;
	if (canread < 0) canread = 0;

	shouldread = (count > (size_t)canread) ? canread : (ssize_t)count;

	memcpy(out, thisbuffer->buf + thisbuffer->offset, shouldread);
	thisbuffer->offset += shouldread;

	didread = shouldread;
end:
	return didread;
}

ssize_t
buffer_write(int key, const void *in, size_t count)
{
	struct bufferdesc	*thisdesc;
	struct buffer		*thisbuffer;

	ssize_t	 		 written = -1;
	size_t 	 		 endoffset;

	if (count > SSIZE_MAX) {
		errno = EINVAL;
		goto end;
	}

	thisdesc = bufferdesc_bufferforkey(key);
	if (thisdesc == NULL) goto end;
	else thisbuffer = thisdesc->backing;

	endoffset = thisbuffer->offset + count;

	if (endoffset > SSIZE_MAX) {
		errno = EFBIG;
		goto end;

	} else if (endoffset > (size_t)thisbuffer->capacity) {
		if (buffer_accomodate(thisbuffer, endoffset) < 0)
			goto end;
	}

	memcpy(thisbuffer->buf + thisbuffer->offset, in, count);

	thisbuffer->offset += count;
	thisbuffer->eof = thisbuffer->offset;
	written = count;
end:
	return written;
}

int
buffer_truncate(int key, off_t length)
{
	struct bufferdesc	*thisdesc;
	struct buffer		*thisbuffer;

	char	*newbuf;
	int	 status = -1;

	thisdesc = bufferdesc_bufferforkey(key);
	if (thisdesc == NULL) goto end;
	else thisbuffer = thisdesc->backing;

	if (length < 0) {
		errno = EINVAL;
		goto end;
	} else if (length > SSIZE_MAX) {
		errno = EFBIG;
		goto end;
	}

	if (thisbuffer->capacity >= (ssize_t)length) {
		newbuf = recallocarray(thisbuffer->buf, thisbuffer->capacity,
			length, sizeof(char));
		if (newbuf == NULL) goto end;

		thisbuffer->buf = newbuf;

	} else {
		if (buffer_accomodate(thisbuffer, length) < 0)
			goto end;
	}

	thisbuffer->eof = thisbuffer->capacity = length;
	status = 0;
end:
	return status;
}

off_t
buffer_seek(int key, off_t offset, int whence)
{
	struct bufferdesc	*thisdesc;
	struct buffer		*thisbuffer;

	off_t	 		 position, returnposition = -1;

	thisdesc = bufferdesc_bufferforkey(key);
	if (thisdesc == NULL) goto end;
	else thisbuffer = thisdesc->backing;

	switch (whence) {
	case SEEK_SET:
		position = offset;
		break;
	case SEEK_CUR:
		position = thisbuffer->offset + offset;
		break;
	case SEEK_END:
		position = thisbuffer->eof + offset;
		break;
	default:
		errno = EINVAL;
		goto end;
	}

	if (position > thisbuffer->capacity)
		if (buffer_accomodate(thisbuffer, position) < 0)
			goto end;

	if (position < 0) {
		errno = EINVAL;
		goto end;
	}

	thisbuffer->offset = position;	
	returnposition = position;
end:
	return returnposition;	
}
