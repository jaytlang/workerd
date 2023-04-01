/* workerd ipc message format
 * (c) jay lang 2023
 */

#include <sys/types.h>

#include <arpa/inet.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "workerd.h"

struct ipcmsg {
	uint32_t	key;
	uint16_t	msgsize;
	char		msg[];
};

struct ipcmsg *
ipcmsg_new(uint32_t key, char *msg)
{
	struct ipcmsg	*out = NULL;
	uint16_t 	 allocsize, msgsize;

	if (msg != NULL) msgsize = strlen(msg) + 1;
	else msgsize = 1;

	allocsize = sizeof(struct ipcmsg) + msgsize;

	if (allocsize > UINT16_MAX) {
		errno = EINVAL;
		goto end;
	}

	out = malloc(allocsize);
	if (out == NULL) goto end;

	out->key = key;
	out->msgsize = msgsize;

	if (msgsize == 1) *out->msg = '\0';
	else memcpy(out->msg, msg, msgsize * sizeof(char));

end:
	return out;
}

uint32_t
ipcmsg_getkey(struct ipcmsg *i)
{
	return i->key;
}

char *
ipcmsg_getmsg(struct ipcmsg *i)
{
	char	*out;

	out = strdup(i->msg);
	return out;
}

struct ipcmsg *
ipcmsg_unmarshal(char *bytes, uint16_t count)
{
	uint32_t	 key;
	char		*msg;

	key = ntohl(*(uint32_t *)bytes);
	msg = bytes + sizeof(uint32_t) + sizeof(uint16_t);

	if (bytes[count - 1] != '\0')
		log_fatalx("illegal message received - no null terminator");

	(void)count;
	return ipcmsg_new(key, msg);
}

void
ipcmsg_teardown(struct ipcmsg *i)
{
	free(i);
}

char *
ipcmsg_marshal(struct ipcmsg *i, uint16_t *msgsizeout)
{
	char		*p, *buf = NULL;
	uint16_t	 bufsize;

	uint32_t	 marshalled_key;
	uint16_t	 marshalled_msgsize;

	bufsize = sizeof(struct ipcmsg) + i->msgsize;
	p = buf = reallocarray(NULL, bufsize, sizeof(char));

	if (buf == NULL) goto end;

	marshalled_key = htonl(i->key);
	marshalled_msgsize = htons(i->msgsize);

	memcpy(p, &marshalled_key, sizeof(uint32_t));
	p += sizeof(uint32_t);

	memcpy(p, &marshalled_msgsize, sizeof(uint16_t));
	p += sizeof(uint16_t);

	memcpy(p, i->msg, i->msgsize * sizeof(char));

	*msgsizeout = i->msgsize + sizeof(uint32_t) + sizeof(uint16_t);
end:
	return buf;
}
