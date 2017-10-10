#include "RdbMerge.h"
#include "Rdb.h"
#include "Process.h"
#include "Spider.h" //dedupSpiderdbList()
#include "MergeSpaceCoordinator.h"
#include "Conf.h"


RdbMerge g_merge;


RdbMerge::RdbMerge()
  : m_mergeSpaceCoordinator(NULL),
	m_isAcquireLockJobSubmited(false),
	m_isLockAquired(false),
    m_doneMerging(false),
    m_getListOutstanding(false),
    m_startFileNum(0),
    m_numFiles(0),
    m_fixedDataSize(0),
    m_targetFile(NULL),
    m_targetMap(NULL),
    m_targetIndex(NULL),
	m_doneRegenerateFiles(false),
    m_isMerging(false),
    m_isHalted(false),
    m_dump(),
    m_msg5(),
    m_list(),
    m_niceness(0),
    m_rdbId(RDB_NONE),
    m_collnum(0),
    m_ks(0)
{
	memset(m_startKey, 0, sizeof(m_startKey));
}

RdbMerge::~RdbMerge() {
	delete m_mergeSpaceCoordinator;
}



// . return false if blocked, true otherwise
// . sets g_errno on error
// . if niceness is 0 merge will block, otherwise will not block
// . we now use niceness of 1 which should spawn threads that don't allow
//   niceness 2 threads to launch while they're running
// . spider process now uses mostly niceness 2 
// . we need the merge to take priority over spider processes on disk otherwise
//   there's too much contention from spider lookups on disk for the merge
//   to finish in a decent amount of time and we end up getting too many files!
bool RdbMerge::merge(rdbid_t rdbId,
                     collnum_t collnum,
                     BigFile *targetFile,
                     RdbMap *targetMap,
                     RdbIndex *targetIndex,
                     int32_t startFileNum,
                     int32_t numFiles,
                     int32_t niceness)
{
	if(m_isHalted) {
		logTrace(g_conf.m_logTraceRdbMerge, "END, merging is halted");
		return true;
	}
	if(m_isMerging) {
		logTrace(g_conf.m_logTraceRdbMerge, "END, already merging");
		return true;
	}

	if(!m_mergeSpaceCoordinator) {
		const char *mergeSpaceDir = strlen(g_hostdb.m_myHost->m_mergeDir) > 0 ? g_hostdb.m_myHost->m_mergeDir : g_conf.m_mergespaceDirectory;
		const char *mergeSpaceLockDir = strlen(g_hostdb.m_myHost->m_mergeLockDir) > 0 ? g_hostdb.m_myHost->m_mergeLockDir : g_conf.m_mergespaceLockDirectory;

		m_mergeSpaceCoordinator = new MergeSpaceCoordinator(mergeSpaceLockDir, g_conf.m_mergespaceMinLockFiles, mergeSpaceDir);
	}

	// get base, returns NULL and sets g_errno to ENOCOLLREC on error
	RdbBase *base = getRdbBase(rdbId, collnum);
	if (!base) {
		return true;
	}

	Rdb *rdb = getRdbFromId(rdbId);

	m_rdbId           = rdbId;
	m_collnum         = collnum;
	m_targetFile      = targetFile;
	m_targetMap       = targetMap;
	m_targetIndex     = targetIndex;
	m_startFileNum    = startFileNum;
	m_numFiles        = numFiles;
	m_fixedDataSize   = base->getFixedDataSize();
	m_niceness        = niceness;
	m_doneRegenerateFiles = false;
	m_doneMerging     = false;
	m_ks              = rdb->getKeySize();

	// . set the key range we want to retrieve from the files
	// . just get from the files, not tree (not cache?)
	KEYMIN(m_startKey,m_ks);

	//calculate how much space we need for resulting merged file
	m_spaceNeededForMerge = base->getSpaceNeededForMerge(m_startFileNum,m_numFiles);


	if(!g_loop.registerSleepCallback(5000, this, getLockWrapper, "RdbMerge::getLockWrapper", 0, true))
		return true;

	// we're now merging since we accepted to try
	m_isMerging = true;

	return false;
}

