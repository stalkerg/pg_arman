/*-------------------------------------------------------------------------
 *
 * backup.c: backup DB cluster, archived WAL
 *
 * Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_arman.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#include "catalog/pg_control.h"
#include "libpq/pqsignal.h"
#include "pgut/pgut-port.h"
#include "parsexlog.h"

/* wait 10 sec until WAL archive complete */
#define TIMEOUT_ARCHIVE		10

/* Server version */
static int server_version = 0;

static bool		 in_backup = false;	/* TODO: more robust logic */
/* List of commands to execute at error processing for snapshot */
ControlFileData control_file;
parray			*backup_files_list; /* backup file list from non-snapshot */

/*
 * Backup routines
 */
static void backup_cleanup(bool fatal, void *userdata);
static void backup_files(const char *from_root, const char *to_root,
	parray *files, parray *prev_files, const XLogRecPtr *lsn, bool compress, const char *prefix);
static parray *do_backup_database(parray *backup_list, pgBackupOption bkupopt);
static void confirm_block_size(const char *name, int blcksz);
static void pg_start_backup(const char *label, bool smooth, pgBackup *backup);
static void pg_stop_backup(pgBackup *backup);
static bool pg_is_standby(void);
static void get_lsn(PGresult *res, XLogRecPtr *lsn);
static void get_xid(PGresult *res, uint32 *xid);

static void add_files(parray *files, const char *root, bool add_root, bool is_pgdata);
static void create_file_list(parray *files,
							 const char *root,
							 const char *subdir,
							 const char *prefix,
							 bool is_append);
static TimeLineID get_current_timeline(void);
static TimeLineHistoryEntry *
getTimelineHistory(ControlFileData *controlFile, int *nentries);
static TimeLineHistoryEntry *
rewind_parseTimeLineHistory(char *buffer, TimeLineID targetTLI, int *nentries);
static void wait_for_archive(pgBackup *backup, const char *sql);


/*
 * Take a backup of database and return the list of files backed up.
 */
