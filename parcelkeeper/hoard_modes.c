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

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "defs.h"

/* Helper for hoard().  Begins a transaction and *leaves it open*, except
   in case of error. */
static pk_err_t build_hoard_table(struct pk_state *state, int *chunks_to_hoard)
{
	struct query *qry;
	gboolean retry;

again:
	/* Build a temp table listing the chunks to hoard, so that
	   while we're actually hoarding we don't have locks against
	   the hoard index.  Take care not to list a tag more than once,
	   to keep the progress meter accurate */
	if (!begin(state->db))
		return PK_IOERR;
	if (!query(NULL, state->db, "CREATE TEMP TABLE to_hoard "
				"(chunk INTEGER NOT NULL, "
				"tag BLOB UNIQUE NOT NULL)", NULL)) {
		sql_log_err(state->db, "Couldn't create temporary table");
		goto bad;
	}
	if (!query(NULL, state->db, "INSERT OR IGNORE INTO temp.to_hoard "
				"(chunk, tag) SELECT chunk, tag "
				"FROM prev.keys WHERE tag NOT IN "
				"(SELECT tag FROM hoard.chunks)", NULL)) {
		sql_log_err(state->db, "Couldn't build list of chunks "
					"to hoard");
		goto bad;
	}

	/* See how much we need to hoard */
	query(&qry, state->db, "SELECT count(*) FROM temp.to_hoard", NULL);
	if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't count unhoarded chunks");
		goto bad;
	}
	query_row(qry, "d", chunks_to_hoard);
	query_free(qry);
	return PK_SUCCESS;

bad:
	retry = query_busy(state->db);
	rollback(state->db);
	if (retry) {
		query_backoff(state->db);
		goto again;
	}
	return PK_IOERR;
}

/* Make sure the chunk hasn't already been put in the hoard cache (because
   someone else already downloaded it) before we download.  Use the hoard DB
   connection so that state->db doesn't acquire the hoard DB lock. */
static gboolean need_fetch(struct pk_state *state, void *tag, unsigned taglen)
{
	gboolean ret = TRUE;
	gboolean retry;

again:
	if (!begin(state->hoard))
		return TRUE;
	query(NULL, state->hoard, "SELECT tag FROM chunks WHERE tag == ?", "b",
				tag, taglen);
	if (query_has_row(state->hoard)) {
		ret = FALSE;
	} else if (!query_ok(state->hoard)) {
		sql_log_err(state->hoard, "Couldn't query hoard cache index");
		retry = query_busy(state->hoard);
		rollback(state->hoard);
		if (retry) {
			query_backoff(state->hoard);
			goto again;
		}
		return ret;
	}
	rollback(state->hoard);
	return ret;
}

int hoard(struct pk_state *state)
{
	struct query *qry;
	void *buf;
	unsigned chunklen;
	int chunk;
	void *tag;
	unsigned taglen;
	int num_hoarded=0;
	int to_hoard;
	int ret=1;
	gboolean retry;

	/* We need to do this first; otherwise, chunks we thought were hoarded
	   could disappear out from under us */
	if (hoard_sync_refs(state, FALSE)) {
		pk_log(LOG_ERROR, "Couldn't synchronize reference list");
		return 1;
	}

	/* This opens a transaction */
	if (build_hoard_table(state, &to_hoard))
		return 1;

	/* If WANT_CHECK, that's all we need to do */
	if (state->conf->flags & WANT_CHECK) {
		rollback(state->db);
		if (to_hoard == 0)
			return 0;
		else
			return 1;
	}

	if (!commit(state->db)) {
		rollback(state->db);
		return 1;
	}

	buf=g_malloc(state->parcel->chunksize);

again:
	if (!begin(state->db))
		goto out_early;
	for (query(&qry, state->db, "SELECT chunk, tag FROM temp.to_hoard",
				NULL); query_has_row(state->db);
				query_next(qry)) {
		query_row(qry, "db", &chunk, &tag, &taglen);
		if (taglen != state->parcel->hashlen) {
			pk_log(LOG_ERROR, "Invalid tag length for chunk %d",
						chunk);
			goto out;
		}

		if (need_fetch(state, tag, taglen)) {
			if (transport_fetch_chunk(state->conn, buf, chunk,
						tag, &chunklen))
				goto out;
			print_progress_chunks(++num_hoarded, to_hoard);
		} else {
			print_progress_chunks(num_hoarded, --to_hoard);
		}
	}
	if (!query_ok(state->db))
		sql_log_err(state->db, "Querying hoard index failed");
	else
		ret=0;
out:
	query_free(qry);
	retry = query_busy(state->db);
	rollback(state->db);
	if (retry) {
		query_backoff(state->db);
		goto again;
	}
out_early:
	g_free(buf);
	if (!begin(state->db)) {
		sql_log_err(state->db, "Couldn't drop temporary table (1)");
		return PK_SQLERR;
	}
	if (!query(NULL, state->db, "DROP TABLE temp.to_hoard", NULL))
		sql_log_err(state->db, "Couldn't drop temporary table (2)");
	if (!commit(state->db)) {
		sql_log_err(state->db, "Couldn't drop temporary table (3)");
		rollback(state->db);
	}
	return ret;
}

