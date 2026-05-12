#include "postgres.h"

#include <rados/librados.h>

#include "catalog/pg_tablespace_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/aio_internal.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC_EXT(
	.name = "rados_smgr",
	.version = "0.0.1",
);

typedef struct RadosRelationData {} radosRelationData;

static char *rados_id = "";
static char *rados_mon_host = "";
static char *rados_keyring = "";
static char *rados_pool = "";

static rados_t cluster;
static rados_ioctx_t io;

static uint32 wait_event_read;
static uint32 wait_event_write;
static uint32 wait_event_extend;
static uint32 wait_event_truncate;

static bool
use_md(RelFileLocatorBackend *rlocator)
{
	return RelFileLocatorBackendIsTemp(*rlocator) ||
		rlocator->locator.spcOid == GLOBALTABLESPACE_OID ||
		rlocator->locator.spcOid == DEFAULTTABLESPACE_OID;
}

static void
rados_smgr_init(void)
{
	int			err;

	wait_event_read = WaitEventExtensionNew("RadosSmgrRead");
	wait_event_write = WaitEventExtensionNew("RadosSmgrWrite");
	wait_event_extend = WaitEventExtensionNew("RadosSmgrExtend");
	wait_event_truncate = WaitEventExtensionNew("RadosSmgrTruncate");

	err = rados_create(&cluster, rados_id);
	if (err < 0)
		elog(ERROR, "cannot create a cluster handle: %s", strerror(-err));

	err = rados_conf_set(cluster, "mon_host", rados_mon_host);
	if (err < 0)
		elog(ERROR, "cannot set \"%s\": %s", "mon_host", strerror(-err));

	err = rados_conf_set(cluster, "keyring", rados_keyring);
	if (err < 0)
		elog(ERROR, "cannot set \"%s\": %s", "keyring", strerror(-err));

	err = rados_connect(cluster);
	if (err < 0)
		elog(ERROR, "cannot connect to cluster: %s", strerror(-err));

	err = rados_ioctx_create(cluster, rados_pool, &io);
	if (err < 0)
	{
        rados_shutdown(cluster);
		elog(ERROR, "cannot open rados pool %s: %s", rados_pool, strerror(-err));
	}
}

static void
rados_smgr_shutdown(void)
{
	rados_ioctx_destroy(io);
	rados_shutdown(cluster);
}

static void
rados_smgr_open(SMgrRelation reln)
{
	if (use_md(&reln->smgr_rlocator))
	{
		mdopen(reln);
		return;
	}
}

static void
rados_smgr_close(SMgrRelation reln, ForkNumber forknum)
{
	if (use_md(&reln->smgr_rlocator))
	{
		mdclose(reln, forknum);
		return;
	}
}

static void
rados_smgr_create(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	RelPathStr	path;
	int			err;

	if (use_md(&reln->smgr_rlocator))
	{
		mdcreate(reln, forknum, isRedo);
		return;
	}

	path = relpath(reln->smgr_rlocator, forknum);

	if (!isRedo)
	{
		err = rados_write_full(io, path.str, "", 0);
		if (err < 0)
			elog(ERROR, "could not write to object \"%s\": %s", path.str, strerror(-err));
	}
	else
	{
		err = rados_write(io, path.str, "", 0, 0);
		if (err < 0)
			elog(ERROR, "could not write to object \"%s\": %s", path.str, strerror(-err));
	}
}

static bool
rados_smgr_exists(SMgrRelation reln, ForkNumber forknum)
{
	RelPathStr	path;
	int			err;
	uint64_t	psize;
	time_t		pmtime;

	if (use_md(&reln->smgr_rlocator))
	{
		return mdexists(reln, forknum);
	}

	path = relpath(reln->smgr_rlocator, forknum);

	err = rados_stat(io, path.str, &psize, &pmtime);
	if (err < 0 && err != -ENOENT)
		elog(ERROR, "could not stat object \"%s\": %s", path.str, strerror(-err));

	return err == 0;
}

static void
rados_smgr_unlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	RelPathStr	path;
	int			err;

	if (use_md(&rlocator))
	{
		mdunlink(rlocator, forknum, isRedo);
	}

	path = relpath(rlocator, forknum);

	err = rados_remove(io, path.str);
	if (err < 0 && err != -ENOENT)
		elog(WARNING, "could not remove object \"%s\": %s", path.str, strerror(-err));
}