void RdbMerge::acquireLockWrapper(void *state) {
	RdbMerge *that = static_cast<RdbMerge*>(state);

	if(that->m_mergeSpaceCoordinator->acquire(that->m_spaceNeededForMerge)) {
		log(LOG_INFO,"Rdbmerge(%p)::getLock(), m_rdbId=%d: got lock for %" PRIu64 " bytes",
		    that, (int)that->m_rdbId, that->m_spaceNeededForMerge);
		g_loop.unregisterSleepCallback(that, getLockWrapper);
		that->m_isLockAquired = true;
	} else {
		log(LOG_INFO, "Rdbmerge(%p)::getLock(), m_rdbId=%d: Didn't get lock for %" PRIu64 " bytes; retrying in a bit...",
		    that, (int)that->m_rdbId, that->m_spaceNeededForMerge);
	}
}

void RdbMerge::acquireLockDoneWrapper(void *state, job_exit_t exit_type) {
	RdbMerge *that = static_cast<RdbMerge*>(state);

	that->m_isAcquireLockJobSubmited = false;

	if (exit_type != job_exit_normal) {
		return;
	}

	if (that->m_isLockAquired) {
		that->gotLock();
	}
}

void RdbMerge::getLockWrapper(int /*fd*/, void *state) {
	logTrace(g_conf.m_logTraceRdbMerge, "RdbMerge::getLockWrapper(%p)", state);
	RdbMerge *that = static_cast<RdbMerge*>(state);
	that->getLock();
}

void RdbMerge::getLock() {
	logDebug(g_conf.m_logDebugMerge, "Rdbmerge(%p)::getLock(), m_rdbId=%d",this,(int)m_rdbId);
	bool isAcquireLockJobSubmited = m_isAcquireLockJobSubmited.exchange(true);
	if (isAcquireLockJobSubmited) {
		return;
	}

	if (g_jobScheduler.submit(acquireLockWrapper, acquireLockDoneWrapper, this, thread_type_file_merge, 0)) {
		return;
	}

	log(LOG_WARN, "db: merge: Unable to submit acquire lock job. Running on main thread!");
	m_isAcquireLockJobSubmited = false;
	acquireLockWrapper(this);
	acquireLockDoneWrapper(this, job_exit_normal);
}

void RdbMerge::gotLockWrapper(int /*fd*/, void *state) {
	logTrace(g_conf.m_logTraceRdbMerge, "RdbMerge::gotLockWrapper(%p)", state);
	RdbMerge *that = static_cast<RdbMerge*>(state);
	g_loop.unregisterSleepCallback(state, gotLockWrapper);

	that->gotLock();
}

void RdbMerge::regenerateFilesWrapper(void *state) {
	RdbMerge *that = static_cast<RdbMerge*>(state);
	if (that->m_targetMap->getFileSize() == 0) {
		log( LOG_INFO, "db: merge: Attempting to generate map file for data file %s* of %" PRId64" bytes. May take a while.",
		     that->m_targetFile->getFilename(), that->m_targetFile->getFileSize() );

		// this returns false and sets g_errno on error
		if (!that->m_targetMap->generateMap(that->m_targetFile)) {
			log(LOG_ERROR, "db: merge: Map generation failed.");
			gbshutdownCorrupted();
		}

		log(LOG_INFO, "db: merge: Map generation succeeded.");
	}

	if (that->m_targetIndex && that->m_targetIndex->getFileSize() == 0) {
		log(LOG_INFO, "db: merge: Attempting to generate index file for data file %s* of %" PRId64" bytes. May take a while.",
		    that->m_targetFile->getFilename(), that->m_targetFile->getFileSize() );

		// this returns false and sets g_errno on error
		if (!that->m_targetIndex->generateIndex(that->m_targetFile)) {
			logError("db: merge: Index generation failed for %s.", that->m_targetFile->getFilename());
			gbshutdownCorrupted();
		}

		log(LOG_INFO, "db: merge: Index generation succeeded.");
	}
}

