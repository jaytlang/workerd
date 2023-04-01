/* bundled archive format
 * (c) jay lang, 2023ish
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/tree.h>

#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

#include "workerd.h"

struct archivefile {
	char			*name;
	size_t			 offset;

	RB_ENTRY(archivefile)	 entries;
};

static struct archivefile	*archivefile_new(char *, size_t);
static void			 archivefile_teardown(struct archivefile *);
static int			 archivefile_compare(struct archivefile *, struct archivefile *);

RB_HEAD(archivecache, archivefile);
RB_PROTOTYPE_STATIC(archivecache, archivefile, entries, archivefile_compare)


struct archive {
	uint32_t	 	 key;

	int		 	 archivefd;
	char			*path;
	int			 weak;

	char		 	 errstr[ERRSTRSIZE];

	struct archivecache	 cachedfiles;
	uint64_t		 numfiles;

	RB_ENTRY(archive)	 entries;
};

static int	 archive_compare(struct archive *, struct archive *);
static void	 archive_recorderror(struct archive *, const char *, ...);
static char	*archive_keytopath(uint32_t);

static uint32_t	 archive_takecrc32(struct archive *);
static void	 archive_writecrc32(struct archive *, uint32_t);

static ssize_t	 archive_seektostart(struct archive *);
static ssize_t	 archive_seekpastsignature(struct archive *);
static ssize_t	 archive_seektoend(struct archive *);

static ssize_t	 archive_readfileinfo(struct archive *, uint16_t *, char *, uint32_t *, uint32_t *);
static void	 archive_appendfileinfo(struct archive *, uint16_t, char *, uint32_t, uint32_t);

static uint64_t	 archive_cacheallfiles(struct archive *);


RB_HEAD(archivetree, archive);
RB_PROTOTYPE_STATIC(archivetree, archive, entries, archive_compare)

static struct archivetree	 activearchives = RB_INITIALIZER(&activearchives);



RB_GENERATE_STATIC(archivecache, archivefile, entries, archivefile_compare)
RB_GENERATE_STATIC(archivetree, archive, entries, archive_compare)

static struct archivefile *
archivefile_new(char *name, size_t offset)
{
	struct archivefile	*out;

	if (strlen(name) > MAXNAMESIZE) {
		errno = ENAMETOOLONG;
		log_fatal("archivefile_new: tried to make archive file with "
			"name %s, which is too long", name);

	} else if (strlen(name) == 0) {
		errno = EINVAL;
		log_fatal("archivefile_new: tried to make archive file with "
			"an empty name, which is disallowed");
	}

	out = calloc(1, sizeof(struct archivefile));
	if (out == NULL)
		log_fatal("archivefile_new: malloc archive file structure failed");

	out->name = strdup(name);
	if (out->name == NULL)
		log_fatal("archivefile_new: strdup archive filename failed");

	out->offset = offset;
	return out;
}

static void
archivefile_teardown(struct archivefile *a)
{
	free(a->name);
	free(a);
}

static int
archivefile_compare(struct archivefile *a, struct archivefile *b)
{
	return strncmp(a->name, b->name, MAXNAMESIZE);
}

static int
archive_compare(struct archive *a, struct archive *b)
{
	int	result = 0;

	if (a->key > b->key) result = 1;
	if (a->key < b->key) result = -1;

	return result;
}

static void
archive_recorderror(struct archive *a, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(a->errstr, ERRSTRSIZE, fmt, ap);
	va_end(ap);
}

static char *
archive_keytopath(uint32_t key)
{
	char	*out;
	size_t	 offset;

	/* XXX: no chroot is a special case */
	offset = strlen(CHROOT);
	if (offset == 1) offset = 0;

	if (asprintf(&out, "%s/%u.bundle", ARCHIVES + offset, key) < 0)
		log_fatal("archive_keytopath: asprintf path failed");

	return out;
}