static void
rados_smgr_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  const void *buffer, bool skipFsync)
{
	RelPathStr	path;
	int			err;

	if (use_md(&reln->smgr_rlocator))
	{
		mdextend(reln, forknum, blocknum, buffer, skipFsync);
		return;
	}

	path = relpath(reln->smgr_rlocator, forknum);

	// TODO: skipFsync
	pgstat_report_wait_start(wait_event_extend);
	err = rados_write(io, path.str, buffer, BLCKSZ, blocknum * BLCKSZ);
	pgstat_report_wait_end();

	if (err < 0 && err != -ENOENT)
		elog(WARNING, "could not write to object \"%s\": %s", path.str, strerror(-err));
}

static void
rados_smgr_zeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					  int nblocks, bool skipFsync)
{
	RelPathStr	path;
	int			err;

	if (use_md(&reln->smgr_rlocator))
	{
		mdzeroextend(reln, forknum, blocknum, nblocks, skipFsync);
		return;
	}

	path = relpath(reln->smgr_rlocator, forknum);

	// TODO: skipFsync
	pgstat_report_wait_start(wait_event_extend);
	err = rados_trunc(io, path.str, (blocknum + nblocks) * BLCKSZ);
	pgstat_report_wait_end();

	if (err < 0)
		elog(ERROR, "could not truncate object \"%s\": %s", path.str, strerror(-err));
}

static bool
rados_smgr_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					int nblocks)
{
	if (use_md(&reln->smgr_rlocator))
	{
		return mdprefetch(reln, forknum, blocknum, nblocks);
	}

	// TODO: Implement me
	return false;
}

static uint32
rados_smgr_maxcombine(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	if (use_md(&reln->smgr_rlocator))
	{
		return mdmaxcombine(reln, forknum, blocknum);
	}

	return (UINT_MAX / 2) / BLCKSZ;
}

static void
rados_smgr_readv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				 void **buffers, BlockNumber nblocks)
{
	RelPathStr	path;
	int			err;
	rados_read_op_t op;
	// TODO: Do something with bytes_read and rval?
	size_t		bytes_read;
	int			rval;

	if (use_md(&reln->smgr_rlocator))
	{
		return mdreadv(reln, forknum, blocknum, buffers, nblocks);
	}

	path = relpath(reln->smgr_rlocator, forknum);

	// TODO: Async?
	op = rados_create_read_op();
	if (!op)
		ereport(ERROR, errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory"));

	for (BlockNumber bn = 0; bn < nblocks; bn++)
		rados_read_op_read(op, (blocknum + bn) * BLCKSZ, BLCKSZ, buffers[bn], &bytes_read, &rval);

	pgstat_report_wait_start(wait_event_read);
	err = rados_read_op_operate(op, io, path.str, 0);
	pgstat_report_wait_end();

	rados_release_read_op(op);

	if (err < 0)
		elog(ERROR, "could not read from object \"%s\": %s", path.str, strerror(-err));
}

static void
rados_smgr_writev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	RelPathStr	path;
	int			err;
	rados_write_op_t op;

	if (use_md(&reln->smgr_rlocator))
	{
		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
		return;
	}

	path = relpath(reln->smgr_rlocator, forknum);

	// TODO: Async?
	op = rados_create_write_op();
	if (!op)
		ereport(ERROR, errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory"));

	// TODO: skipFsync
	for (BlockNumber bn = 0; bn < nblocks; bn++)
		rados_write_op_write(op, buffers[bn], BLCKSZ, (blocknum + bn) * BLCKSZ);

	pgstat_report_wait_start(wait_event_write);
	err = rados_write_op_operate(op, io, path.str, NULL, 0);
	pgstat_report_wait_end();

	rados_release_write_op(op);

	if (err < 0)
		elog(ERROR, "could not write to object \"%s\": %s", path.str, strerror(-err));
}

static void
rados_smgr_writeback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				 	 BlockNumber nblocks)
{
	if (use_md(&reln->smgr_rlocator))
	{
		mdwriteback(reln, forknum, blocknum, nblocks);
		return;
	}

	// Do nothing?
}

static BlockNumber
rados_smgr_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	RelPathStr	path;
	int			err;
	uint64_t	psize;
	time_t		pmtime;

	if (use_md(&reln->smgr_rlocator))
	{
		return mdnblocks(reln, forknum);
	}

	path = relpath(reln->smgr_rlocator, forknum);

	err = rados_stat(io, path.str, &psize, &pmtime);
	if (err < 0)
		elog(ERROR, "could not stat object \"%s\": %s", path.str, strerror(-err));

	return psize / BLCKSZ;
}