int examine_hoard(struct pk_state *state)
{
	struct query *qry;
	unsigned validchunks;
	unsigned maxchunks;
	unsigned valid_mb;
	unsigned max_mb;
	unsigned valid_pct;
	gboolean retry;

	if (hoard_sync_refs(state, FALSE)) {
		pk_log(LOG_ERROR, "Couldn't synchronize reference list");
		return 1;
	}

again:
	if (!begin(state->db))
		return 1;
	query(&qry, state->db, "SELECT count(*) FROM "
				"(SELECT 1 FROM prev.keys GROUP BY tag)",
				NULL);
	if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't query previous keyring");
		goto bad;
	}
	query_row(qry, "d", &maxchunks);
	query_free(qry);
	query(&qry, state->db, "SELECT count(*) FROM hoard.refs WHERE "
				"parcel == ?", "d", state->hoard_ident);
	if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't query hoard cache");
		goto bad;
	}
	query_row(qry, "d", &validchunks);
	query_free(qry);
	if (!commit(state->db))
		goto bad;

	max_mb=(((off64_t)maxchunks) * state->parcel->chunksize) >> 20;
	valid_mb=(((off64_t)validchunks) * state->parcel->chunksize) >> 20;
	valid_pct=(100 * validchunks) / maxchunks;
	printf("Hoard cache : %u%% populated (%u/%u MB)\n", valid_pct,
				valid_mb, max_mb);
	return 0;

bad:
	retry = query_busy(state->db);
	rollback(state->db);
	if (retry) {
		query_backoff(state->db);
		goto again;
	}
	return 1;
}

int list_hoard(struct pk_state *state)
{
	struct query *qry;
	int ret=1;
	int parcel;
	const char *uuid;
	const char *server;
	const char *user;
	const char *name;
	int p_total;
	int p_unique;
	int shared;
	int unreferenced;
	int unused;
	gboolean retry;

again:
	if (!begin(state->db))
		return 1;
	query(&qry, state->db, "SELECT count(tag) FROM hoard.chunks WHERE "
				"referenced == 1", NULL);
	if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't count referenced chunks");
		goto out;
	}
	query_row(qry, "d", &shared);
	query_free(qry);
	query(&qry, state->db, "SELECT count(tag) FROM hoard.chunks WHERE "
				"referenced == 0", NULL);
	if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't count unreferenced chunks");
		goto out;
	}
	query_row(qry, "d", &unreferenced);
	query_free(qry);
	query(&qry, state->db, "SELECT count(*) FROM hoard.chunks WHERE "
				"tag ISNULL", NULL);
	if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't count unused chunk slots");
		goto out;
	}
	query_row(qry, "d", &unused);
	query_free(qry);
	for (query(&qry, state->db, "SELECT hoard.parcels.parcel, uuid, "
				"server, user, name, total, uniq "
				"FROM hoard.parcels LEFT JOIN "
				"(SELECT parcel, count(*) AS total "
				"FROM refs GROUP BY parcel) AS sub1 "
				"ON parcels.parcel == sub1.parcel "
				"LEFT JOIN "
				"(SELECT parcel, count(parcel) AS uniq FROM "
				"(SELECT parcel FROM refs GROUP BY tag "
				"HAVING count(*) == 1) GROUP BY parcel) "
				"AS sub2 "
				"ON parcels.parcel == sub2.parcel", NULL);
				query_has_row(state->db); query_next(qry)) {
		query_row(qry, "dssssdd", &parcel, &uuid, &server, &user,
					&name, &p_total, &p_unique);
		printf("%s %s %s %s %d %d\n", uuid, server, user, name, p_total,
					p_unique);
		shared -= p_unique;
	}
	query_free(qry);
	if (query_ok(state->db)) {
		printf("shared %d\n", shared);
		printf("unreferenced %d\n", unreferenced);
		printf("unused %d\n", unused);
		ret=0;
	} else {
		sql_log_err(state->db, "Couldn't list parcels in hoard cache");
	}