static uint32_t
archive_takecrc32(struct archive *a)
{
	char		*readbuffer;
	uint32_t	 checksum = 0;
	ssize_t		 bytesread;

	readbuffer = reallocarray(NULL, BLOCKSIZE, sizeof(char));
	if (readbuffer == NULL)
		log_fatal("archive_takecrc32: malloc read buffer for crc32 "
			"computation failed");

	checksum = crc32_z(checksum, NULL, 0);

	if (archive_seekpastsignature(a) < 0) {
		if (errno != EBADMSG)
			log_fatal("archive_takecrc32: couldn't seek past signature to begin "
				"crc32 computation");

	} else {
		while ((bytesread = read(a->archivefd, readbuffer, BLOCKSIZE)) > 0)
			checksum = crc32(checksum, (Bytef *)readbuffer, (uint32_t)bytesread);

		if (bytesread < 0)
			log_fatal("archive_takecrc32: read of data block for crc32 "
				"computation failed");
	}

	if (archive_seektoend(a) < 0)
		log_fatal("archive_takecrc32: couldn't reset archive seek pointer "
			"to end of archive");

	free(readbuffer);
	return checksum;
}

static void
archive_writecrc32(struct archive *a, uint32_t checksum)
{
	ssize_t		written;
	uint32_t	bechecksum;

	if (archive_seektostart(a) != 0)
		log_fatal("archive_writecrc32: couldn't seek to start of archive "
			"(crc32's starting point)");

	bechecksum = htonl(checksum);

	written = write(a->archivefd, &bechecksum, sizeof(uint32_t));
	if (written < 0)
		log_fatal("archive_writecrc32: writing crc32 failed");

	if (archive_seektoend(a) < 0)
		log_fatal("archive_writecrc32: couldn't reset archive seek pointer "
			"to end of archive");
}

static ssize_t
archive_seektostart(struct archive *a)
{
	return lseek(a->archivefd, 0, SEEK_SET);
}

static ssize_t
archive_seekpastsignature(struct archive *a)
{
	ssize_t	endoffset, offset = sizeof(uint32_t) + sizeof(uint16_t) + MAXSIGSIZE;
	ssize_t status = -1;

	endoffset = archive_seektoend(a);
	if (endoffset < 0) log_fatal("archive_seekpastsignature: couldn't seek to "
		"end of archive to check whether content exists past signature");

	if (endoffset < offset) {
		errno = EBADMSG;
		goto end;
	}

	status = lseek(a->archivefd, offset, SEEK_SET);
end:
	return status;
}

static ssize_t
archive_seektoend(struct archive *a)
{
	return lseek(a->archivefd, 0, SEEK_END);
}

static ssize_t
archive_readfileinfo(struct archive *a, uint16_t *labelsize, char *label,
	uint32_t *uncompressedsize, uint32_t *compressedsize)
{
	char		*bufp;
	char		 buf[sizeof(uint16_t) + MAXNAMESIZE + 2 * sizeof(uint32_t)];

	ssize_t		 initial_offset, hdrcount = -1;
	uint16_t	 labelsizecopy;

	initial_offset = lseek(a->archivefd, 0, SEEK_CUR);
	if (initial_offset < 0) 
		log_fatal("archive_readfileinfo: obtaining current seek pointer "
			"offset failed");

	hdrcount = read(a->archivefd, buf, sizeof(uint16_t));

	if (hdrcount < 0)
		log_fatal("archive_readfileinfo: reading file name size off archive failed");
	else if (hdrcount == 0)
		goto end;

	else if (hdrcount < (ssize_t)sizeof(uint16_t)) {
		hdrcount = -1;
		log_writex(LOGTYPE_DEBUG, "archive_readfileinfo: reading file name size off "
			"archive returned < sizeof(uint16_t) bytes. a malformed or incomplete "
			"archive was probably received");

		errno = EFTYPE;
		goto end;
	}

	bufp = buf;
	labelsizecopy = ntohs(*(uint16_t *)bufp);
	bufp += sizeof(uint16_t);

	if (labelsize != NULL) *labelsize = labelsizecopy;

	/* XXX: Need to integrity check this here, because we are
	 * called by archive_isvalid, and there's a memcpy below that
	 * would break if we got a bogus label size
	 */

	if (labelsizecopy > MAXNAMESIZE || labelsizecopy == 0) {
		hdrcount = -1;
		log_writex(LOGTYPE_DEBUG, "archive_readfileinfo: a filename of length "
			"%lu (> MAXNAMESIZE = %lu) was found in the archive. this looks fishy, "
			"erroring out", labelsizecopy, MAXNAMESIZE);

		errno = ENAMETOOLONG;
		goto end;
	}

	hdrcount = read(a->archivefd, bufp, labelsizecopy + 2 * sizeof(uint32_t));

	if (hdrcount < 0)
		log_fatal("archive_readfileinfo: reading label + uncompressed/compressed "
			"sizes off of archive failed unexpectedly");

	else if (hdrcount != labelsizecopy + 2 * sizeof(uint32_t)) {
		hdrcount = -1;
		log_writex(LOGTYPE_DEBUG, "archive_readfileinfo: the remainder of the file header "
			"after file name size (this includes the filename, and compressed/ "
			"uncompressed sizes) seems to be incomplete. stopping.");

		errno = EFTYPE;
		goto end;
	}

	hdrcount += sizeof(uint16_t);

	if (label != NULL) {
		memcpy(label, bufp, labelsizecopy);
		label[labelsizecopy] = '\0';
	}

	bufp += labelsizecopy;

	if (uncompressedsize != NULL)
		*uncompressedsize = ntohl(*(uint32_t *)bufp);
	bufp += sizeof(uint32_t);

	if (compressedsize != NULL)
		*compressedsize = ntohl(*(uint32_t *)bufp);

end:
	if (hdrcount < 0)
		if (lseek(a->archivefd, initial_offset, SEEK_SET) != initial_offset)
			log_fatal("archive_readfileinfo: during error recovery, attempt to "
				"reset the file offset to its value before reading metadata "
				"failed");

	return hdrcount;
}

