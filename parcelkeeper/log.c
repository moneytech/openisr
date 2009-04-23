/*
 * Parcelkeeper - support daemon for the OpenISR (R) system virtual disk
 *
 * Copyright (C) 2006-2009 Carnegie Mellon University
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.  A copy of the GNU General Public License
 * should have been distributed along with this program in the file
 * LICENSE.GPL.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include "defs.h"

static void curtime(char *buf, unsigned buflen)
{
	struct timeval tv;
	struct tm tm;
	char fmt[22];

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	snprintf(fmt, sizeof(fmt), "%%b %%d %%Y %%H:%%M:%%S.%.3u",
				(unsigned)(tv.tv_usec / 1000));
	buf[0]=0;
	strftime(buf, buflen, fmt, &tm);
}

static pk_err_t parse_logtype(const char *name, enum pk_log_type *out)
{
	if (!strcmp(name, "info"))
		*out=LOG_INFO;
	else if (!strcmp(name, "chunk"))
		*out=LOG_CHUNK;
	else if (!strcmp(name, "transport"))
		*out=LOG_TRANSPORT;
	else if (!strcmp(name, "query"))
		*out=LOG_QUERY;
	else if (!strcmp(name, "slow"))
		*out=LOG_SLOW_QUERY;
	else if (!strcmp(name, "error"))
		*out=LOG_WARNING;  /* ERROR is just WARNING plus _LOG_FUNC */
	else if (!strcmp(name, "stats"))
		*out=LOG_STATS;
	else
		return PK_INVALID;
	return PK_SUCCESS;
}

static const char *log_prefix(enum pk_log_type type)
{
	switch (type & ~_LOG_FUNC) {
	case LOG_INFO:
		return "INFO";
	case LOG_CHUNK:
		return "CHUNK";
	case LOG_TRANSPORT:
		return "TRANSPORT";
	case LOG_QUERY:
		return "QUERY";
	case LOG_SLOW_QUERY:
		return "SLOW";
	case LOG_WARNING:
		return "ERROR";
	case LOG_STATS:
		return "STATS";
	}
	return NULL;
}

/* Cannot call pk_log(), since the logger hasn't started yet */
pk_err_t logtypes_to_mask(const char *list, unsigned *out)
{
	gchar **types;
	enum pk_log_type type;
	int i;

	*out=0;
	if (strcmp(list, "none")) {
		types=g_strsplit(list, ",", 0);
		for (i=0; types[i] != NULL; i++) {
			if (parse_logtype(types[i], &type)) {
				g_strfreev(types);
				return PK_INVALID;
			}
			*out |= (1 << type);
		}
		g_strfreev(types);
	}
	return PK_SUCCESS;
}

static void open_log(void)
{
	state.log_fp=fopen(config.log_file, "a");
	if (state.log_fp == NULL)
		pk_log(LOG_ERROR, "Couldn't open log file %s",
					config.log_file);
}

static void close_log(void)
{
	fclose(state.log_fp);
	state.log_fp=NULL;
}

static void check_log(void)
{
	struct stat st;

	if (state.log_fp == NULL)
		return;
	if (fstat(fileno(state.log_fp), &st)) {
		close_log();
		pk_log(LOG_ERROR, "Couldn't stat log file %s",
					config.log_file);
		return;
	}
	if (st.st_nlink == 0) {
		close_log();
		open_log();
		pk_log(LOG_INFO, "Log file disappeared; reopening");
	}
}

void _pk_log(enum pk_log_type type, const char *fmt, const char *func, ...)
{
	va_list ap;
	char buf[50];

	if (state.log_fp != NULL && ((1 << type) & config.log_file_mask)) {
		curtime(buf, sizeof(buf));
		check_log();
		/* Ignore errors; it's better to write the log entry unlocked
		   than to drop it on the floor */
		get_file_lock(fileno(state.log_fp),
					FILE_LOCK_WRITE | FILE_LOCK_WAIT);
		fseek(state.log_fp, 0, SEEK_END);
		va_start(ap, func);
		fprintf(state.log_fp, "%s %d %s: ", buf, state.pk_pid,
					log_prefix(type));
		if (type & _LOG_FUNC)
			fprintf(state.log_fp, "%s(): ", func);
		vfprintf(state.log_fp, fmt, ap);
		fprintf(state.log_fp, "\n");
		va_end(ap);
		fflush(state.log_fp);
		put_file_lock(fileno(state.log_fp));
	}

	if ((1 << type) & config.log_stderr_mask) {
		va_start(ap, func);
		fprintf(stderr, "PK: ");
		if (type & _LOG_FUNC)
			fprintf(stderr, "%s(): ", func);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
		va_end(ap);
	}
}

void log_start(void)
{
	state.pk_pid=getpid();
	/* stderr is unbuffered by default */
	setlinebuf(stderr);
	if (config.log_file != NULL && config.log_file_mask)
		open_log();
	pk_log(LOG_INFO, "Parcelkeeper starting in %s mode", config.modename);
}

void log_shutdown(void)
{
	pk_log(LOG_INFO, "Parcelkeeper shutting down");
	if (state.log_fp != NULL)
		close_log();
}