void RdbMerge::regenerateFilesDoneWrapper(void *state, job_exit_t exit_type) {
	RdbMerge *that = static_cast<RdbMerge*>(state);
	that->m_doneRegenerateFiles = true;
	that->gotLock();
}

// . returns false if blocked, true otherwise
// . sets g_errno on error
bool RdbMerge::gotLock() {
	// regenerate map/index if needed
	if (!m_doneRegenerateFiles &&
		m_targetFile->getFileSize() > 0 &&
		((m_targetIndex && m_targetIndex->getFileSize() == 0) || m_targetMap->getFileSize() == 0)) {
		log(LOG_WARN, "db: merge: Regenerating map/index from a killed merge.");

		if (g_jobScheduler.submit(regenerateFilesWrapper, regenerateFilesDoneWrapper, this, thread_type_file_merge, 0)) {
			return true;
		}

		log(LOG_WARN, "db: merge: Unable to submit regenerate files job. Running on main thread!");
		regenerateFilesWrapper(this);
	}

	// if we're resuming a killed merge, set m_startKey to last
	// key the map knows about.
	// the dump will start dumping at the end of the targetMap's data file.
	if (m_targetMap->getNumRecs() > 0) {
		log(LOG_INIT,"db: Resuming a killed merge.");
		m_targetMap->getLastKey(m_startKey);
		KEYINC(m_startKey,m_ks);
	}

	// . get last mapped offset
	// . this may actually be smaller than the file's actual size
	//   but the excess is not in the map, so we need to do it again
	int64_t startOffset = m_targetMap->getFileSize();

	// if startOffset is > 0 use the last key as RdbDump:m_prevLastKey
	// so it can compress the next key it dumps providee m_useHalfKeys
	// is true (key compression) and the next key has the same top 6 bytes
	// as m_prevLastKey
	char prevLastKey[MAX_KEY_BYTES];
	if (startOffset > 0) {
		m_targetMap->getLastKey(prevLastKey);
	} else {
		KEYMIN(prevLastKey, m_ks);
	}

	// get base, returns NULL and sets g_errno to ENOCOLLREC on error
	RdbBase *base = getRdbBase(m_rdbId, m_collnum);
	if (!base) {
		relinquishMergespaceLock();
		m_isMerging = false;
		//no need for calling incorporateMerge() because the base/collection is cone
		return true;
	}

	/// @note ALC currently this working because only posdb is using index
	/// if we ever enable more rdb to use indexes, we could potentially have a lot more
	/// global index jobs. means this mechanism will have to be changed.
	if (base->hasPendingGlobalIndexJob()) {
		// wait until no more pending global index job
		g_loop.registerSleepCallback(1000, this, gotLockWrapper, "RdbMerge::gotLockWrapper");
		log(LOG_INFO, "db: merge: Waiting for global index job to complete");
		return true;
	}

	// . set up a a file to dump the records into
	// . returns false and sets g_errno on error
	// . this will open m_target as O_RDWR ...

	m_dump.set(m_collnum,
	           m_targetFile,
	           NULL, // buckets to dump is NULL, we call dumpList
	           NULL, // tree to dump is NULL, we call dumpList
	           m_targetMap,
	           m_targetIndex,
	           0, // m_maxBufSize. not needed if no tree!
	           m_niceness, // niceness of dump
	           this, // state
	           dumpListWrapper,
	           base->useHalfKeys(),
	           startOffset,
	           prevLastKey,
	           m_ks,
	           m_rdbId);
	// what kind of error?
	if ( g_errno ) {
		log(LOG_WARN, "db: gotLock: merge.set: %s.", mstrerror(g_errno));
		relinquishMergespaceLock();
		m_isMerging = false;
		base->incorporateMerge();
		return true;
	}

	// . this returns false on error and sets g_errno
	// . it returns true if blocked or merge completed successfully
	return resumeMerge ( );
}