static void
archive_appendfileinfo(struct archive *a, uint16_t labelsize, char *label,
	uint32_t uncompressedsize, uint32_t compressedsize)
{
	uint32_t	beuncompressedsize, becompressedsize;
	uint16_t	belabelsize;
	int		status = - 1;

	belabelsize = htons(labelsize);
	beuncompressedsize = htonl(uncompressedsize);
	becompressedsize = htonl(compressedsize);

	if (write(a->archivefd, &belabelsize, sizeof(uint16_t)) != sizeof(uint16_t))
		goto end;

	if (write(a->archivefd, label, labelsize) != (ssize_t)labelsize)
		goto end;
	
	if (write(a->archivefd, &beuncompressedsize, sizeof(uint32_t)) != sizeof(uint32_t))
		goto end;

	if (write(a->archivefd, &becompressedsize, sizeof(uint32_t)) != sizeof(uint32_t))
		goto end;

	status = 0;
end:
	if (status < 0)
		log_fatal("archive_appendfileinfo: write of file metadata to archive failed");
}

static uint64_t
archive_cacheallfiles(struct archive *a)
{
	char	 		 newname[MAXNAMESIZE + 1];
	uint64_t		 tally = 0;
	uint32_t		 newsize;
	ssize_t	 		 amtread, newoffset = 0;

	/* XXX: shouldn't happen, call archive_isvalid somewhere safe before
	 * trying to set up data based off of it. archive_isvalid doesn't check
	 * the cache, so this should mostly pass if everything else is set up.
	 */
	if (!archive_isvalid(a))
		log_fatal("archive_cacheallfiles: while trying to inspect archive to build "
			"a list of its constituent files, found the archive itself isn't "
			"valid. the reason for this seems to be: '%s'", a->errstr);

	/* XXX: just making sure */
	if (!RB_EMPTY(&a->cachedfiles))
		log_fatalx("archive_cacheallfiles: tried to build out the archive file "
			"cache when one was already build. this is disallowed, halting");

	newoffset = archive_seekpastsignature(a);
	if (newoffset < 0)
		log_fatal("archive_cacheallfiles: seeking past archive signature to start "
			"of file list failed");

	while ((amtread = archive_readfileinfo(a, NULL, newname, NULL, &newsize)) > 0) {
		struct archivefile	*newentry;

		newentry = archivefile_new(newname, (size_t)newoffset);
		RB_INSERT(archivecache, &a->cachedfiles, newentry);

		if (lseek(a->archivefd, (off_t)newsize, SEEK_CUR) < 0)
			log_fatal("archive_cacheallfiles: skipping over the body of %s "
				"while trying to cache it failed", newname);

		newoffset += amtread;
		newoffset += newsize;

		tally++;
	}

	if (amtread < 0) log_fatal("archive_cacheallfiles: reading file metadata failed at "
		"some point during the caching process. if debug logs are enabled, inspect "
		"any log lines above this one for further clues. archive_readfileinfo returned");

	return tally;
}