out:
	retry = query_busy(state->db);
	rollback(state->db);
	if (retry) {
		query_backoff(state->db);
		goto again;
	}
	return ret;
}

int rmhoard(struct pk_state *state)
{
	struct query *qry;
	const char *server;
	const char *user;
	const char *name;
	gchar *desc;
	int parcel;
	int removed;
	gboolean retry;

again:
	if (!begin(state->db))
		return 1;
	query(&qry, state->db, "SELECT parcel, server, user, name FROM "
				"hoard.parcels WHERE uuid == ?", "S",
				state->conf->uuid);
	if (query_ok(state->db)) {
		pk_log(LOG_INFO, "rmhoard: %s: No such parcel",
					state->conf->uuid);
		rollback(state->db);
		return 0;
	} else if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't query parcel table");
		goto bad;
	}
	query_row(qry, "dsss", &parcel, &server, &user, &name);
	/* server, user, and name expire when we free the query */
	desc = g_strdup_printf("%s/%s/%s", server, user, name);
	query_free(qry);

	query(&qry, state->db, "SELECT count(*) FROM "
				"(SELECT parcel FROM hoard.refs GROUP BY tag "
				"HAVING parcel == ? AND count(*) == 1)", "d",
				parcel);
	if (!query_has_row(state->db)) {
		g_free(desc);
		sql_log_err(state->db, "Couldn't enumerate unique "
					"parcel chunks");
		goto bad;
	}
	query_row(qry, "d", &removed);
	query_free(qry);

	pk_log(LOG_INFO, "Removing parcel %s from hoard cache...", desc);
	g_free(desc);
	if (!query(NULL, state->db, "UPDATE hoard.chunks SET referenced = 0 "
				"WHERE tag IN "
				"(SELECT tag FROM hoard.refs GROUP BY tag "
				"HAVING parcel == ? AND count(*) == 1)",
				"d", parcel)) {
		sql_log_err(state->db, "Couldn't update referenced flags");
		goto bad;
	}
	if (!query(NULL, state->db, "DELETE FROM hoard.refs WHERE parcel == ?",
				"d", parcel)) {
		sql_log_err(state->db, "Couldn't remove parcel from "
					"hoard cache");
		goto bad;
	}

	/* We can't remove the parcel from the parcels table unless we know
	   that no other Parcelkeeper process is running against that parcel */

	if (!commit(state->db))
		goto bad;
	pk_log(LOG_INFO, "Deallocated %d chunks", removed);
	return 0;

bad:
	retry = query_busy(state->db);
	rollback(state->db);
	if (retry) {
		query_backoff(state->db);
		goto again;
	}
	return 1;
}