static parray *
do_backup_database(parray *backup_list, pgBackupOption bkupopt)
{
	int			i;
	parray	   *files = NULL;
	parray	   *prev_files = NULL;	/* file list of previous database backup */
	FILE	   *fp;
	char		path[MAXPGPATH];
	char		label[1024];
	XLogRecPtr *lsn = NULL;
	char		prev_file_txt[MAXPGPATH];	/* path of the previous backup
											 * list file */
	bool		has_backup_label  = true;	/* flag if backup_label is there */

	/* repack the options */
	bool	smooth_checkpoint = bkupopt.smooth_checkpoint;
	pgBackup   *prev_backup = NULL;

	/* Block backup operations on a standby */
	if (pg_is_standby())
		elog(ERROR_SYSTEM, _("Backup cannot run on a standby."));

	elog(INFO, _("database backup start"));

	/* Initialize size summary */
	current.data_bytes = 0;

	/*
	 * Obtain current timeline by scanning control file, theh LSN
	 * obtained at output of pg_start_backup or pg_stop_backup does
	 * not contain this information.
	 */
	current.tli = get_current_timeline();

	/*
	 * In differential backup mode, check if there is an already-validated
	 * full backup on current timeline.
	 */
	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE)
	{
		pgBackup   *prev_backup;

		prev_backup = catalog_get_last_data_backup(backup_list, current.tli);
		if (prev_backup == NULL)
			elog(ERROR_SYSTEM, _("Valid full backup not found for "
					"differential backup. Either create a full backup "
					"or validate existing one."));
	}

	/* notify start of backup to PostgreSQL server */
	time2iso(label, lengthof(label), current.start_time);
	strncat(label, " with pg_arman", lengthof(label));
	pg_start_backup(label, smooth_checkpoint, &current);

	/* If backup_label does not exist in $PGDATA, stop taking backup */
	snprintf(path, lengthof(path), "%s/backup_label", pgdata);
	make_native_path(path);
	if (!fileExists(path))
		has_backup_label = false;

	/* Leave if no backup file */
	if (!has_backup_label)
	{
		if (verbose)
			printf(_("backup_label does not exist, stop backup\n"));
		pg_stop_backup(NULL);
		elog(ERROR_SYSTEM, _("backup_label does not exist in PGDATA."));
	}

	/*
	 * List directories and symbolic links with the physical path to make
	 * mkdirs.sh, then sort them in order of path. Omit $PGDATA.
	 */
	files = parray_new();
	dir_list_file(files, pgdata, NULL, false, false);

	if (!check)
	{
		pgBackupGetPath(&current, path, lengthof(path), MKDIRS_SH_FILE);
		fp = fopen(path, "wt");
		if (fp == NULL)
			elog(ERROR_SYSTEM, _("can't open make directory script \"%s\": %s"),
				path, strerror(errno));
		dir_print_mkdirs_sh(fp, files, pgdata);
		fclose(fp);
		if (chmod(path, DIR_PERMISSION) == -1)
			elog(ERROR_SYSTEM, _("can't change mode of \"%s\": %s"), path,
				strerror(errno));
	}

	/* clear directory list */
	parray_walk(files, pgFileFree);
	parray_free(files);
	files = NULL;

	/*
	 * To take differential backup, the file list of the last completed database
	 * backup is needed.
	 */
	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE)
	{
		/* find last completed database backup */
		prev_backup = catalog_get_last_data_backup(backup_list, current.tli);
		pgBackupGetPath(prev_backup, prev_file_txt, lengthof(prev_file_txt),
			DATABASE_FILE_LIST);
		prev_files = dir_read_file_list(pgdata, prev_file_txt);

		/*
		 * Do backup only pages having larger LSN than previous backup.
		 */
		lsn = &prev_backup->start_lsn;
		elog(LOG, _("backup only the page that there was of the update from LSN(%X/%08X).\n"),
			 (uint32) (*lsn >> 32), (uint32) *lsn);
	}

	/* initialize backup list from non-snapshot */
	backup_files_list = parray_new();

	/* list files with the logical path. omit $PGDATA */
	add_files(backup_files_list, pgdata, false, true);

	/* backup files */
	pgBackupGetPath(&current, path, lengthof(path), DATABASE_DIR);

	/* build pagemaps if diff mode */
	if (current.backup_mode == BACKUP_MODE_DIFF_PAGE)
	{
		int current_tli_index = -1;
		timeline_history = getTimelineHistory(&control_file, &timeline_num_entries);
		for (i = 0; i < timeline_num_entries; i++)
		{
			if (timeline_history[i].tli == current.tli) {
				current_tli_index = i;
				printf("Find timeline index\n");
				break;
			}
		}
		if (current_tli_index < 0) {
			printf("Can't find timeline index\n");
			exit(1);
		}
		parray_qsort(backup_files_list, pgFileComparePathDesc);
		/* need last full segment */
		wait_for_archive(&current, "SELECT * FROM pg_switch_xlog()");

		/* for debug now, maybe need remove*/
		if (verbose)
		{
			printf("extractPageMap\n");
			printf("current_tli_index:%X  current_tli:%X\n", current_tli_index, current.tli);
			printf("prev_backup->start_lsn: %X/%X\n", (uint32) (prev_backup->start_lsn >> 32), (uint32) (prev_backup->start_lsn));
			printf("current.start_lsn: %X/%X\n", (uint32) (current.start_lsn >> 32), (uint32) (current.start_lsn));
		}
		extractPageMap(arclog_path, prev_backup->start_lsn, current_tli_index,
					   current.start_lsn);
	}

	backup_files(pgdata, path, backup_files_list, prev_files, lsn, current.compress_data, NULL);

	/* notify end of backup */
	pg_stop_backup(&current);

	/* create file list */
	create_file_list(backup_files_list, pgdata, DATABASE_FILE_LIST, NULL, false);

	/* print summary of size of backup mode files */
	for (i = 0; i < parray_num(backup_files_list); i++)
	{
		pgFile *file = (pgFile *) parray_get(backup_files_list, i);
		if (!S_ISREG(file->mode))
			continue;
		/*
		 * Count only the amount of data. For a full backup, the total
		 * amount of data written counts while for an differential
		 * backup only the data read counts.
		 */
		if (current.backup_mode == BACKUP_MODE_DIFF_PAGE)
			current.data_bytes += file->read_size;
		else if (current.backup_mode == BACKUP_MODE_FULL)
			current.data_bytes += file->size;
	}

	if (verbose)
	{
		printf(_("database backup completed(Backup: " INT64_FORMAT ")\n"),
			current.data_bytes);
		printf(_("========================================\n"));
	}

	return backup_files_list;
}


