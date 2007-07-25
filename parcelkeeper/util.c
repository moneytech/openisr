/*
 * Parcelkeeper - support daemon for the OpenISR (TM) system virtual disk
 *
 * Copyright (C) 2006-2007 Carnegie Mellon University
 *
 * This software is distributed under the terms of the Eclipse Public License,
 * Version 1.0 which can be found in the file named LICENSE.Eclipse.  ANY USE,
 * REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES RECIPIENT'S
 * ACCEPTANCE OF THIS AGREEMENT
 */

#define LOCKFILE_NAME "vulpes.lock"
#define PIDFILE_NAME "vulpes.pid"

vulpes_err_t read_file(const char *path, char *buf, int *bufsize)
{
	int fd;
	int count;
	vulpes_err_t ret=VULPES_SUCCESS;

	fd=open(path, O_RDONLY);
	if (fd == -1) {
		switch (errno) {
		case ENOTDIR:
		case ENOENT:
			return VULPES_NOTFOUND;
		case ENOMEM:
			return VULPES_NOMEM;
		default:
			return VULPES_IOERR;
		}
	}
	count=read(fd, buf, *bufsize);
	if (count == -1)
		ret=VULPES_IOERR;
	else if (count == *bufsize && !at_eof(fd))
		ret=VULPES_OVERFLOW;
	else
		*bufsize=count;
	close(fd);
	return ret;
}

/* Read a file consisting of a newline-terminated string, and return the string
   without the newline */
vulpes_err_t read_sysfs_file(const char *path, char *buf, int bufsize)
{
	vulpes_err_t ret=read_file(path, buf, &bufsize);
	if (ret)
		return ret;
	while (--bufsize >= 0 && buf[bufsize] != '\n');
	if (bufsize < 0)
		return VULPES_BADFORMAT;
	buf[bufsize]=0;
	return VULPES_SUCCESS;
}

char *vulpes_strerror(vulpes_err_t err)
{
	switch (err) {
	case VULPES_SUCCESS:
		return "Success";
	case VULPES_OVERFLOW:
		return "Buffer too small for data";
	case VULPES_IOERR:
		return "I/O error";
	case VULPES_NOTFOUND:
		return "Object not found";
	case VULPES_INVALID:
		return "Invalid parameter";
	case VULPES_NOMEM:
		return "Out of memory";
	case VULPES_NOKEY:
		return "No such key in keyring";
	case VULPES_TAGFAIL:
		return "Tag did not match data";
	case VULPES_BADFORMAT:
		return "Invalid format";
	case VULPES_CALLFAIL:
		return "Call failed";
	case VULPES_PROTOFAIL:
		return "Driver protocol error";
	case VULPES_NETFAIL:
		return "Network failure";
	case VULPES_BUSY:
		return "Object busy";
	}
	return "(Unknown)";
}

int set_signal_handler(int sig, void (*handler)(int sig))
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler=handler;
	sa.sa_flags=SA_RESTART;
	return sigaction(sig, &sa, NULL);
}

void print_progress(unsigned chunks, unsigned maxchunks)
{
	unsigned percent;
	unsigned chunks_per_mb=(1 << 20)/state.chunksize_bytes;

	if (maxchunks)
		percent=chunks*100/maxchunks;
	else
		percent=0;
	printf("  %u%% (%u/%u MB)\n", percent, chunks/chunks_per_mb,
				maxchunks/chunks_per_mb);
	/* Move cursor to previous line */
	printf("\x1b[A");
}

vulpes_err_t form_lockdir_file_name(char *buf, int len,
	const char *suffix)
{
	int ret=snprintf(buf, len, "%s/%s", config.lockdir_name, suffix);
	if (ret == -1 || ret >= len)
		return VULPES_OVERFLOW;
	return VULPES_SUCCESS;
}

/* Create lock file.  flock locks don't work over NFS; byterange locks don't
   work over AFS; and dotlocks are difficult to check for freshness.  So
   we use a whole-file fcntl lock.  The lock shouldn't become stale because the
   kernel checks that for us; however, over NFS file systems without a lock
   manager, locking will fail.  For safety, we treat that as an error. */
vulpes_err_t acquire_lock(void)
{
	char name[MAX_PATH_LENGTH];
	int fd;
	struct flock lock = {
		.l_type   = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start  = 0,
		.l_len    = 0
	};

	if (form_lockdir_file_name(name, sizeof(name), LOCKFILE_NAME))
		return VULPES_OVERFLOW;
	fd=open(name, O_CREAT|O_WRONLY, 0666);
	if (fd == -1) {
		vulpes_log(LOG_ERRORS, "Couldn't open lock file %s", name);
		return VULPES_IOERR;
	}
	if (fcntl(fd, F_SETLK, &lock)) {
		close(fd);
		if (errno == EACCES || errno == EAGAIN)
			return VULPES_BUSY;
		else
			return VULPES_CALLFAIL;
	}
	state.lock_fd=fd;
	return VULPES_SUCCESS;
}

void release_lock(void)
{
	char name[MAX_PATH_LENGTH];

	if (form_lockdir_file_name(name, sizeof(name), LOCKFILE_NAME))
		return;
	unlink(name);
	close(state.lock_fd);
}

vulpes_err_t create_pidfile(void)
{
	char name[MAX_PATH_LENGTH];
	FILE *fp;

	if (form_lockdir_file_name(name, sizeof(name), PIDFILE_NAME))
		return VULPES_OVERFLOW;
	fp=fopen(name, "w");
	if (fp == NULL) {
		vulpes_log(LOG_ERRORS, "Couldn't open pid file %s", name);
		return VULPES_IOERR;
	}
	fprintf(fp, "%d\n", getpid());
	fclose(fp);
	return VULPES_SUCCESS;
}

void remove_pidfile(void)
{
	char name[MAX_PATH_LENGTH];

	if (form_lockdir_file_name(name, sizeof(name), PIDFILE_NAME))
		return;
	unlink(name);
}

/* Fork, and have the parent wait for the child to indicate that the parent
   should exit.  In the parent, this returns only on error.  In the child, it
   returns success and sets *status_fd.  If the child writes a byte to the fd,
   the parent will exit with that byte as its exit status.  If the child closes
   the fd without writing anything, the parent will exit(0). */
vulpes_err_t fork_and_wait(int *status_fd)
{
	int fds[2];
	pid_t pid;
	char ret=1;

	/* Make sure the child isn't killed if the parent dies */
	if (set_signal_handler(SIGPIPE, SIG_IGN)) {
		vulpes_log(LOG_ERRORS, "Couldn't block SIGPIPE");
		return VULPES_CALLFAIL;
	}
	if (pipe(fds)) {
		vulpes_log(LOG_ERRORS, "Can't create pipe");
		return VULPES_CALLFAIL;
	}

	pid=fork();
	if (pid == -1) {
		vulpes_log(LOG_ERRORS, "fork() failed");
		return VULPES_CALLFAIL;
	} else if (pid) {
		/* Parent */
		close(fds[1]);
		if (read(fds[0], &ret, sizeof(ret)) == 0)
			exit(0);
		else
			exit(ret);
	} else {
		/* Child */
		close(fds[0]);
		*status_fd=fds[1];
	}
	return VULPES_SUCCESS;
}