static pk_err_t check_hoard_data(struct pk_state *state)
{
	struct query *qry;
	char buf[131072];  /* XXX assumes 128 KB */
	const void *tag;
	char calctag[64];  /* XXX */
	unsigned taglen;
	unsigned offset;
	unsigned len;
	int crypto;
	off64_t examined_bytes;
	off64_t total_bytes;
	pk_err_t ret;
	int count;
	gboolean retry;

	pk_log(LOG_INFO, "Validating hoard cache data");
	printf("Validating hoard cache data...\n");

again:
	if (!begin(state->db))
		return PK_IOERR;
	query(&qry, state->db, "SELECT sum(length) FROM temp.to_check", NULL);
	if (!query_has_row(state->db)) {
		sql_log_err(state->db, "Couldn't find the amount of data "
					"to check");
		goto bad;
	}
	query_row(qry, "D", &total_bytes);
	query_free(qry);

	count=0;
	examined_bytes=0;
	for (query(&qry, state->db, "SELECT tag, offset, length, crypto "
				"FROM temp.to_check", NULL);
				query_has_row(state->db); query_next(qry)) {
		query_row(qry, "bddd", &tag, &taglen, &offset, &len, &crypto);
		examined_bytes += len;
		print_progress_mb(examined_bytes, total_bytes);
		/* We assume the taglen, crypto suite, and chunk length are
		   good, because check_hoard() already validated these */
		if (pread(state->hoard_fd, buf, len, ((off_t)offset) << 9)
					!= (off_t)len) {
			pk_log(LOG_ERROR, "Couldn't read chunk at offset %d",
						offset);
			hoard_invalidate_chunk(state, offset, tag, taglen);
			count++;
			continue;
		}

		if (digest(crypto, calctag, buf, len)) {
			pk_log(LOG_ERROR, "digest() failed");
			ret=PK_CALLFAIL;
			continue;
		}
		if (memcmp(tag, calctag, taglen)) {
			pk_log(LOG_WARNING, "Tag mismatch at offset %d",
						offset);
			log_tag_mismatch(tag, calctag, taglen);
			hoard_invalidate_chunk(state, offset, tag, taglen);
			count++;
		}
	}
	query_free(qry);
	if (!query_ok(state->db)) {
		sql_log_err(state->db, "Couldn't walk chunk list");
		goto bad;
	}
	if (!query(NULL, state->db, "DROP TABLE temp.to_check", NULL)) {
		sql_log_err(state->db, "Couldn't drop temporary table");
		goto bad;
	}
	if (!commit(state->db))
		goto bad;
	if (count)
		pk_log(LOG_WARNING, "Removed %d invalid chunks", count);
	return ret;

bad:
	retry = query_busy(state->db);
	rollback(state->db);
	if (retry) {
		query_backoff(state->db);
		goto again;
	}
	if (!begin(state->db)) {
		sql_log_err(state->db, "Couldn't drop temporary table (1)");
		return PK_IOERR;
	}
	if (!query(NULL, state->db, "DROP TABLE temp.to_check", NULL))
		sql_log_err(state->db, "Couldn't drop temporary table (2)");
	if (!commit(state->db)) {
		sql_log_err(state->db, "Couldn't drop temporary table (3)");
		rollback(state->db);
	}
	return PK_IOERR;
}