int
do_backup(pgBackupOption bkupopt)
{
	parray *backup_list;
	parray *files_database;
	int		ret;

	/* repack the necessary options */
	int	keep_data_generations = bkupopt.keep_data_generations;
	int	keep_data_days        = bkupopt.keep_data_days;

	/* PGDATA and BACKUP_MODE are always required */
	if (pgdata == NULL)
		elog(ERROR_ARGS, _("Required parameter not specified: PGDATA "
						   "(-D, --pgdata)"));

	/* A backup mode is needed */
	if (current.backup_mode == BACKUP_MODE_INVALID)
		elog(ERROR_ARGS, _("Required parameter not specified: BACKUP_MODE "
						   "(-b, --backup-mode)"));

	/* ARCLOG_PATH is required only when backup archive WAL */
	if (HAVE_ARCLOG(&current) && arclog_path == NULL)
		elog(ERROR_ARGS,
			_("required parameter not specified: ARCLOG_PATH (-A, --arclog-path)"));

#ifndef HAVE_LIBZ
	if (current.compress_data)
	{
		elog(WARNING, _("requested compression not available in this "
						"installation. Archive will not be compressed"));
		current.compress_data = false;
	}
#endif

	/* Confirm data block size and xlog block size are compatible */
	check_server_version();

	/* setup cleanup callback function */
	in_backup = true;

	/* show configuration actually used */
	if (verbose)
	{
		printf(_("========================================\n"));
		printf(_("backup start\n"));
		printf(_("----------------------------------------\n"));
		pgBackupWriteConfigSection(stderr, &current);
		printf(_("----------------------------------------\n"));
	}

	/* get exclusive lock of backup catalog */
	ret = catalog_lock();
	if (ret == -1)
		elog(ERROR_SYSTEM, _("can't lock backup catalog."));
	else if (ret == 1)
		elog(ERROR_ALREADY_RUNNING,
			_("another pg_arman is running, skip this backup."));

	/* initialize backup result */
	current.status = BACKUP_STATUS_RUNNING;
	current.tli = 0;		/* get from result of pg_start_backup() */
	current.start_lsn = 0;
	current.stop_lsn = 0;
	current.start_time = time(NULL);
	current.end_time = (time_t) 0;
	current.data_bytes = BYTES_INVALID;
	current.block_size = BLCKSZ;
	current.wal_block_size = XLOG_BLCKSZ;
	current.recovery_xid = 0;
	current.recovery_time = (time_t) 0;

	/* create backup directory and backup.ini */
	if (!check)
	{
		if (pgBackupCreateDir(&current))
			elog(ERROR_SYSTEM, _("can't create backup directory."));
		pgBackupWriteIni(&current);
	}
	if (verbose)
		printf(_("backup destination is initialized.\n"));

	/* get list of backups already taken */
	backup_list = catalog_get_backup_list(NULL);
	if (!backup_list)
		elog(ERROR_SYSTEM, _("can't process any more."));

	/* set the error processing function for the backup process */
	pgut_atexit_push(backup_cleanup, NULL);

	/* backup data */
	files_database = do_backup_database(backup_list, bkupopt);
	pgut_atexit_pop(backup_cleanup, NULL);

	/* update backup status to DONE */
	current.end_time = time(NULL);
	current.status = BACKUP_STATUS_DONE;
	if (!check)
		pgBackupWriteIni(&current);

	/* Calculate the total data read */
	if (verbose)
	{
		int64 total_read = 0;

		/* Database data */
		if (current.backup_mode == BACKUP_MODE_FULL ||
			current.backup_mode == BACKUP_MODE_DIFF_PAGE)
			total_read += current.data_bytes;

		if (total_read == 0)
			printf(_("nothing to backup\n"));
		else
			printf(_("all backup completed(read: " INT64_FORMAT " write: "
				INT64_FORMAT ")\n"),
				total_read, current.data_bytes);
		printf(_("========================================\n"));
	}


	/* Delete old backup files after all backup operation. */
	pgBackupDelete(keep_data_generations, keep_data_days);

	/* Cleanup backup mode file list */
	if (files_database)
		parray_walk(files_database, pgFileFree);
	parray_free(files_database);

	/* release catalog lock */
	catalog_unlock();

	return 0;
}

/*
 * get server version and confirm block sizes.
 */