struct archive *
archive_new(uint32_t key)
{
	struct archive	*a, *out = NULL;
	char		*newpath = NULL;
	uint32_t	 initialcrc;

	int		 newfd = -1;
	int		 flags = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC;
	mode_t		 mode = S_IRUSR | S_IWUSR | S_IRGRP;

	if (archive_fromkey(key) != NULL)
		log_fatalx("archive_new: tried to create two archives w/ the same key, "
			"which honestly defeats the purpose of the whole key abstraction "
			"thing. i give up");

	newpath = archive_keytopath(key);
	log_writex(LOGTYPE_DEBUG, "opening %s", newpath);
	newfd = open(newpath, flags, mode);
	if (newfd < 0)
		log_fatal("archive_new: open");

	a = calloc(1, sizeof(struct archive));
	if (a == NULL)
		log_fatal("archive_new: calloc");

	a->key = key;
	a->archivefd = newfd;
	a->weak = 0;
	a->path = newpath;

	RB_INIT(&a->cachedfiles);

	initialcrc = archive_takecrc32(a);
	archive_writecrc32(a, initialcrc);
	archive_writesignature(a, "");

	/* XXX: these steps do nothing but good to have and debug i guess */
	if (!archive_isvalid(a))
		log_fatalx("archive_new: a newly created archive is not valid. "
		"if i had to guess, you have a bug somewhere, and you have some serious "
		"fixing up to do. the reason for invalidity is '%s'", a->errstr);

	archive_cacheallfiles(a);

	out = a;
	RB_INSERT(archivetree, &activearchives, out);

	return out;
}

struct archive *
archive_fromfile(uint32_t key, char *path)
{
	struct archive	*a, *out = NULL;
	char		*loadpath = NULL;
	int		 loadfd = -1;

	if (archive_fromkey(key) != NULL)
		log_fatalx("archive_fromfile: tried to create two archives w/ the same key, "
			"which honestly defeats the purpose of the whole key abstraction "
			"thing. i give up");

	loadpath = strdup(path);
	if (loadpath == NULL)
		log_fatal("archive_fromfile: strdup (path to be loaded from) failed");

	loadfd = open(path, O_RDONLY);
	if (loadfd < 0) goto end;
	else if (lseek(loadfd, 0, SEEK_END) < 0)
		log_fatal("archive_fromfile: initial seek to end of freshly loaded "
			"archive (to preserve the seek-pointer-always-at-the-end invariant "
			"on these things) failed");

	a = calloc(1, sizeof(struct archive));
	if (a == NULL)
		log_fatal("archive_fromfile: malloc in-memory archive structure failed");

	a->key = key;
	a->archivefd = loadfd;
	a->weak = 1;
	a->path = loadpath;

	RB_INIT(&a->cachedfiles);

	if (!archive_isvalid(a)) {
		log_writex(LOGTYPE_DEBUG, "archive loaded from path %s is not valid! the "
			"reason for this is '%s'. cannot continue, returning EFTYPE", a->errstr);
		errno = EFTYPE;
		goto end;
	}

	a->numfiles = archive_cacheallfiles(a);

	out = a;
	RB_INSERT(archivetree, &activearchives, out);
end:
	if (out == NULL && a != NULL)
		free(a);

	if (loadfd > 0 && out == NULL)	
		close(loadfd);

	if (loadpath != NULL && out == NULL)
		free(loadpath);

	return out;
}

struct archive *
archive_fromkey(uint32_t key)
{
	struct archive	dummy, *out;

	dummy.key = key;
	out = RB_FIND(archivetree, &activearchives, &dummy); 

	if (out == NULL) errno = EINVAL;
	return out;
}

void
archive_teardown(struct archive *a)
{
	struct archivefile	*thisfile;

	while (!RB_EMPTY(&a->cachedfiles)) {
		thisfile = RB_ROOT(&a->cachedfiles);
		RB_REMOVE(archivecache, &a->cachedfiles, thisfile);

		archivefile_teardown(thisfile);
	}

	RB_REMOVE(archivetree, &activearchives, a);

	close(a->archivefd);
	if (!a->weak)
		if (unlink(a->path) < 0)
			log_fatal("archive_teardown: unlink underlying "
				" archive file storage failed");

	free(a->path);
	free(a);
}

void
archive_teardownall(void)
{
	struct archive *a;

	while (!RB_EMPTY(&activearchives)) {
		a = RB_ROOT(&activearchives);
		archive_teardown(a);
	}
}

