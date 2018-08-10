#include "test-tool.h"
#include "cache.h"

/*
 * Verify that O_APPEND causes first write() to append to file.
 */ 
void verify_basic_append(const char *pathname)
{
	const char *buf = "test\n";
	size_t len = strlen(buf);
	int fd;
	off_t seek_eof_1;
	off_t seek_eof_2;

	fd = open(pathname, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		die_errno("creating '%s'", pathname);
	write(fd, buf, len);
	seek_eof_1 = lseek(fd, 0, SEEK_CUR);
	if (seek_eof_1 != len)
		die("seek_eof_1[%d] expected[%d]", seek_eof_1, len);
	close(fd);

	fd = open(pathname, O_WRONLY | O_APPEND, 0644);
	if (fd == -1)
		die_errno("opening '%s'", pathname);
	write(fd, buf, len);
	seek_eof_2 = lseek(fd, 0, SEEK_CUR);
	if (seek_eof_2 != len * 2)
		die("seek_eof_2[%d] expected[%d]", seek_eof_2, len * 2);
	close(fd);

	unlink(pathname);
}

/*
 * Open 2 file descriptors onto a file and confirm that interleaved writes
 * always append to the file.  That is, confirm that write() seeks forward
 * on each call.
 */
void verify_basic_interleve(const char *pathname)
{
	const char *buf = "test\n";
	size_t len = strlen(buf);
	int fd_1;
	int fd_2;
	off_t seek_eof_1;
	off_t seek_eof_2;

	fd_1 = open(pathname, O_WRONLY | O_APPEND | O_CREAT | O_EXCL, 0644);
	if (fd_1 == -1)
		die_errno("creating '%s'", pathname);
	fd_2 = open(pathname, O_WRONLY | O_APPEND, 0644);
	if (fd_2 == -1)
		die_errno("opening '%s'", pathname);

	write(fd_1, buf, len);
	write(fd_2, buf, len);

	write(fd_1, buf, len);
	write(fd_2, buf, len);

	write(fd_1, buf, len);
	write(fd_2, buf, len);

	write(fd_1, buf, len);
	write(fd_2, buf, len);

	seek_eof_1 = lseek(fd_1, 0, SEEK_END);
	seek_eof_2 = lseek(fd_2, 0, SEEK_END);
	if (seek_eof_1 != seek_eof_2)
		die("seek_eof_1[%d] != seek_eof_2[%d]", seek_eof_1, seek_eof_2);
	if (seek_eof_1 != len * 4 * 2)
		die("seek_eof[%d] expected[%d]", seek_eof_1, len * 4 * 2);

	close(fd_1);
	close(fd_2);

	unlink(pathname);
}


int cmd__file_append(int argc, const char **argv)
{
	const char *argv0 = argv[-1];
	char *pathname = NULL;

	if (argc == 1)
		die("Usage: %s %s <pathname>", argv0, argv[0]);

	pathname = argv[1];
	verify_basic_append(pathname);
	verify_basic_interleve(pathname);
	

	return 0;
}