void
check_server_version(void)
{
	bool		my_conn;

	/* Leave if server has already been checked */
	if (server_version > 0)
		return;

	my_conn = (connection == NULL);

	if (my_conn)
		reconnect();

	/* confirm server version */
	server_version = PQserverVersion(connection);
	if (server_version != PG_VERSION_NUM)
		elog(ERROR_PG_INCOMPATIBLE,
			_("server version is %d.%d.%d, must be %s or higher."),
			 server_version / 10000,
			 (server_version / 100) % 100,
			 server_version % 100, PG_MAJORVERSION);

	/* confirm block_size (BLCKSZ) and wal_block_size (XLOG_BLCKSZ) */
	confirm_block_size("block_size", BLCKSZ);
	confirm_block_size("wal_block_size", XLOG_BLCKSZ);

	if (my_conn)
		disconnect();
}

static void
confirm_block_size(const char *name, int blcksz)
{
	PGresult   *res;
	char	   *endp;
	int			block_size;

	res = execute("SELECT current_setting($1)", 1, &name);
	if (PQntuples(res) != 1 || PQnfields(res) != 1)
		elog(ERROR_PG_COMMAND, _("can't get %s: %s"),
			name, PQerrorMessage(connection));
	block_size = strtol(PQgetvalue(res, 0, 0), &endp, 10);
	PQclear(res);
	if ((endp && *endp) || block_size != blcksz)
		elog(ERROR_PG_INCOMPATIBLE,
			_("%s(%d) is not compatible(%d expected)"),
			name, block_size, blcksz);
}

/*
 * Notify start of backup to PostgreSQL server.
 */
static void
pg_start_backup(const char *label, bool smooth, pgBackup *backup)
{
	PGresult	   *res;
	const char	   *params[2];

	params[0] = label;

	reconnect();

	/* 2nd argument is 'fast'*/
	params[1] = smooth ? "false" : "true";
	res = execute("SELECT pg_start_backup($1, $2)", 2, params);

	if (backup != NULL)
		get_lsn(res, &backup->start_lsn);
	PQclear(res);
	disconnect();
}

static void
wait_for_archive(pgBackup *backup, const char *sql)
{
	PGresult	   *res;
	char			ready_path[MAXPGPATH];
	char			file_name[MAXFNAMELEN];
	int				try_count;
	XLogRecPtr		lsn;
	TimeLineID		tli;
	XLogSegNo	targetSegNo;

	reconnect();
	res = execute(sql, 0, NULL);

	/* Get LSN from execution result */
	get_lsn(res, &lsn);

	/*
	 * Enforce TLI obtention if backup is not present as this code
	 * path can be taken as a callback at exit.
	 */
	tli = get_current_timeline();

	/* Fill in fields if backup exists */
	if (backup != NULL)
	{
		backup->tli = tli;
		backup->stop_lsn = lsn;
		elog(LOG, _("%s(): tli=%X lsn=%X/%08X"),
			 __FUNCTION__, backup->tli,
			 (uint32) (backup->stop_lsn >> 32),
			 (uint32) backup->stop_lsn);
	}

	/* As well as WAL file name */
	XLByteToSeg(lsn, targetSegNo);
	XLogFileName(file_name, tli, targetSegNo);

	snprintf(ready_path, lengthof(ready_path),
		"%s/pg_xlog/archive_status/%s.ready", pgdata,
			 file_name);
	if (verbose)
		printf("%s() wait for %s\n", __FUNCTION__, ready_path);

	PQclear(res);

	res = execute(TXID_CURRENT_SQL, 0, NULL);
	if(backup != NULL){
		get_xid(res, &backup->recovery_xid);
		backup->recovery_time = time(NULL);
	}
	disconnect();

	/* wait until switched WAL is archived */
	try_count = 0;
	while (fileExists(ready_path))
	{
		sleep(1);
		if (interrupted)
			elog(ERROR_INTERRUPTED,
				_("interrupted during waiting for WAL archiving"));
		try_count++;
		if (try_count > TIMEOUT_ARCHIVE)
			elog(ERROR_ARCHIVE_FAILED,
				_("switched WAL could not be archived in %d seconds"),
				TIMEOUT_ARCHIVE);
	}
	elog(LOG, "%s() .ready deleted in %d try", __FUNCTION__, try_count);
}

/*
 * Notify end of backup to PostgreSQL server.
 */
static void
pg_stop_backup(pgBackup *backup)
{
	wait_for_archive(backup,
		"SELECT * FROM pg_stop_backup()");
}


/*
 * Check if node is a standby by looking at the presence of
 * recovery.conf.
 */