static void
rados_smgr_truncate(SMgrRelation reln, ForkNumber forknum, BlockNumber old_blocks,
					BlockNumber nblocks)
{
	RelPathStr	path;
	int			err;

	if (use_md(&reln->smgr_rlocator))
	{
		mdtruncate(reln, forknum, old_blocks, nblocks);
		return;
	}

	path = relpath(reln->smgr_rlocator, forknum);

	pgstat_report_wait_start(wait_event_truncate);
	err = rados_trunc(io, path.str, 0);
	pgstat_report_wait_end();

	if (err < 0)
		elog(ERROR, "could not truncate object \"%s\": %s", path.str, strerror(-err));
}

static void
rados_smgr_immedsync(SMgrRelation reln, ForkNumber forknum)
{
	if (use_md(&reln->smgr_rlocator))
	{
		mdimmedsync(reln, forknum);
		return;
	}

	// Do nothing?
}

static void
rados_smgr_registersync(SMgrRelation reln, ForkNumber forknum)
{
	if (use_md(&reln->smgr_rlocator))
	{
		mdregistersync(reln, forknum);
		return;
	}

	// Do nothing?
}

static void
rados_smgr_startreadv(PgAioHandle *ioh, SMgrRelation reln,
					  ForkNumber forknum, BlockNumber blocknum,
					  void **buffers, BlockNumber nblocks)
{
	if (use_md(&reln->smgr_rlocator))
	{
		mdstartreadv(ioh, reln, forknum, blocknum, buffers, nblocks);
		return;
	}

	rados_smgr_readv(reln, forknum, blocknum, buffers, nblocks);

	// TODO: Expose custom AIO ops instead of faking AIO
	pgaio_io_set_target_smgr(ioh,
							 reln,
							 forknum,
							 blocknum,
							 nblocks,
							 false);

	ioh->op = PGAIO_OP_READV;
	ioh->result = 0;

	ioh->state = PGAIO_HS_DEFINED;

	pgaio_my_backend->handed_out_io = NULL;

	pgaio_io_call_stage(ioh);

	ioh->state = PGAIO_HS_STAGED;

	pgaio_io_prepare_submit(ioh);

	START_CRIT_SECTION();

	ioh->result = nblocks;
	pgaio_io_process_completion(ioh, ioh->result);

	END_CRIT_SECTION();
}

static int
rados_smgr_fd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  uint32 *off)
{
	if (use_md(&reln->smgr_rlocator))
	{
		return mdfd(reln, forknum, blocknum, off);
	}

	return -1;
}

void
_PG_init(void)
{
	f_smgr		smgr = {
		.name = "rados",
		.smgr_init = rados_smgr_init,
		.smgr_shutdown = rados_smgr_shutdown,
		.smgr_open = rados_smgr_open,
		.smgr_close = rados_smgr_close,
		.smgr_create = rados_smgr_create,
		.smgr_exists = rados_smgr_exists,
		.smgr_unlink = rados_smgr_unlink,
		.smgr_extend = rados_smgr_extend,
		.smgr_zeroextend = rados_smgr_zeroextend,
		.smgr_prefetch = rados_smgr_prefetch,
		.smgr_maxcombine = rados_smgr_maxcombine,
		.smgr_readv = rados_smgr_readv,
		.smgr_startreadv = rados_smgr_startreadv,
		.smgr_writev = rados_smgr_writev,
		.smgr_writeback = rados_smgr_writeback,
		.smgr_nblocks = rados_smgr_nblocks,
		.smgr_truncate = rados_smgr_truncate,
		.smgr_immedsync = rados_smgr_immedsync,
		.smgr_registersync = rados_smgr_registersync,
		.smgr_fd = rados_smgr_fd,
	};

	storage_manager_id = smgr_register(&smgr, sizeof(radosRelationData));

	DefineCustomStringVariable("rados_smgr.id",
							   "Client ID for rados",
							   NULL,
							   &rados_id,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("rados_smgr.mon_host",
							   "Ceph monitors to try to connect to",
							   NULL,
							   &rados_mon_host,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("rados_smgr.keyring",
							   "Path to rados keyring",
							   NULL,
							   &rados_keyring,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("rados_smgr.pool",
							   "Name of the rados pool",
							   NULL,
							   &rados_pool,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);
}