int
archive_addfile(struct archive *a, char *fname, char *data, size_t datasize)
{
	struct archivefile	*newcacheentry;
	size_t			 newcacheoffset;
	uint32_t		 newcrc;

	char			*compressedbuf;
	size_t		 	 compressedsize, compressedsizebound;
	int		 	 zstatus, status = -1;

	if (datasize > MAXFILESIZE) {
		archive_recorderror(a, "adding a file to your archive failed, because "
			"it is too large. your file is %lu bytes, but the maximum allowed "
			"is %lu bytes", datasize, MAXFILESIZE);
		goto end;

	} else if (strlen(fname) > MAXNAMESIZE) {
		archive_recorderror(a, "adding a file to your archive failed, because "
			"its name is too large. the upper limit on name length is %lu, and you "
			"hit %lu bytes.", MAXNAMESIZE, strlen(fname));
		goto end;

	} else if (strlen(fname) == 0) {
		archive_recorderror(a, "you seem to have tried to add a file with no name? to "
			"the archive? i'm not really sure how you did this, unless you crafted "
			"a custom message to see what would happen. you dirty hacker.");
		goto end;
	}


	if (archive_hasfile(a, fname)) {
		archive_recorderror(a, "you seem to have tried to add the same file twice to "
			"the archive. some symbolic link stuff is happening? or i have a bug");
		goto end;
	}

	if (a->numfiles == ARCHIVE_MAXFILES) {
		archive_recorderror(a, "whoa there buddy, you already have %lu files in this "
			"archive, which equals the allowed maximum. trying to add one more didn't "
			"work, but what you've added so far has been preserved", ARCHIVE_MAXFILES);
		goto end;
	}

	newcacheoffset = lseek(a->archivefd, 0, SEEK_CUR);
	if (newcacheoffset < 0)
		log_fatal("archive_addfile: getting the new file's seek pointer offset to throw "
			"into the archive cache didn't work");

	compressedsizebound = compressBound(datasize);

	compressedbuf = reallocarray(NULL, compressedsizebound, sizeof(char));
	if (compressedbuf == NULL)
		log_fatal("archive_addfile: malloc in-memory buffer "
			"for compressed file failed out");

	compressedsize = compressedsizebound;
	zstatus = compress((Bytef *)compressedbuf, &compressedsize, (Bytef *)data, datasize);

	if (zstatus != Z_OK)
		log_fatalx("archive_addfile: compressing file content failed with zlib "
			"status = %d", zstatus);

	archive_appendfileinfo(a, strlen(fname), fname, datasize, compressedsize);

	if (write(a->archivefd, compressedbuf, compressedsize) < 0)
		log_fatal("archive_addfile: write of compressed file data to backing "
			"storage failed");

	newcacheentry = archivefile_new(fname, newcacheoffset);
	RB_INSERT(archivecache, &a->cachedfiles, newcacheentry);

	newcrc = archive_takecrc32(a);
	archive_writecrc32(a, newcrc);

	free(compressedbuf);
	a->numfiles++;
	status = 0;
end:
	return status;
}

int
archive_hasfile(struct archive *a, char *fname)
{
	struct archivefile	*found, dummy;
	int			 present = 0;

	if (strlen(fname) > MAXNAMESIZE) {
		archive_recorderror(a, "you looked for a file with name length %lu, which "
			"exceeds the allowed maximum %lu. i can assure you such a file "
			"does not exist in the archive", strlen(fname), MAXNAMESIZE);
		goto end;

	} else if (strlen(fname) == 0) {
		archive_recorderror(a, "you looked for a file with no name in the archive, "
			"which makes positively little sense. either you're a dirty hacker "
			"(stop it and do your psets) or i have a bug...");
		goto end;
	}

	dummy.name = fname;
	found = RB_FIND(archivecache, &a->cachedfiles, &dummy);
	
	if (found != NULL) present = 1;
end:
	return present;
}