static bool
pg_is_standby(void)
{
	char	path[MAXPGPATH];
	snprintf(path, lengthof(path), "%s/recovery.conf", pgdata);
	make_native_path(path);
	return fileExists(path);
}

/*
 * Get LSN from result of pg_start_backup() or pg_stop_backup().
 */
static void
get_lsn(PGresult *res, XLogRecPtr *lsn)
{
	uint32	xlogid;
	uint32	xrecoff;

	if (res == NULL || PQntuples(res) != 1 || PQnfields(res) != 1)
		elog(ERROR_PG_COMMAND,
			_("result of backup command is invalid: %s"),
			PQerrorMessage(connection));

	/*
	 * Extract timeline and LSN from results of pg_stop_backup()
	 * and friends.
	 */
	XLogDataFromLSN(PQgetvalue(res, 0, 0), &xlogid, &xrecoff);

	/* Calculate LSN */
	*lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
}

/*
 * Get XID from result of txid_current() after pg_stop_backup().
 */
static void
get_xid(PGresult *res, uint32 *xid)
{
	if(res == NULL || PQntuples(res) != 1 || PQnfields(res) != 1)
		elog(ERROR_PG_COMMAND,
			_("result of txid_current() is invalid: %s"),
			PQerrorMessage(connection));

	if(sscanf(PQgetvalue(res, 0, 0), "%u", xid) != 1)
	{
		elog(ERROR_PG_COMMAND,
			_("result of txid_current() is invalid: %s"),
			PQerrorMessage(connection));
	}
	elog(LOG, "%s():%s", __FUNCTION__, PQgetvalue(res, 0, 0));
}

/*
 * Return true if the path is a existing regular file.
 */
bool
fileExists(const char *path)
{
	struct stat buf;

	if (stat(path, &buf) == -1 && errno == ENOENT)
		return false;
	else if (!S_ISREG(buf.st_mode))
		return false;
	else
		return true;
}

/*
 * Notify end of backup to server when "backup_label" is in the root directory
 * of the DB cluster.
 * Also update backup status to ERROR when the backup is not finished.
 */
static void
backup_cleanup(bool fatal, void *userdata)
{
	char path[MAXPGPATH];

	if (!in_backup)
		return;

	/* If backup_label exist in $PGDATA, notify stop of backup to PostgreSQL */
	snprintf(path, lengthof(path), "%s/backup_label", pgdata);
	make_native_path(path);
	if (fileExists(path))
	{
		if (verbose)
			printf(_("backup_label exists, stop backup\n"));
		pg_stop_backup(NULL);	/* don't care stop_lsn on error case */
	}

	/*
	 * Update status of backup.ini to ERROR.
	 * end_time != 0 means backup finished
	 */
	if (current.status == BACKUP_STATUS_RUNNING && current.end_time == 0)
	{
		if (verbose)
			printf(_("backup is running, update its status to ERROR\n"));
		current.end_time = time(NULL);
		current.status = BACKUP_STATUS_ERROR;
		pgBackupWriteIni(&current);
	}
}

/*
 * Take differential backup at page level.
 */
