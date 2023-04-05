#include <sys/types.h>

#include <err.h>
#include <stdlib.h>
#include <string.h>

#include "workerd.h"

#define NAME_FIRST		"f1.txt"
#define NAME_SECOND		"f2.txt"
#define CONTENT_FIRST		"i am the first file"
#define CONTENT_SECOND		"i am _not_ the first file!!"

int	debug = 1, verbose = 1;

int
main()
{
	int	 status = -1;
	char	*first = NULL, *firstbackup = NULL, *second = NULL;

	char	*name = NULL, *data = NULL;
	size_t	 datasize;

	first = wbfile_writeback(NAME_FIRST, CONTENT_FIRST, strlen(CONTENT_FIRST) + 1);
	second = wbfile_writeback(NAME_SECOND, CONTENT_SECOND, strlen(CONTENT_SECOND) + 1);

	if (strcmp(first, second) == 0) {
		warnx("filename was reused between two wbfiles");
		goto end;
	}

	wbfile_readout(first, &name, &data, &datasize);
	
	if (strcmp(name, NAME_FIRST) != 0) {
		warnx("first filename %s does not match read out filename %s", NAME_FIRST, name);
		goto end;
	} else if (strcmp(data, CONTENT_FIRST) != 0) {
		warnx("first data %s does not match read data %s", CONTENT_FIRST, data);
		goto end;
	}

	free(name);
	free(data);
	wbfile_readout(second, &name, &data, &datasize);

	if (strcmp(name, NAME_SECOND) != 0) {
		warnx("second filename %s does not match read out filename %s", NAME_SECOND, name);
		goto end;
	} else if (strcmp(data, CONTENT_SECOND) != 0) {
		warnx("second data %s does not match read data %s", CONTENT_SECOND, data);
		goto end;
	}

	free(name);
	free(data);
	name = NULL;
	data = NULL;

	if ((firstbackup = strdup(first)) == NULL) {
		warn("backing up first filename via strdup failed");
		goto end;
	}

	wbfile_teardown(first);
	first = wbfile_writeback(NAME_SECOND, CONTENT_SECOND, strlen(CONTENT_SECOND) + 1);

	if (strcmp(first, firstbackup) != 0) {
		warnx("filename was not reused after wbfile_teardown");
		goto end;
	}

	wbfile_readout(first, &name, &data, &datasize);
	
	if (strcmp(name, NAME_SECOND) != 0) {
		warnx("rewritten filename %s does not match read filename %s", NAME_SECOND, name);
		goto end;
	} else if (strcmp(data, CONTENT_SECOND) != 0) {
		warnx("rewritten data %s does not match read data %s", CONTENT_SECOND, data);
		goto end;
	}

	status = 0;
end:
	if (first != NULL) wbfile_teardown(first);
	if (firstbackup != NULL) free(firstbackup);
	if (second != NULL) wbfile_teardown(second);

	if (name != NULL) free(name);
	if (data != NULL) free(data);

	return status;
}