char *
archive_loadfile(struct archive *a, char *fname, size_t *datasizeout)
{
	struct archivefile	*found, dummy;
	char			*compressedbuf, *bufout = NULL;
	uint32_t		 compressedsize, rawdatasize;
	int			 zstatus;
	
	if (strlen(fname) > MAXNAMESIZE) {
		archive_recorderror(a, "you tried to load a file with name length %lu, "
			"which exceeds the allowed maximum %lu. no such file exists in "
			"the archive as such", strlen(fname), MAXNAMESIZE);
		goto end;

	} else if (strlen(fname) == 0) {
		archive_recorderror(a, "why are you trying to load a file with no name?");
		goto end;
	}

	dummy.name = fname;
	found = RB_FIND(archivecache, &a->cachedfiles, &dummy);

	if (found == NULL) {
		archive_recorderror(a, "the file you're looking for is not "
			"present in this archive");
		goto end;
	}

	if (lseek(a->archivefd, found->offset, SEEK_SET) < 0)
		log_fatal("archive_loadfile: can't seek underlying archive storage "
			"to where the target file should be");

	if (archive_readfileinfo(a, NULL, NULL, &rawdatasize, &compressedsize) < 0)
		log_fatal("archive_loadfile: not able to pull file metadata off of "
			"archive, perhaps due to data corruption or a malformed "
			"archive that made it past validation. this is likely a bug");

	*datasizeout = (ssize_t)rawdatasize;

	compressedbuf = reallocarray(NULL, compressedsize, sizeof(char));
	if (compressedbuf == NULL)
		log_fatal("archive_loadfile: malloc in-memory copy of compressed "
			"file data failed");

	bufout = reallocarray(NULL, *datasizeout, sizeof(char));
	if (bufout == NULL)
		log_fatal("archive_loadfile: malloc buffer for uncompressed "
			"file data failed");

	if (read(a->archivefd, compressedbuf, compressedsize) < 0)
		log_fatal("archive_loadfile: couldn't read compressed data out of "
			"backing storage");

	zstatus = uncompress((Bytef *)bufout,
			     datasizeout,
			     (Bytef *)compressedbuf,
			     compressedsize);

	if (zstatus != Z_OK)
		log_fatalx("archive_loadfile: uncompressing file data failed "
			"unexpectedly inside zlib, which returned zstatus %d", zstatus);

	free(compressedbuf);
end:
	if (lseek(a->archivefd, 0, SEEK_END) < 0)
		log_fatal("archive_loadfile: couldn't reset seek pointer to the "
			"end of the archive after reading file data");

	return bufout;
}

uint32_t
archive_getcrc32(struct archive *a)
{
	uint32_t	crcout;

	if (lseek(a->archivefd, 0, SEEK_SET) < 0)
		log_fatal("archive_getcrc32: couldn't move seek pointer of a good "
			"archive to find its crc");

	if (read(a->archivefd, &crcout, sizeof(uint32_t)) < 0)
		log_fatal("archive_getcrc32: couldn't read crc32 off backing storage");

	crcout = ntohl(crcout);

	if (lseek(a->archivefd, 0, SEEK_END) < 0)
		log_fatal("archive_getcrc32: couldn't reset seek pointer to end of "
			"the archive after reading its CRC");

	return crcout;
}

char *
archive_getsignature(struct archive *a)
{
	uint16_t	 signaturelen;
	char		*signature;

	if (lseek(a->archivefd, sizeof(uint32_t), SEEK_SET) < 0)
		log_fatal("archive_getsignature: couldn't move seek pointer of a "
			"good archive to find its signature");

	if (read(a->archivefd, &signaturelen, sizeof(uint16_t)) < 0)
		log_fatal("archive_getsignature: couldn't read off the length of "
			"this archive's signature from backing storage");

	signaturelen = ntohs(signaturelen);

	signature = reallocarray(NULL, signaturelen + 1, sizeof(char));
	if (signature == NULL)
		log_fatal("archive_getsignature: couldn't allocate in-memory buffer "
			"for a copy of this archive's signature");

	if (read(a->archivefd, signature, signaturelen) < 0)
		log_fatal("archive_getsignature: couldn't read signature off of "
			"backing storage");

	signature[signaturelen] = '\0';

	if (lseek(a->archivefd, 0, SEEK_END) < 0)
		log_fatal("archive_getsignature: couldn't reset seek pointer to "
		"end of archive after signature read");

	return signature;
}