static void
backup_files(const char *from_root,
			 const char *to_root,
			 parray *files,
			 parray *prev_files,
			 const XLogRecPtr *lsn,
			 bool compress,
			 const char *prefix)
{
	int				i;
	struct timeval	tv;

	/* sort pathname ascending */
	parray_qsort(files, pgFileComparePath);

	gettimeofday(&tv, NULL);

	/* backup a file or create a directory */
	for (i = 0; i < parray_num(files); i++)
	{
		int			ret;
		struct stat	buf;

		pgFile *file = (pgFile *) parray_get(files, i);

		/* If current time is rewinded, abort this backup. */
		if(tv.tv_sec < file->mtime){
			elog(ERROR_SYSTEM, _("current time may be rewound. Please retry with full backup mode."));
		}

		/* check for interrupt */
		if (interrupted)
			elog(ERROR_INTERRUPTED, _("interrupted during backup"));

		/* print progress in verbose mode */
		if (verbose)
		{
			if (prefix)
			{
				char path[MAXPGPATH];
				join_path_components(path, prefix, file->path + strlen(from_root) + 1);
				printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files), path);
			}
			else
				printf(_("(%d/%lu) %s "), i + 1, (unsigned long) parray_num(files),
					file->path + strlen(from_root) + 1);
		}

		/* stat file to get file type, size and modify timestamp */
		ret = stat(file->path, &buf);
		if (ret == -1)
		{
			if (errno == ENOENT)
			{
				/* record as skipped file in file_xxx.txt */
				file->write_size = BYTES_INVALID;
				if (verbose)
					printf(_("skip\n"));
				continue;
			}
			else
			{
				if (verbose)
					printf("\n");
				elog(ERROR_SYSTEM,
					_("can't stat backup mode. \"%s\": %s"),
					file->path, strerror(errno));
			}
		}

		/* if the entry was a directory, create it in the backup */
		if (S_ISDIR(buf.st_mode))
		{
			char dirpath[MAXPGPATH];

			join_path_components(dirpath, to_root, JoinPathEnd(file->path, from_root));
			if (!check){
				dir_create_dir(dirpath, DIR_PERMISSION);
			}
			if (verbose)
				printf(_("directory\n"));
		}
		else if (S_ISREG(buf.st_mode))
		{
			/* skip files which have not been modified since last backup */
			if (prev_files)
			{
				pgFile *prev_file = NULL;

				/*
				 * If prefix is not NULL, the table space is backup from the snapshot.
				 * Therefore, adjust file name to correspond to the file list.
				 */
				if (prefix)
				{
					int j;

					for (j = 0; j < parray_num(prev_files); j++)
					{
						pgFile *p = (pgFile *) parray_get(prev_files, j);
						char *prev_path;
						char curr_path[MAXPGPATH];

						prev_path = p->path + strlen(from_root) + 1;
						join_path_components(curr_path, prefix, file->path + strlen(from_root) + 1);
						if (strcmp(curr_path, prev_path) == 0)
						{
							prev_file = p;
							break;
						}
					}
				}
				else
				{
					pgFile **p = (pgFile **) parray_bsearch(prev_files, file, pgFileComparePath);
					if (p)
						prev_file = *p;
				}

				if (prev_file && prev_file->mtime == file->mtime)
				{
					/* record as skipped file in file_xxx.txt */
					file->write_size = BYTES_INVALID;
					if (verbose)
						printf(_("skip\n"));
					continue;
				}

				/* if the file was not in the previous backup simply copy it  */
				if (prev_file == NULL)
					file->is_datafile = false;
			}

			/*
			 * We will wait until the next second of mtime so that backup
			 * file should contain all modifications at the clock of mtime.
			 * timer resolution of ext3 file system is one second.
			 */

			if (tv.tv_sec == file->mtime)
			{
				/* update time and recheck */
				gettimeofday(&tv, NULL);
				while (tv.tv_sec <= file->mtime)
				{
					usleep(1000000 - tv.tv_usec);
					gettimeofday(&tv, NULL);
				}
			}

			/* copy the file into backup */
			if (!(file->is_datafile
					? backup_data_file(from_root, to_root, file, lsn, compress)
					: copy_file(from_root, to_root, file,
								compress ? COMPRESSION : NO_COMPRESSION)))
			{
				/* record as skipped file in file_xxx.txt */
				file->write_size = BYTES_INVALID;
				if (verbose)
					printf(_("skip\n"));
				continue;
			}

			if (verbose)
			{
				/* print compression rate */
				if (file->write_size != file->size)
					printf(_("compressed %lu (%.2f%% of %lu)\n"),
						(unsigned long) file->write_size,
						100.0 * file->write_size / file->size,
						(unsigned long) file->size);
				else
					printf(_("copied %lu\n"), (unsigned long) file->write_size);
			}

		}
		else
		{
			if (verbose)
				printf(_(" unexpected file type %d\n"), buf.st_mode);
		}
	}
}


/*
 * Append files to the backup list array.
 */
static void
add_files(parray *files, const char *root, bool add_root, bool is_pgdata)
{
	parray	*list_file;
	int		 i;

	list_file = parray_new();

	/* list files with the logical path. omit $PGDATA */
	dir_list_file(list_file, root, pgdata_exclude, true, add_root);

	/* mark files that are possible datafile as 'datafile' */
	for (i = 0; i < parray_num(list_file); i++)
	{
		pgFile *file = (pgFile *) parray_get(list_file, i);
		char *relative;
		char *fname;

		/* data file must be a regular file */
		if (!S_ISREG(file->mode))
			continue;

		/* data files are under "base", "global", or "pg_tblspc" */
		relative = file->path + strlen(root) + 1;
		if (is_pgdata &&
			!path_is_prefix_of_path("base", relative) &&
			!path_is_prefix_of_path("global", relative) &&
			!path_is_prefix_of_path("pg_tblspc", relative))
			continue;

		/* name of data file start with digit */
		fname = last_dir_separator(relative);
		if (fname == NULL)
			fname = relative;
		else
			fname++;
		if (!isdigit(fname[0]))
			continue;

		file->is_datafile = true;
	}
	parray_concat(files, list_file);
}