void RdbMerge::haltMerge() {
	if(m_isHalted) {
		return;
	}

	m_isHalted = true;

	// . we don't want the dump writing to an RdbMap that has been deleted
	// . this can happen if the close is delayed because we are dumping
	//   a tree to disk
	m_dump.setSuspended();
}

void RdbMerge::doSleep() {
	log(LOG_WARN, "db: Merge had error: %s. Sleeping and retrying.", mstrerror(g_errno));
	g_errno = 0;
	g_loop.registerSleepCallback(1000, this, tryAgainWrapper, "RdbMerge::tryAgainWrapper");
}

// . return false if blocked, otherwise true
// . sets g_errno on error
bool RdbMerge::resumeMerge() {
	if(m_isHalted) {
		return true;
	}

	// the usual loop
	for (;;) {
		// . this returns false if blocked, true otherwise
		// . sets g_errno on error
		// . we return true if it blocked
		if (!getNextList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. getNextList blocked. list=%p", &m_list);
			return false;
		}

		// if g_errno is out of memory then msg3 wasn't able to get the lists
		// so we should sleep and retry...
		if (g_errno == ENOMEM) {
			doSleep();
			logTrace(g_conf.m_logTraceRdbMerge, "END. out of memory. list=%p", &m_list);
			return false;
		}

		// if list is empty or we had an error then we're done
		if (g_errno || m_doneMerging) {
			doneMerging();
			logTrace(g_conf.m_logTraceRdbMerge, "END. error/done merging. list=%p", &m_list);
			return true;
		}

		// return if this blocked
		if (!filterList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. filterList blocked. list=%p", &m_list);
			return false;
		}

		// . otherwise dump the list we read to our target file
		// . this returns false if blocked, true otherwise
		if (!dumpList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. dumpList blocked. list=%p", &m_list);
			return false;
		}
	}
}

// . return false if blocked, true otherwise
// . sets g_errno on error
bool RdbMerge::getNextList() {
	// return true if g_errno is set
	if (g_errno || m_doneMerging) {
		return true;
	}

	// it's suspended so we count this as blocking
	if(m_isHalted) {
		return false;
	}

	// get base, returns NULL and sets g_errno to ENOCOLLREC on error
	RdbBase *base = getRdbBase(m_rdbId, m_collnum);
	if (!base) {
		// hmmm it doesn't set g_errno so we set it here now
		// otherwise we do an infinite loop sometimes if a collection
		// rec is deleted for the collnum
		g_errno = ENOCOLLREC;
		return true;
	}

	// otherwise, get it now
	return getAnotherList();
}

bool RdbMerge::getAnotherList() {
	logDebug(g_conf.m_logDebugMerge, "db: Getting another list for merge.");

	// clear it up in case it was already set
	g_errno = 0;

	// get base, returns NULL and sets g_errno to ENOCOLLREC on error
	RdbBase *base = getRdbBase(m_rdbId, m_collnum);
	if (!base) {
		return true;
	}

	logTrace(g_conf.m_logTraceRdbMerge, "list=%p startKey=%s",
	         &m_list, KEYSTR(m_startKey, m_ks));

	// . this returns false if blocked, true otherwise
	// . sets g_errno on error
	// . we return false if it blocked
	// . m_maxBufSize may be exceeded by a rec, it's just a target size
	// . niceness is usually MAX_NICENESS, but reindex.cpp sets to 0
	// . this was a call to Msg3, but i made it call Msg5 since
	//   we now do the merging in Msg5, not in msg3 anymore
	// . this will now handle truncation, dup and neg rec removal
	// . it remembers last termId and count so it can truncate even when
	//   IndexList is split between successive reads
	// . IMPORTANT: when merging titledb we could be merging about 255
	//   files, so if we are limited to only X fds it can have a cascade
	//   affect where reading from one file closes the fd of another file
	//   in the read (since we call open before spawning the read thread)
	//   and can therefore take 255 retries for the Msg3 to complete 
	//   because each read gives a EFILCLOSED error.
	//   so to fix it we allow one retry for each file in the read plus
	//   the original retry of 25
	int32_t nn = base->getNumFiles();
	if ( m_numFiles > 0 && m_numFiles < nn ) nn = m_numFiles;

	int32_t bufSize = g_conf.m_mergeBufSize;
	// get it
	m_getListOutstanding = true;
	bool rc = m_msg5.getList(m_rdbId,
				 m_collnum,
				 &m_list,
				 m_startKey,
				 KEYMAX(),        // usually is maxed!
				 bufSize,
				 false,           // includeTree?
				 m_startFileNum,  // startFileNum
				 m_numFiles,
				 this,            // state
				 gotListWrapper,  // callback
				 m_niceness,      // niceness
				 true,            // do error correction?
				 nn + 75,         // max retries (mk it high)
				 true);            // isRealMerge? absolutely!
	if(rc)
		m_getListOutstanding = false;
	return rc;
	
}