void
archive_writesignature(struct archive *a, char *signature)
{
	size_t		signaturelen;
	ssize_t		written;
	uint16_t	shortsignaturelen, besignaturelen;

	char		writebuffer[sizeof(uint16_t) + MAXSIGSIZE];

	signaturelen = strlen(signature);
	if (signaturelen > MAXSIGSIZE)
		log_fatalx("archive_writesignature: you tried to write "
			"a signature which exceeds the maximum allowed "
			"signature size, %lu. cannot continue.", MAXSIGSIZE);

	shortsignaturelen = (uint16_t)signaturelen;
	besignaturelen = htons(shortsignaturelen);

	bzero(writebuffer, sizeof(uint16_t) + MAXSIGSIZE);

	memcpy(writebuffer, &besignaturelen, sizeof(uint16_t));
	memcpy(writebuffer + sizeof(uint16_t), signature, signaturelen);

	if (lseek(a->archivefd, sizeof(uint32_t), SEEK_SET) < 0)
		log_fatal("archive_writesignature: seek to the start of "
			"the signature length + signature length fields "
			"for overwrite failed out");

	written = write(a->archivefd, writebuffer, sizeof(uint16_t) + MAXSIGSIZE);
	if (written < 0)
		log_fatal("archive_writesignature: write");

	if (archive_seektoend(a) < 0)
		log_fatal("archive_writesignature: archive_seektoend");
}

char *
archive_error(struct archive *a)
{
	char	*out;

	out = strdup(a->errstr);
	if (out == NULL) log_fatal("archive_error: strdup");

	return out;
}

int
archive_isvalid(struct archive *a)
{
	/* things to do:
	 * -> check archive length is sane (>= signature + crc)
	 * -> check the crc is correct
	 * -> for each file,
	 *	-> ensure header can be read at all; i.e. that it is long enough
	 *	-> ensure file name size is sane
	 *	-> ensure file uncompressed size is sane
	 *	-> ensure file compressed size <= compressBound(uncompressed)
	 *	-> ensure file is long enough i.e. no eof before compressed size
	 */

	ssize_t		amtread, archivelength, currentoffset;
	uint32_t	claimedcrc, actualcrc;

	char		 label[MAXNAMESIZE + 1];
	uint32_t	 compressedsize, uncompressedsize;
	uint16_t	 labelsize;
	int		 status = 0;
	
	archivelength = lseek(a->archivefd, 0, SEEK_END);
	if (archivelength < 0)
		log_fatal("archive_isvalid: lseek to ascertain length");

	if (archivelength < (ssize_t)(sizeof(uint32_t) + sizeof(uint16_t) + MAXSIGSIZE)) {
		archive_recorderror(a, "archive is too short (length %ld)", archivelength);
		goto end;
	}

	claimedcrc = archive_getcrc32(a);
	actualcrc = archive_takecrc32(a);

	if (claimedcrc != actualcrc) {
		archive_recorderror(a, "incorrect crc recorded for archive");
		goto end;
	}

	if ((currentoffset = archive_seekpastsignature(a)) < 0)
		log_fatal("archive_isvalid: lseek to past signature");

	for (;;) {
		amtread = archive_readfileinfo( a,
					       &labelsize,
					        label,
					       &uncompressedsize,
					       &compressedsize);
		
		if (amtread < 0) {
			archive_recorderror(a, "reading file info failed: %s", strerror(errno));
			goto end;
		} else if (amtread == 0) break;

		/* XXX: this is already checked for, but let's pretend we don't know that */
		if (labelsize > MAXNAMESIZE) {
			archive_recorderror(a, "file label (length %u) too long", labelsize);
			goto end;
		} else if (labelsize == 0) {
			archive_recorderror(a, "file label has zero length");
			goto end;
		}

		if (uncompressedsize > MAXFILESIZE) {
			archive_recorderror(a, "file (length %u) too long", label, uncompressedsize);
			goto end;
		}

		if (compressedsize > compressBound(uncompressedsize)) {
			archive_recorderror(a, "file compressed size is impossibly long");
			goto end;
		}

		currentoffset += amtread + compressedsize;

		if (currentoffset > archivelength) {
			archive_recorderror(a, "file extends past end of archive file");
			goto end;
		}

		if (lseek(a->archivefd, currentoffset, SEEK_SET) < 0)
			log_fatal("archive_isvalid: lseek to next file");
	}
		
	status = 1;
end:
	return status;
}

char *
archive_getpath(struct archive *a)
{
	char	*pathout;

	pathout = strdup(a->path);
	if (pathout == NULL) log_fatal("archive_getpath: strdup");

	return pathout;
}