/*
 * Output the list of files to backup catalog
 */
static void
create_file_list(parray *files,
				 const char *root,
				 const char *subdir,
				 const char *prefix,
				 bool is_append)
{
	FILE	*fp;
	char	 path[MAXPGPATH];

	if (!check)
	{
		/* output path is '$BACKUP_PATH/file_database.txt' */
		pgBackupGetPath(&current, path, lengthof(path), subdir);
		fp = fopen(path, is_append ? "at" : "wt");
		if (fp == NULL)
			elog(ERROR_SYSTEM, _("can't open file list \"%s\": %s"), path,
				strerror(errno));
		dir_print_file_list(fp, files, root, prefix);
		fclose(fp);
	}
}

/*
 * Check CRC of control file
 */
static void
checkControlFile(ControlFileData *ControlFile)
{
	pg_crc32c	crc;

	/* Calculate CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (char *) ControlFile, offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	/* And simply compare it */
	if (!EQ_CRC32C(crc, ControlFile->crc)) {
		printf("unexpected control file CRC\n");
		exit(1);
	}
}

/*
 * Verify control file contents in the buffer src, and copy it to *ControlFile.
 */
static void
digestControlFile(ControlFileData *ControlFile, char *src, size_t size)
{
	if (size != PG_CONTROL_SIZE) {
		printf("unexpected control file size %d, expected %d\n",
				 (int) size, PG_CONTROL_SIZE);
		exit(1);
	}

	memcpy(ControlFile, src, sizeof(ControlFileData));

	/* Additional checks on control file */
	checkControlFile(ControlFile);
}

/*
 * Scan control file of given cluster at obtain the current timeline
 * since last checkpoint that occurred on it.
 */
static TimeLineID
get_current_timeline(void)
{
	char	   *buffer;
	size_t		size;

	/* First fetch file... */
	buffer = slurpFile(pgdata, "global/pg_control", &size);

	/* .. Then interpret it */
    if (size != PG_CONTROL_SIZE)
		elog(ERROR_CORRUPTED, "unexpected control file size %d, expected %d\n",
			 (int) size, PG_CONTROL_SIZE);

	digestControlFile(&control_file, buffer, size);
	pg_free(buffer);

	/* Finally return the timeline wanted */
	return control_file.checkPointCopy.ThisTimeLineID;
}


/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component TLIs (the given TLI followed by
 * its ancestor TLIs).  If we can't find the history file, assume that the
 * timeline has no parents, and return a list of just the specified timeline
 * ID.
 */
static TimeLineHistoryEntry *
rewind_parseTimeLineHistory(char *buffer, TimeLineID targetTLI, int *nentries)
{
	char	   *fline;
	TimeLineHistoryEntry *entry;
	TimeLineHistoryEntry *entries = NULL;
	int			nlines = 0;
	TimeLineID	lasttli = 0;
	XLogRecPtr	prevend;
	char	   *bufptr;
	bool		lastline = false;

	/*
	 * Parse the file...
	 */
	prevend = InvalidXLogRecPtr;
	bufptr = buffer;
	while (!lastline)
	{
		char	   *ptr;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		fline = bufptr;
		while (*bufptr && *bufptr != '\n')
			bufptr++;
		if (!(*bufptr))
			lastline = true;
		else
			*bufptr++ = '\0';

		/* skip leading whitespace and check for # comment */
		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo);

		if (nfields < 1)
		{
			/* expect a numeric timeline ID as first field of line */
			fprintf(stderr, _("syntax error in history file: %s\n"), fline);
			fprintf(stderr, _("Expected a numeric timeline ID.\n"));
			exit(1);
		}
		if (nfields != 3)
		{
			fprintf(stderr, _("syntax error in history file: %s\n"), fline);
			fprintf(stderr, _("Expected a transaction log switchpoint location.\n"));
			exit(1);
		}
		if (entries && tli <= lasttli)
		{
			fprintf(stderr, _("invalid data in history file: %s\n"), fline);
			fprintf(stderr, _("Timeline IDs must be in increasing sequence.\n"));
			exit(1);
		}

		lasttli = tli;

		nlines++;
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));

		entry = &entries[nlines - 1];
		entry->tli = tli;
		entry->begin = prevend;
		entry->end = ((uint64) (switchpoint_hi)) << 32 | (uint64) switchpoint_lo;
		prevend = entry->end;
		//printf("begin:%X end:%X\n", entry->begin, entry->end);
		/* we ignore the remainder of each line */
	}

	if (entries && targetTLI <= lasttli)
	{
		fprintf(stderr, _("invalid data in history file\n"));
		fprintf(stderr, _("Timeline IDs must be less than child timeline's ID.\n"));
		exit(1);
	}

	/*
	 * Create one more entry for the "tip" of the timeline, which has no entry
	 * in the history file.
	 */
	nlines++;
	if (entries)
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));
	else
		entries = pg_malloc(1 * sizeof(TimeLineHistoryEntry));

	entry = &entries[nlines - 1];
	entry->tli = targetTLI;
	entry->begin = prevend;
	entry->end = InvalidXLogRecPtr;

	*nentries = nlines;
	return entries;
}