void RdbMerge::gotListWrapper(void *state, RdbList * /*list*/, Msg5 * /*msg5*/) {
	// get a ptr to ourselves
	RdbMerge *THIS = (RdbMerge *)state;

	logTrace(g_conf.m_logTraceRdbMerge, "list=%p startKey=%s",
	         &(THIS->m_list), KEYSTR(THIS->m_startKey, THIS->m_ks));

	THIS->m_getListOutstanding = false;

	for (;;) {
		// if g_errno is out of memory then msg3 wasn't able to get the lists
		// so we should sleep and retry
		if (g_errno == ENOMEM) {
			THIS->doSleep();
			logTrace(g_conf.m_logTraceRdbMerge, "END. out of memory. list=%p", &THIS->m_list);
			return;
		}

		// if g_errno we're done
		if (g_errno || THIS->m_doneMerging) {
			THIS->doneMerging();
			logTrace(g_conf.m_logTraceRdbMerge, "END. error/done merging. list=%p", &THIS->m_list);
			return;
		}

		// return if this blocked
		if (!THIS->filterList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. filterList blocked. list=%p", &THIS->m_list);
			return;
		}

		// return if this blocked
		if (!THIS->dumpList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. dumpList blocked. list=%p", &THIS->m_list);
			return;
		}

		// return if this blocked
		if (!THIS->getNextList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. getNextList blocked. list=%p", &THIS->m_list);
			return;
		}

		// otherwise, keep on trucking
	}
}

// called after sleeping for 1 sec because of ENOMEM
void RdbMerge::tryAgainWrapper(int /*fd*/, void *state) {
	// get a ptr to ourselves
	RdbMerge *THIS = (RdbMerge *)state;

	// unregister the sleep callback
	g_loop.unregisterSleepCallback(THIS, tryAgainWrapper);

	// clear this
	g_errno = 0;

	// return if this blocked
	if (!THIS->getNextList()) {
		logTrace(g_conf.m_logTraceRdbMerge, "END. getNextList blocked. list=%p", &THIS->m_list);
		return;
	}

	// if this didn't block do the loop
	gotListWrapper(THIS, NULL, NULL);
}

void RdbMerge::filterListWrapper(void *state) {
	RdbMerge *THIS = (RdbMerge *)state;

	logTrace(g_conf.m_logTraceRdbMerge, "BEGIN. list=%p m_startKey=%s", &THIS->m_list, KEYSTR(THIS->m_startKey, THIS->m_ks));

	if (THIS->m_rdbId == RDB_SPIDERDB_DEPRECATED) {
		dedupSpiderdbList(&(THIS->m_list));
	} else if (THIS->m_rdbId == RDB_TITLEDB) {
//		filterTitledbList(&(THIS->m_list));
	}

	logTrace(g_conf.m_logTraceRdbMerge, "END. list=%p", &THIS->m_list);
}

