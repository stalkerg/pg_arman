#!/bin/bash

#============================================================================
# This is a test script for backup command of pg_arman.
#============================================================================

# Load common rules
. sql/common.sh backup

# Extra parameters exclusive to this test
SCALE=1
DURATION=10
USE_DATA_CHECKSUM=""

# command line option handling for this script
while [ $# -gt 0 ]; do
	case $1 in
		"-d")
			DURATION=`expr $2 + 0`
			if [ $? -ne 0 ]; then
				echo "invalid duration"
				exit 1
			fi
			shift 2
			;;
		"-s")
			SCALE=`expr $2 + 0`
			if [ $? -ne 0 ]; then
				echo "invalid scale"
				exit 1
			fi
			shift 2
			;;
		"--with-checksum")
			USE_DATA_CHECKSUM="--data-checksum"
			shift
			;;
		*)
			shift
			;;
	esac
done

# Check presence of pgbench command and initialize environment
which pgbench > /dev/null 2>&1
ERR_NUM=$?
if [ $ERR_NUM != 0 ]
then
    echo "pgbench is not installed in this environment."
    echo "It is needed in PATH for those regression tests."
    exit 1
fi

function cleanup()
{
    # cleanup environment
    pg_ctl stop -m immediate > /dev/null 2>&1
    rm -fr ${PGDATA_PATH}
    rm -fr ${BACKUP_PATH}
    rm -fr ${ARCLOG_PATH}
    rm -fr ${SRVLOG_PATH}
    rm -fr ${TBLSPC_PATH}
    mkdir -p ${ARCLOG_PATH}
    mkdir -p ${SRVLOG_PATH}
    mkdir -p ${TBLSPC_PATH}
}

function init_database()
{
    rm -rf ${PGDATA_PATH}
    rm -fr ${ARCLOG_PATH}
    rm -fr ${SRVLOG_PATH}
    rm -fr ${TBLSPC_PATH}
    mkdir -p ${ARCLOG_PATH}
    mkdir -p ${SRVLOG_PATH}
    mkdir -p ${TBLSPC_PATH}

    # create new database cluster
    initdb ${USE_DATA_CHECKSUM} --no-locale -D ${PGDATA_PATH} > ${TEST_BASE}/initdb.log 2>&1
    cp ${PGDATA_PATH}/postgresql.conf ${PGDATA_PATH}/postgresql.conf_org
    cat << EOF >> ${PGDATA_PATH}/postgresql.conf
port = ${TEST_PGPORT}
logging_collector = on
wal_level = hot_standby
log_directory = '${SRVLOG_PATH}'
log_filename = 'postgresql-%F_%H%M%S.log'
archive_mode = on
archive_command = 'cp %p ${ARCLOG_PATH}/%f'
max_wal_size = 512MB
EOF

    # start PostgreSQL
    pg_ctl start -D ${PGDATA_PATH} -w -t 300 > ${TEST_BASE}/pg_ctl.log 2>&1
	mkdir -p ${TBLSPC_PATH}/pgbench
	psql --no-psqlrc -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1 << EOF
CREATE TABLESPACE pgbench LOCATION '${TBLSPC_PATH}/pgbench';
CREATE DATABASE pgbench TABLESPACE = pgbench;
EOF

    pgbench -i -s ${SCALE} -p ${TEST_PGPORT} -d pgbench > ${TEST_BASE}/pgbench.log 2>&1

}

function init_catalog()
{
    rm -fr ${BACKUP_PATH}
    pg_arman init -B ${BACKUP_PATH} --quiet
}

cleanup
init_database
init_catalog

echo '###### BACKUP COMMAND TEST-0001 ######'
echo '###### full backup mode ######'
pg_arman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_arman validate -B ${BACKUP_PATH} --quiet
pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0001.log
grep OK ${TEST_BASE}/TEST-0001.log | sed -e 's@[^-]@@g' | wc -c