/*
 * Retrieve timeline history for given control file which should behold
 * either source or target.
 */
static TimeLineHistoryEntry *
getTimelineHistory(ControlFileData *controlFile, int *nentries)
{
	TimeLineHistoryEntry   *history;
	TimeLineID				tli;
	int		i;

	tli = controlFile->checkPointCopy.ThisTimeLineID;

	/*
	 * Timeline 1 does not have a history file, so there is no need to check and
	 * fake an entry with infinite start and end positions.
	 */
	if (tli == 1)
	{
		history = (TimeLineHistoryEntry *) pg_malloc(sizeof(TimeLineHistoryEntry));
		history->tli = tli;
		history->begin = history->end = InvalidXLogRecPtr;
		*nentries = 1;
	}
	else
	{
		char		path[MAXPGPATH];
		char	   *histfile;

		TLHistoryFilePath(path, tli);

		/* Get history file from appropriate source */
		histfile = slurpFile(pgdata, path, NULL);
		history = rewind_parseTimeLineHistory(histfile, tli, nentries);
		pg_free(histfile);
	}

	if (verbose)
	{
		printf("Timeline history:\n");
		/*
		 * Print the target timeline history.
		 */
		for (i = 0; i < *nentries; i++)
		{
			TimeLineHistoryEntry *entry;

			entry = &history[i];
			printf("%d: %X/%X - %X/%X\n", entry->tli,
				(uint32) (entry->begin >> 32), (uint32) (entry->begin),
				(uint32) (entry->end >> 32), (uint32) (entry->end));
		}
	}
	return history;
}


/*
 * A helper function to create the path of a relation file and segment.
 *
 * The returned path is palloc'd
 */
static char *
datasegpath(RelFileNode rnode, ForkNumber forknum, BlockNumber segno)
{
	char	   *path;
	char	   *segpath;

	path = relpathperm(rnode, forknum);
	if (segno > 0)
	{
		segpath = psprintf("%s.%u", path, segno);
		pfree(path);
		return segpath;
	}
	else
		return path;
}

/*
 * This callback gets called while we read the WAL in the target, for every
 * block that have changed in the target system. It makes note of all the
 * changed blocks in the pagemap of the file.
 */
void
arman_process_block_change(ForkNumber forknum, RelFileNode rnode, BlockNumber blkno)
{
	char		*path;
	char		*rel_path;
	BlockNumber blkno_inseg;
	int			segno;
	pgFile		*file_item;

	segno = blkno / RELSEG_SIZE;
	blkno_inseg = blkno % RELSEG_SIZE;

	rel_path = datasegpath(rnode, forknum, segno);
	path = pg_malloc(strlen(rel_path)+strlen(pgdata)+2);
	sprintf(path, "%s/%s", pgdata, rel_path);
	file_item = (pgFile *) parray_bsearch(backup_files_list, path, pgFileCharComparePath);

	if (file_item)
	{
		file_item = *(pgFile **)file_item;
		datapagemap_add(&file_item->pagemap, blkno_inseg);
	}
	else
	{
		/*
		 * If we don't have any record of this file in the file map, it means
		 * that it's a relation that doesn't exist in the source system, and
		 * it was subsequently removed in the target system, too. We can
		 * safely ignore it.
		 */
	}
	pg_free(path);
	pg_free(rel_path);
}