// similar to gotListWrapper but we call dumpList() before dedupList()
void RdbMerge::filterDoneWrapper(void *state, job_exit_t exit_type) {
	// get a ptr to ourselves
	RdbMerge *THIS = (RdbMerge *)state;

	logTrace(g_conf.m_logTraceRdbMerge, "BEGIN. list=%p m_startKey=%s", &THIS->m_list, KEYSTR(THIS->m_startKey, THIS->m_ks));

	for (;;) {
		// return if this blocked
		if (!THIS->dumpList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. dumpList blocked. list=%p", &THIS->m_list);
			return;
		}

		// return if this blocked
		if (!THIS->getNextList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. getNextList blocked. list=%p", &THIS->m_list);
			return;
		}

		// if g_errno is out of memory then msg3 wasn't able to get the lists
		// so we should sleep and retry
		if (g_errno == ENOMEM) {
			THIS->doSleep();
			logTrace(g_conf.m_logTraceRdbMerge, "END. out of memory. list=%p", &THIS->m_list);
			return;
		}

		// if g_errno we're done
		if (g_errno || THIS->m_doneMerging) {
			THIS->doneMerging();
			logTrace(g_conf.m_logTraceRdbMerge, "END. error/done merging. list=%p", &THIS->m_list);
			return;
		}

		// return if this blocked
		if (!THIS->filterList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. filterList blocked. list=%p", &THIS->m_list);
			return;
		}

		// otherwise, keep on trucking
	}
}

bool RdbMerge::filterList() {
	// return true on g_errno
	if (g_errno) {
		return true;
	}

	// . it's suspended so we count this as blocking
	// . resumeMerge() will call getNextList() again, not dumpList() so
	//   don't advance m_startKey
	if(m_isHalted) {
		return false;
	}

	// if we use getLastKey() for this the merge completes but then
	// tries to merge two empty lists and cores in the merge function
	// because of that. i guess it relies on endkey rollover only and
	// not on reading less than minRecSizes to determine when to stop
	// doing the merge.
	m_list.getEndKey(m_startKey) ;
	KEYINC(m_startKey,m_ks);

	logTrace(g_conf.m_logTraceRdbMerge, "listEndKey=%s startKey=%s",
	         KEYSTR(m_list.getEndKey(), m_list.getKeySize()), KEYSTR(m_startKey, m_ks));

	/////
	//
	// dedup for spiderdb before we dump it. try to save disk space.
	//
	/////
	if (m_rdbId == RDB_SPIDERDB_DEPRECATED || m_rdbId == RDB_TITLEDB) {
		if (g_jobScheduler.submit(filterListWrapper, filterDoneWrapper, this, thread_type_merge_filter, 0)) {
			return false;
		}

		log(LOG_WARN, "db: Unable to submit job for merge filter. Will run in main thread");

		// fall back to filter without thread
		if (m_rdbId == RDB_SPIDERDB_DEPRECATED) {
			dedupSpiderdbList(&m_list);
		} else {
//			filterTitledbList(&m_list);
		}
	}

	return true;
}