echo '###### BACKUP COMMAND TEST-0002 ######'
echo '###### page backup mode ######'
pg_arman backup -B ${BACKUP_PATH} -b page -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_arman validate -B ${BACKUP_PATH} --quiet
pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0002.log
grep OK ${TEST_BASE}/TEST-0002.log | sed -e 's@[^-]@@g' | wc -c

#archive mode not support in pg_arman
#echo '###### BACKUP COMMAND TEST-0003 ######'
#echo '###### archive backup mode ######'
#pg_arman backup -B ${BACKUP_PATH} -b archive -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_arman validate -B ${BACKUP_PATH} --quiet
#pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0003.log 2>&1
#grep -c OK ${TEST_BASE}/TEST-0003.log
#grep OK ${TEST_BASE}/TEST-0003.log | sed -e 's@[^-]@@g' | wc -c

#we not support server logs
#echo '###### BACKUP COMMAND TEST-0004 ######'
#echo '###### full backup with server log ######'
#init_catalog
#pg_arman backup -B ${BACKUP_PATH} -b full -s -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_arman validate -B ${BACKUP_PATH} --quiet
#pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004.log 2>&1
#grep -c OK ${TEST_BASE}/TEST-0004.log
#grep OK ${TEST_BASE}/TEST-0004.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0005 ######'
echo '###### full backup with compression ######'
init_catalog
pg_arman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_arman validate -B ${BACKUP_PATH} --quiet
pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0005.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0005.log
grep OK ${TEST_BASE}/TEST-0005.log | grep -c true
grep OK ${TEST_BASE}/TEST-0005.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0006 ######'
echo '###### full backup with smooth checkpoint ######'
init_catalog
pg_arman backup -B ${BACKUP_PATH} -b full -C -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_arman validate -B ${BACKUP_PATH} --quiet
pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0006.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0006.log
grep OK ${TEST_BASE}/TEST-0006.log | sed -e 's@[^-]@@g' | wc -c


#--full-backup-on-error option not supported in pg_arman
#echo '###### BACKUP COMMAND TEST-0010 ######'
#echo '###### switch backup mode from page to full ######'
#init_catalog
#echo 'page backup without validated full backup'
#pg_arman backup -B ${BACKUP_PATH} -b page -Z -p ${TEST_PGPORT} -d postgres;echo $?
#echo 'page backup in the same situation but with --full-backup-on-error option'
#pg_arman backup -B ${BACKUP_PATH} -b page -F -Z -p ${TEST_PGPORT} -d postgres;echo $?
#pg_arman validate -B ${BACKUP_PATH} --quiet
#pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0010.log 2>&1
#grep OK ${TEST_BASE}/TEST-0010.log | grep FULL | wc -l
#grep ERROR ${TEST_BASE}/TEST-0010.log | grep PAGE | wc -l

#archive mode not support in pg_arman
#echo '###### BACKUP COMMAND TEST-0011 ######'
#echo '###### switch backup mode from archive to full ######'
#init_catalog
#echo 'archive backup without validated full backup'
#pg_arman backup -B ${BACKUP_PATH} -b archive -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
#sleep 1
#echo 'archive backup in the same situation but with --full-backup-on-error option'
#pg_arman backup -B ${BACKUP_PATH} -b archive -F -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
#pg_arman validate -B ${BACKUP_PATH} --quiet
#pg_arman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0011.log 2>&1
#grep OK ${TEST_BASE}/TEST-0011.log | grep FULL | wc -l
#grep ERROR ${TEST_BASE}/TEST-0011.log | grep ARCH | wc -l

#not support now in pg_arman
#echo '###### BACKUP COMMAND TEST-0012 ######'
#echo '###### failure in backup with different system identifier database ######'
#init_catalog
#pg_ctl stop -m immediate > /dev/null 2>&1
#init_database
#pg_arman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT} -d postgres --quiet;echo $?


# cleanup
## clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