int check_hoard(struct pk_state *state)
{
	struct query *qry;
	struct query *qry2;
	const char *uuid;
	int offset;
	int next_offset;
	const void *tag;
	unsigned taglen;
	int crypto;
	int count;
	int curcount;
	int curtime;
	gboolean retry;

	pk_log(LOG_INFO, "Validating hoard cache");
	printf("Validating hoard cache...\n");
	if (!validate_db(state->db))
		return 1;

again:
	if (!begin(state->db))
		return 1;

	for (query(&qry, state->db, "SELECT uuid FROM hoard.parcels",
				NULL), count=0; query_has_row(state->db);
				query_next(qry)) {
		query_row(qry, "s", &uuid);
		if (canonicalize_uuid(uuid, NULL) == PK_INVALID) {
			if (!query(&qry2, state->db, "DELETE FROM hoard.parcels "
						"WHERE uuid == ?", "s",
						uuid)) {
				sql_log_err(state->db, "Couldn't remove "
							"invalid parcel record "
							"from hoard index");
				query_free(qry);
				goto bad;
			}
			query_row(qry2, "d", &curcount);
			query_free(qry2);
			count += curcount;
		}
	}
	query_free(qry);
	if (!query_ok(state->db)) {
		sql_log_err(state->db, "Couldn't query parcel list");
		goto bad;
	}
	if (count)
		pk_log(LOG_WARNING, "Removed %d invalid parcel records", count);

	next_offset=0;
	for (query(&qry, state->db, "SELECT offset FROM hoard.chunks "
				"ORDER BY offset", NULL);
				query_has_row(state->db); query_next(qry)) {
		query_row(qry, "d", &offset);
		if (offset != next_offset) {
			/* XXX how do we fix this? */
			pk_log(LOG_WARNING, "Expected offset %d, found %d",
						next_offset, offset);
			query_free(qry);
			goto bad;
		}
		/* XXX assumes 128 KB */
		next_offset += 256;
	}
	query_free(qry);
	if (!query_ok(state->db)) {
		sql_log_err(state->db, "Couldn't query chunk table");
		goto bad;
	}

	count=0;
	for (query(&qry, state->db, "SELECT offset, tag, crypto FROM "
				"hoard.chunks", NULL); query_has_row(state->db);
				query_next(qry)) {
		query_row(qry, "dbd", &offset, &tag, &taglen, &crypto);
		if ((tag == NULL && crypto != 0) || (tag != NULL &&
					(!crypto_is_valid(crypto) ||
					crypto_hashlen(crypto) != taglen))) {
			count++;
			if (!query(NULL, state->db, "UPDATE hoard.chunks "
						"SET tag = NULL, length = 0, "
						"crypto = 0, last_access = 0, "
						"referenced = 0 WHERE "
						"offset = ?", "d", offset)) {
				sql_log_err(state->db, "Couldn't deallocate "
							"offset %d", offset);
				query_free(qry);
				goto bad;
			}
		}
	}
	query_free(qry);
	if (!query_ok(state->db)) {
		sql_log_err(state->db, "Couldn't query chunk list");
		goto bad;
	}
	if (count)
		pk_log(LOG_WARNING, "Cleaned %d chunks with invalid "
					"crypto suite", count);

	/* XXX assumes 128 KB */
	if (cleanup_action(state->db, "UPDATE hoard.chunks SET tag = NULL, "
				"length = 0, crypto = 0, last_access = 0, "
				"referenced = 0 WHERE length < 0 OR "
				"length > 131072 OR "
				"(length == 0 AND tag NOTNULL)",
				LOG_WARNING,
				"chunks with invalid length"))
		goto bad;
	if (cleanup_action(state->db, "UPDATE hoard.chunks SET tag = NULL, "
				"length = 0, crypto = 0, last_access = 0, "
				"referenced = 0 WHERE referenced != 0 AND "
				"referenced != 1",
				LOG_WARNING,
				"chunks with invalid referenced flag"))
		goto bad;
	if (cleanup_action(state->db, "DELETE FROM hoard.refs WHERE parcel "
				"NOT IN (SELECT parcel FROM hoard.parcels)",
				LOG_WARNING,
				"refs with dangling parcel ID"))
		goto bad;
	if (cleanup_action(state->db, "DELETE FROM hoard.refs WHERE tag NOT IN "
				"(SELECT tag FROM hoard.chunks)",
				LOG_WARNING,
				"refs with dangling tag"))
		goto bad;
	if (cleanup_action(state->db, "UPDATE hoard.chunks SET referenced = 0 "
				"WHERE referenced == 1 AND tag NOTNULL AND "
				"tag NOT IN (SELECT tag FROM hoard.refs)",
				LOG_WARNING,
				"chunks with spurious referenced flag"))
		goto bad;
	if (cleanup_action(state->db, "UPDATE hoard.chunks SET referenced = 1 "
				"WHERE referenced == 0 AND tag IN "
				"(SELECT tag FROM hoard.refs)",
				LOG_WARNING,
				"chunks with missing referenced flag"))
		goto bad;

	curtime=time(NULL);
	if (!query(&qry, state->db, "UPDATE hoard.chunks SET last_access = ? "
				"WHERE last_access > ?", "dd", curtime,
				curtime + 10)) {
		sql_log_err(state->db, "Couldn't locate chunks with invalid "
					"timestamps");
		goto bad;
	}
	query_row(qry, "d", &count);
	query_free(qry);
	if (count)
		pk_log(LOG_WARNING, "Repaired %d chunks with timestamps in "
					"the future", count);

	/* If we're going to do a FULL_CHECK, then get the necessary metadata
	   *now* while we still know it's consistent. */
	if (state->conf->flags & WANT_FULL_CHECK) {
		if (!query(NULL, state->db, "CREATE TEMP TABLE to_check AS "
					"SELECT tag, offset, length, crypto "
					"FROM hoard.chunks WHERE tag NOTNULL "
					"ORDER BY offset", NULL)) {
			sql_log_err(state->db, "Couldn't enumerate hoarded "
						"chunks");
			goto bad;
		}
	}

	if (!commit(state->db))
		goto bad;

	if (state->conf->flags & WANT_FULL_CHECK)
		if (check_hoard_data(state))
			return 1;
	/* XXX compact */
	return 0;

bad:
	retry = query_busy(state->db);
	rollback(state->db);
	if (retry) {
		query_backoff(state->db);
		goto again;
	}
	return 1;
}

int hoard_refresh(struct pk_state *state)
{
	if (hoard_sync_refs(state, FALSE))
		return 1;
	return 0;
}