// similar to gotListWrapper but we call getNextList() before dumpList()
void RdbMerge::dumpListWrapper(void *state) {
	// debug msg
	logDebug(g_conf.m_logDebugMerge, "db: Dump of list completed: %s.",mstrerror(g_errno));

	// get a ptr to ourselves
	RdbMerge *THIS = (RdbMerge *)state;

	logTrace(g_conf.m_logTraceRdbMerge, "list=%p startKey=%s",
	         &(THIS->m_list), KEYSTR(THIS->m_startKey, THIS->m_ks));

	for (;;) {
		// collection reset or deleted while RdbDump.cpp was writing out?
		if (g_errno == ENOCOLLREC) {
			THIS->doneMerging();
			logTrace(g_conf.m_logTraceRdbMerge, "END. error/done merging. list=%p", &THIS->m_list);
			return;
		}
		// return if this blocked
		if (!THIS->getNextList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. getNextList blocked. list=%p", &THIS->m_list);
			return;
		}

		// if g_errno is out of memory then msg3 wasn't able to get the lists
		// so we should sleep and retry
		if (g_errno == ENOMEM) {
			// if the dump failed, it should reset m_dump.m_offset of
			// the file to what it was originally (in case it failed
			// in adding the list to the map). we do not need to set
			// m_startKey back to the startkey of this list, because
			// it is *now* only advanced on successful dump!!
			THIS->doSleep();
			logTrace(g_conf.m_logTraceRdbMerge, "END. out of memory. list=%p", &THIS->m_list);
			return;
		}

		// . if g_errno we're done
		// . if list is empty we're done
		if (g_errno || THIS->m_doneMerging) {
			THIS->doneMerging();
			logTrace(g_conf.m_logTraceRdbMerge, "END. error/done merging. list=%p", &THIS->m_list);
			return;
		}

		// return if this blocked
		if (!THIS->filterList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. filterList blocked. list=%p", &THIS->m_list);
			return;
		}

		// return if this blocked
		if (!THIS->dumpList()) {
			logTrace(g_conf.m_logTraceRdbMerge, "END. dumpList blocked. list=%p", &THIS->m_list);
			return;
		}

		// otherwise, keep on trucking
	}
}

// . return false if blocked, true otherwise
// . set g_errno on error
// . list should be truncated, possible have all negative keys removed,
//   and de-duped thanks to RdbList::indexMerge_r() and RdbList::merge_r()
bool RdbMerge::dumpList() {
	// if the startKey rolled over we're done
	if (KEYCMP(m_startKey, KEYMIN(), m_ks) == 0) {
		m_doneMerging = true;
	}

	logDebug(g_conf.m_logDebugMerge, "db: Dumping list.");

	logTrace(g_conf.m_logTraceRdbMerge, "list=%p startKey=%s",
	         &m_list, KEYSTR(m_startKey, m_ks));

	// . send the whole list to the dump
	// . it returns false if blocked, true otherwise
	// . it sets g_errno on error
	// . it calls dumpListWrapper when done dumping
	// . return true if m_dump had an error or it did not block
	// . if it gets a EFILECLOSED error it will keep retrying forever
	return m_dump.dumpList(&m_list);
}

void RdbMerge::doneMerging() {
	// save this
	int32_t saved_errno = g_errno;

	// let RdbDump free its m_verifyBuf buffer if it existed
	m_dump.reset();

	// . free the list's memory, reset() doesn't do it
	// . when merging titledb i'm still seeing 200MB allocs to read from tfndb.
	m_list.freeList();

	log(LOG_INFO,"db: Merge status: %s.",mstrerror(g_errno));

	// . reset our class
	// . this will free it's cutoff keys buffer, trash buffer, treelist
	// . TODO: should we not reset to keep the mem handy for next time
	//   to help avoid out of mem errors?
	m_msg5.reset();

	// if collection rec was deleted while merging files for it
	// then the rdbbase should be NULL i guess.
	if (saved_errno == ENOCOLLREC) {
		return;
	}

	// if we are exiting then dont bother renaming the files around now.
	// this prevents a core in RdbBase::incorporateMerge()
	if (g_process.isShuttingDown()) {
		log(LOG_INFO, "merge: exiting. not ending merge.");
		return;
	}

	// get base, returns NULL and sets g_errno to ENOCOLLREC on error
	RdbBase *base = getRdbBase(m_rdbId, m_collnum);
	if (!base) {
		return;
	}

	// pass g_errno on to incorporate merge so merged file can be unlinked
	base->incorporateMerge();
}


void RdbMerge::relinquishMergespaceLock() {
	if(m_mergeSpaceCoordinator) {
		m_mergeSpaceCoordinator->relinquish();
		m_isLockAquired = false;
	}
}


void RdbMerge::mergeIncorporated(const RdbBase * /*currently irrelevant*/) {
	relinquishMergespaceLock();
	m_isMerging = false;
}
