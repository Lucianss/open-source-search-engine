#include "SpiderColl.h"
#include "Spider.h"
#include "SpiderLoop.h"
#include "Doledb.h"
#include "Collectiondb.h"
#include "UdpServer.h"
#include "Stats.h"
#include "SafeBuf.h"
#include "Repair.h" //g_repairMode
#include "Process.h"
#include "JobScheduler.h"
#include "XmlDoc.h"
#include "ip.h"
#include "Conf.h"
#include "Mem.h"
#include "SpiderdbRdbSqliteBridge.h"
#include "SpiderdbUtil.h"
#include "UrlBlockCheck.h"
#include "ScopedLock.h"
#include "Sanity.h"


#define OVERFLOWLISTSIZE 200

// How large chunks of spiderdb to load in for each read
#define SR_READ_SIZE (512*1024)
//Hint for size allocation to m_winnerTree
#define MAX_REQUEST_SIZE	(sizeof(SpiderRequest)+MAX_URL_LEN+1)
#define MAX_SP_REPLY_SIZE	sizeof(SpiderReply)



static key96_t makeWaitingTreeKey ( uint64_t spiderTimeMS , int32_t firstIp ) {
	// sanity
	if ( ((int64_t)spiderTimeMS) < 0 ) gbshutdownAbort(true);
	// make the wait tree key
	key96_t wk;
	wk.n1 = (spiderTimeMS>>32);
	wk.n0 = (spiderTimeMS&0xffffffff);
	wk.n0 <<= 32;
	wk.n0 |= (uint32_t)firstIp;
	// sanity
	if ( wk.n1 & 0x8000000000000000LL ) gbshutdownAbort(true);
	return wk;
}

/////////////////////////
/////////////////////////      SpiderColl
/////////////////////////

void SpiderColl::setCollectionRec ( CollectionRec *cr ) {
	m_cr = cr;
}

CollectionRec *SpiderColl::getCollectionRec() {
	return m_cr;
}
const CollectionRec *SpiderColl::getCollectionRec() const {
	return m_cr;
}

SpiderColl::SpiderColl(CollectionRec *cr) {
	m_overflowList = NULL;
	m_lastOverflowFirstIp = 0;
	m_deleteMyself = false;
	m_isLoading = false;
	m_gettingWaitingTreeList = false;
	m_lastScanTime = 0;
	m_isPopulatingDoledb = false;
	m_numAdded = 0;
	m_numBytesScanned = 0;
	m_lastPrintCount = 0;
	m_siteListIsEmptyValid = false;
	m_cr = NULL;
	// re-set this to min and set m_needsWaitingTreeRebuild to true
	// when the admin updates the url filters page
	m_waitingTreeNeedsRebuild = false;
	m_waitingTreeNextKey.setMin();
	m_spidersOut = 0;
	m_coll[0] = '\0';

	// PVS-Studio
	m_lastReplyValid = false;
	memset(m_lastReplyBuf, 0, sizeof(m_lastReplyBuf));
	m_didRead = false;
	m_siteListIsEmpty = false;
	m_tailIp = 0;
	m_tailPriority = 0;
	m_tailTimeMS = 0;
	m_tailUh48 = 0;
	m_minFutureTimeMS = 0;
	m_gettingWaitingTreeList = false;
	m_lastScanTime = 0;
	m_waitingTreeNeedsRebuild = false;
	m_numAdded = 0;
	m_numBytesScanned = 0;
	m_collnum = -1;
	m_lastReindexTimeMS = 0;
	m_countingPagesIndexed = false;
	m_lastReqUh48a = 0;
	m_lastReqUh48b = 0;
	m_lastRepUh48 = 0;
	m_waitingTreeKeyValid = false;
	m_scanningIp = 0;
	m_gotNewDataForScanningIp = 0;
	m_lastListSize = 0;
	m_lastScanningIp = 0;
	m_totalBytesScanned = 0;
	m_deleteMyself = false;
	m_pri2 = 0;
	memset(m_outstandingSpiders, 0, sizeof(m_outstandingSpiders));
	m_overflowList = NULL;
	m_totalNewSpiderRequests = 0;
	m_lastSreqUh48 = 0;
	memset(m_cblocks, 0, sizeof(m_cblocks));
	m_pageNumInlinks = 0;
	m_lastCBlockIp = 0;
	m_lastOverflowFirstIp = 0;

	reset();

	// reset this
	memset ( m_outstandingSpiders , 0 , 4 * MAX_SPIDER_PRIORITIES );

	m_collnum = cr->m_collnum;
	strcpy(m_coll, cr->m_coll);
	m_cr = cr;

	// set first doledb scan key
	m_nextDoledbKey.setMin();

	// mark it as loading so it can't be deleted while loading
	m_isLoading = true;

	// . load its tables from disk
	// . crap i think this might call quickpoll and we get a parm
	//   update to delete this spider coll!
	load();

	m_isLoading = false;
}

// load the tables that we set when m_doInitialScan is true
bool SpiderColl::load ( ) {

	// error?
	int32_t err = 0;
	// make the dir
	const char *coll = g_collectiondb.getCollName(m_collnum);
	// sanity check
	if ( ! coll || coll[0]=='\0' ) {
		log(LOG_ERROR,"spider: bad collnum of %" PRId32,(int32_t)m_collnum);
		g_errno = ENOCOLLREC;
		return false;
		//gbshutdownAbort(true);
	}

	// reset this once
	m_isPopulatingDoledb = false;

	// keep it kinda low if we got a ton of collections
	int32_t maxMem = 15000;
	int32_t maxNodes = 500;
	if ( g_collectiondb.getNumRecsUsed() > 500 ) {
		maxNodes = 100;
		maxMem = maxNodes * 20;
	}

	if ( ! m_lastDownloadCache.init ( maxMem     , // maxcachemem,
					  8          , // fixed data size (MS)
					  maxNodes   , // max nodes
					  "downcache", // dbname
					  false      , // load from disk?
					  12         , // key size (firstip)
					  -1         )) {// numPtrsMax
		log(LOG_WARN, "spider: dcache init failed");
		return false;
	}

	// this has a quickpoll in it, so that quickpoll processes
	// a restart request from crawlbottesting for this collnum which
	// calls Collectiondb::resetColl2() which calls deleteSpiderColl()
	// on THIS spidercoll, but our m_loading flag is set
	if (!m_sniTable.set   ( 4,8,0,NULL,0,false,"snitbl") )
		return false;
	if (!m_cdTable.set    (4,4,0,NULL,0,false,"cdtbl"))
		return false;

	// doledb seems to have like 32000 entries in it
	int32_t numSlots = 0; // was 128000
	if(!m_doledbIpTable.set(4,4,numSlots,NULL,0,false,"doleip"))
		return false;

	// this should grow dynamically...
	if (!m_waitingTable.set (4,8,16,NULL,0,false,"waittbl"))
		return false;

	// . a tree of keys, key is earliestSpiderTime|ip (key=12 bytes)
	// . earliestSpiderTime is 0 if unknown
	// . max nodes is 1M but we should grow dynamically! TODO
	// . let's up this to 5M because we are hitting the limit in some
	//   test runs...
	// . try going to 20M now since we hit it again...
	// . start off at just 10 nodes since we grow dynamically now
	if (!m_waitingTree.set(0, 10, -1, true, "waittree2", "waitingtree", sizeof(key96_t))) {
		return false;
	}
	m_waitingTreeKeyValid = false;
	m_scanningIp = 0;

	// make dir
	char dir[500];
	sprintf(dir,"%scoll.%s.%" PRId32,g_hostdb.m_dir,coll,(int32_t)m_collnum);

	// load in the waiting tree, IPs waiting to get into doledb
	BigFile file;
	file.set ( dir , "waitingtree-saved.dat");
	bool treeExists = file.doesExist();

	// load the table with file named "THISDIR/saved"
	if ( treeExists && !m_waitingTree.fastLoad(&file, &m_waitingMem) )
		err = g_errno;

	// init wait table. scan wait tree and add the ips into table.
	if ( ! makeWaitingTable() ) err = g_errno;
	// save it
	g_errno = err;
	// return false on error
	if ( g_errno ) {
		// note it
		log(LOG_WARN,"spider: had error loading initial table: %s", mstrerror(g_errno));
		return false;
	}

	// . do this now just to keep everything somewhat in sync
	// . we lost dmoz.org and could not get it back in because it was
	//   in the doleip table but NOT in doledb!!!
	return makeDoledbIPTable();
}

// . scan all spiderRequests in doledb at startup and add them to our tables
// . then, when we scan spiderdb and add to orderTree/urlhashtable it will
//   see that the request is in doledb and set m_doled...
// . initialize the dole table for that then
//   quickly scan doledb and add the doledb records to our trees and
//   tables. that way if we receive a SpiderReply() then addSpiderReply()
//   will be able to find the associated SpiderRequest.
//   MAKE SURE to put each spiderrequest into m_doleTable... and into
//   maybe m_urlHashTable too???
//   this should block since we are at startup...
bool SpiderColl::makeDoledbIPTable() {
	log(LOG_DEBUG,"spider: making dole ip table for %s",m_coll);

	key96_t startKey ; startKey.setMin();
	key96_t endKey   ; endKey.setMax();
	key96_t lastKey  ; lastKey.setMin();
	// get a meg at a time
	int32_t minRecSizes = 1024*1024;
	Msg5 msg5;
	RdbList list;

	for (;;) {
		// use msg5 to get the list, should ALWAYS block since no threads
		if (!msg5.getList(RDB_DOLEDB,
		                  m_collnum,
		                  &list,
		                  &startKey,
		                  &endKey,
		                  minRecSizes,
		                  true, // includeTree?
		                  0, // startFileNum
		                  -1, // numFiles
		                  NULL, // state
		                  NULL, // callback
		                  0, // niceness
		                  false, // err correction?
		                  -1, // maxRetries
		                  false)) { // isRealMerge
			log(LOG_LOGIC, "spider: getList did not block.");
			return false;
		}

		// shortcut
		int32_t minSize = (int32_t)(sizeof(SpiderRequest) + sizeof(key96_t) + 4 - MAX_URL_LEN);
		// all done if empty
		if (list.isEmpty()) {
			log(LOG_DEBUG,"spider: making dole ip table done.");
			return true;
		}

		// loop over entries in list
		for (list.resetListPtr(); !list.isExhausted(); list.skipCurrentRecord()) {
			// get rec
			const char *rec = list.getCurrentRec();
			// get key
			key96_t k = list.getCurrentKey();

			// skip deletes -- how did this happen?
			if ((k.n0 & 0x01) == 0) {
				continue;
			}

			// check this out
			int32_t recSize = list.getCurrentRecSize();
			// zero?
			if (recSize <= 0) gbshutdownCorrupted();

			// 16 is bad too... wtf is this?
			if (recSize <= 16) {
				continue;
			}

			// crazy?
			if (recSize <= minSize) gbshutdownAbort(true);
			// . doledb key is 12 bytes, followed by a 4 byte datasize
			// . so skip that key and dataSize to point to spider request
			const SpiderRequest *sreq = (const SpiderRequest *)(rec + sizeof(key96_t) + 4);
			// add to dole tables
			if (!addToDoledbIpTable(sreq)) {
				// return false with g_errno set on error
				return false;
			}
		}
		startKey = *(key96_t *)list.getLastKey();
		startKey++;

		// watch out for wrap around
		if (startKey < *(key96_t *)list.getLastKey()) {
			log(LOG_DEBUG,"spider: making dole ip table done.");
			return true;
		}
	}
}

CollectionRec *SpiderColl::getCollRec() {
	CollectionRec *cr = g_collectiondb.getRec(m_collnum);
	if ( ! cr ) log(LOG_WARN,"spider: lost coll rec");
	return cr;
}

const CollectionRec *SpiderColl::getCollRec() const {
	const CollectionRec *cr = g_collectiondb.getRec(m_collnum);
	if ( ! cr ) log(LOG_WARN,"spider: lost coll rec");
	return cr;
}

const char *SpiderColl::getCollName() const {
	const CollectionRec *cr = getCollRec();
	if ( ! cr ) return "lostcollection";
	return cr->m_coll;
}

bool SpiderColl::makeWaitingTable ( ) {
	ScopedLock sl(m_waitingTree.getLock());

	log(LOG_DEBUG,"spider: making waiting table for %s.",m_coll);

	for (int32_t node = m_waitingTree.getFirstNode_unlocked(); node >= 0;
	     node = m_waitingTree.getNextNode_unlocked(node)) {
		// get key
		const key96_t *key = reinterpret_cast<const key96_t*>(m_waitingTree.getKey_unlocked(node));
		// get ip from that
		int32_t ip = (key->n0) & 0xffffffff;
		// spider time is up top
		uint64_t spiderTimeMS = (key->n1);
		spiderTimeMS <<= 32;
		spiderTimeMS |= ((key->n0) >> 32);
		// store in waiting table
		if (!addToWaitingTable(ip, spiderTimeMS)) return false;
	}
	log(LOG_DEBUG,"spider: making waiting table done.");
	return true;
}


SpiderColl::~SpiderColl () {
	reset();
}

// we call this now instead of reset when Collectiondb::resetColl() is used
void SpiderColl::clearLocks ( ) {
	g_spiderLoop.clearLocks(m_collnum);
}

void SpiderColl::reset ( ) {
	// reset these for SpiderLoop;
	m_nextDoledbKey.setMin();

	// set this to -1 here, when we enter spiderDoledUrls() it will
	// see that its -1 and set the m_msg5StartKey
	m_pri2 = -1; // MAX_SPIDER_PRIORITIES - 1;

	m_isPopulatingDoledb = false;

	const char *coll = "unknown";
	if ( m_coll[0] ) coll = m_coll;
	log(LOG_DEBUG,"spider: resetting spider cache coll=%s",coll);

	m_doledbIpTable .reset();
	m_cdTable     .reset();
	m_sniTable    .reset();
	m_waitingTable.reset();
	m_waitingTree.reset();
	m_waitingMem  .reset();
	m_winnerTree.reset();
	m_winnerTable .reset();
	m_dupCache    .reset();

	if ( m_overflowList ) {
		mfree ( m_overflowList , OVERFLOWLISTSIZE * 4 ,"olist" );
		m_overflowList = NULL;
	}

	// each spider priority in the collection has essentially a cursor
	// that references the next spider rec in doledb to spider. it is
	// used as a performance hack to avoid the massive positive/negative
	// key annihilations related to starting at the top of the priority
	// queue every time we scan it, which causes us to do upwards of
	// 300 re-reads!
	for ( int32_t i = 0 ; i < MAX_SPIDER_PRIORITIES ; i++ ) {
		m_nextKeys[i] =	Doledb::makeFirstKey2 ( i );
	}

}

bool SpiderColl::updateSiteNumInlinksTable(int32_t siteHash32, int32_t sni, time_t timestamp) {
	// do not update if invalid
	if ( sni == -1 ) return true;

	ScopedLock sl(m_sniTableMtx);

	// . get entry for siteNumInlinks table
	// . use 32-bit key specialized lookup for speed
	uint64_t *val = (uint64_t *)m_sniTable.getValue32(siteHash32);
	// bail?
	if ( val && ((*val)&0xffffffff) > (uint32_t)timestamp ) return true;
	// . make new data for this key
	// . lower 32 bits is the addedTime
	// . upper 32 bits is the siteNumInlinks
	uint64_t nv = (uint32_t)sni;
	// shift up
	nv <<= 32;
	// or in time
	nv |= (uint32_t)timestamp;//sreq->m_addedTime;
	// just direct update if faster
	if  ( val ) *val = nv;
	// store it anew otherwise
	else if ( ! m_sniTable.addKey(&siteHash32,&nv) )
		// return false with g_errno set on error
		return false;
	// success
	return true;
}

// . we call this when we receive a spider reply in Rdb.cpp
// . returns false and sets g_errno on error
// . xmldoc.cpp adds reply AFTER the negative doledb rec since we decement
//   the count in m_doledbIpTable here
bool SpiderColl::addSpiderReply(const SpiderReply *srep) {

	////
	//
	// skip if not assigned to us for doling
	//
	////
	if ( ! isAssignedToUs ( srep->m_firstIp ) )
		return true;

	/////////
	//
	// remove the lock here
	//
	//////
	int64_t lockKey = makeLockTableKey ( srep );

	logDebug(g_conf.m_logDebugSpider, "spider: removing lock uh48=%" PRId64" lockKey=%" PRIu64, srep->getUrlHash48(), lockKey);

	/////
	//
	// but do note that its spider has returned for populating the
	// waiting tree. addToWaitingTree should not add an entry if
	// a spiderReply is still pending according to the lock table,
	// UNLESS, maxSpidersPerIP is more than what the lock table says
	// is currently being spidered.
	//
	/////

	// now just remove it since we only spider our own urls
	// and doledb is in memory
	g_spiderLoop.removeLock(lockKey);

	// update the latest siteNumInlinks count for this "site" (repeatbelow)
	updateSiteNumInlinksTable ( srep->m_siteHash32, srep->m_siteNumInlinks, srep->m_spideredTime );

	// clear error for this
	g_errno = 0;

	// no update if injecting or from pagereindex (docid based spider request)
	if (!srep->m_fromInjectionRequest) {
		ScopedLock sl(m_cdTableMtx);

		// use the domain hash for this guy! since its from robots.txt
		const int32_t *cdp = (const int32_t *)m_cdTable.getValue32(srep->m_domHash32);

		// update it only if better or empty
		if (!cdp) {
			// update m_sniTable if we should
			// . make new data for this key
			// . lower 32 bits is the spideredTime
			// . upper 32 bits is the crawldelay
			int32_t nv = (int32_t)(srep->m_crawlDelayMS);
			if (!m_cdTable.addKey(&srep->m_domHash32, &nv)) {
				char ipbuf[16];
				log(LOG_WARN, "spider: failed to add crawl delay for firstip=%s", iptoa(srep->m_firstIp,ipbuf));

				// just ignore
				g_errno = 0;
			}
		}
	}

	// . anytime we add a reply then
	//   we must update this downloadTable with the replies 
	//   SpiderReply::m_downloadEndTime so we can obey sameIpWait
	// . that is the earliest that this url can be respidered, but we
	//   also have a sameIpWait constraint we have to consider...
	// . we alone our responsible for adding doledb recs from this ip so
	//   this is easy to throttle...
	// . and make sure to only add to this download time hash table if
	//   SpiderReply::m_downloadEndTime is non-zero, because zero means
	//   no download happened. (TODO: check this)
	// . TODO: consult crawldelay table here too! use that value if is
	//   less than our sameIpWait
	// . make m_lastDownloadTable an rdbcache ...
	// . this is 0 for pagereindex docid-based replies
	if (srep->m_downloadEndTime) {
		RdbCacheLock rcl(m_lastDownloadCache);
		m_lastDownloadCache.addLongLong(m_collnum, srep->m_firstIp, srep->m_downloadEndTime);

		// ignore errors from that, it's just a cache
		g_errno = 0;
	}

	char ipbuf[16];
	logDebug(g_conf.m_logDebugSpider, "spider: adding spider reply, download end time %" PRId64" for "
		    "ip=%s(%" PRIu32") uh48=%" PRIu64" indexcode=\"%s\" coll=%" PRId32" "
		    "k.n1=%" PRIu64" k.n0=%" PRIu64,
		    //"to SpiderColl::m_lastDownloadCache",
		    srep->m_downloadEndTime,
		    iptoa(srep->m_firstIp,ipbuf),
		    (uint32_t)srep->m_firstIp,
		    srep->getUrlHash48(),
		    mstrerror(srep->m_errCode),
		    (int32_t)m_collnum,
		    srep->m_key.n1,
		    srep->m_key.n0);

	// . add to wait tree and let it populate doledb on its batch run
	// . use a spiderTime of 0 which means unknown and that it needs to
	//   scan spiderdb to get that
	// . returns false if did not add to waiting tree
	// . returns false sets g_errno on error
	addToWaitingTree(srep->m_firstIp);

	// ignore errors i guess
	g_errno = 0;

	return true;
}

bool SpiderColl::isInDupCache(const SpiderRequest *sreq, bool addToCache) {

	// init dup cache?
	if ( ! m_dupCache.isInitialized() )
		// use 50k i guess of 64bit numbers and linked list info
		m_dupCache.init ( 90000, 
				  4 , // fixeddatasize (don't really need this)
				  5000, // maxcachenodes
				  "urldups", // dbname
				  false, // loadfromdisk
				  12, // cachekeysize
				  -1 ); // numptrsmax

	// quit add dups over and over again...
	int64_t dupKey64 = sreq->getUrlHash48();
	int64_t org_dupKey64 = dupKey64;


	// . these flags make big difference in url filters
	// . NOTE: if you see a url that is not getting spidered that should be it might
	//   be because we are not incorporating other flags here...
	if ( sreq->m_fakeFirstIp ) dupKey64 ^= 12345;
	if ( sreq->m_isAddUrl    ) dupKey64 ^= 49387333;
	if ( sreq->m_isInjecting ) dupKey64 ^= 3276404;
	if ( sreq->m_isPageReindex) dupKey64 ^= 32999604;
	if ( sreq->m_forceDelete ) dupKey64 ^= 29386239;
	if ( sreq->m_hadReply    ) dupKey64 ^= 293294099;

	// . maxage=86400,promoteRec=yes. returns -1 if not in there
	RdbCacheLock rcl(m_dupCache);

	if (m_dupCache.getLong(0, dupKey64, 86400, true) != -1) {
		logDebug(g_conf.m_logDebugSpider, "spider: skipping dup request. url=%s uh48=%" PRIu64 ", dupkey=%" PRIu64 ", org_dupkey=%" PRIu64 ", %s%s%s%s%s%s", sreq->m_url, sreq->getUrlHash48(), dupKey64, org_dupKey64,
			sreq->m_fakeFirstIp?"fakeFirstIp ":"",sreq->m_isAddUrl?"isAddUrl ":"",sreq->m_isInjecting?"isInjecting ":"",sreq->m_isPageReindex?"isPageReindex ":"",sreq->m_forceDelete?"forceDelete ":"",sreq->m_hadReply?"hadReply":"");
		return true;
	}

	if (addToCache) {
		logDebug(g_conf.m_logDebugSpider, "spider: Adding to dup cache. url=%s uh48=%" PRIu64 ", dupkey=%" PRIu64 ", org_dupkey=%" PRIu64 ", %s%s%s%s%s%s", sreq->m_url, sreq->getUrlHash48(), dupKey64, org_dupKey64,
			sreq->m_fakeFirstIp?"fakeFirstIp ":"",sreq->m_isAddUrl?"isAddUrl ":"",sreq->m_isInjecting?"isInjecting ":"",sreq->m_isPageReindex?"isPageReindex ":"",sreq->m_forceDelete?"forceDelete ":"",sreq->m_hadReply?"hadReply":"");

		// add it
		m_dupCache.addLong(0, dupKey64, 1);
	}

	return false;
}


// . Rdb.cpp calls SpiderColl::addSpiderRequest/Reply() for every positive
//   spiderdb record it adds to spiderdb. that way our cache is kept 
//   uptodate incrementally
// . returns false and sets g_errno on error
// . if the spiderTime appears to be AFTER m_nextReloadTime then we should
//   not add this spider request to keep the cache trimmed!!! (MDW: TODO)
// . BUT! if we have 150,000 urls that is going to take a long time to
//   spider, so it should have a high reload rate!
bool SpiderColl::addSpiderRequest(const SpiderRequest *sreq) {
	int64_t nowGlobalMS = gettimeofdayInMilliseconds();
	// don't add negative keys or data less thangs
	if ( sreq->m_dataSize <= 0 ) {
		log( "spider: add spider request is dataless for uh48=%" PRIu64, sreq->getUrlHash48() );
		return true;
	}

	// . are we already more or less in spiderdb? true = addToCache
	// . put this above isAssignedToUs() so we *try* to keep twins in sync because
	//   Rdb.cpp won't add the spiderrequest if its in this dup cache, and we add
	//   it to the dupcache here...
	if ( isInDupCache ( sreq , true ) ) {
		logDebug( g_conf.m_logDebugSpider, "spider: skipping dup request url=%s uh48=%" PRIu64,
		          sreq->m_url, sreq->getUrlHash48() );
		return true;
	}

	// skip if not assigned to us for doling
	if ( ! isAssignedToUs ( sreq->m_firstIp ) ) {
		logDebug( g_conf.m_logDebugSpider, "spider: spider request not assigned to us. skipping." );
		return true;
	}

	if ( sreq->isCorrupt() ) {
		log( LOG_WARN, "spider: not adding corrupt spider req to doledb");
		return true;
	}

	// . get the url's length contained in this record
	// . it should be NULL terminated
	// . we set the ip here too
	int32_t ulen = sreq->getUrlLen();
	// watch out for corruption
	if ( sreq->m_firstIp ==  0 || sreq->m_firstIp == -1 || ulen <= 0 ) {
		log(LOG_ERROR,"spider: Corrupt spider req with url length of "
		    "%" PRId32" <= 0 u=%s. dataSize=%" PRId32" firstip=%" PRId32" uh48=%" PRIu64". Skipping.",
		    ulen,sreq->m_url,
		    sreq->m_dataSize,sreq->m_firstIp,sreq->getUrlHash48());
		return true;
	}

	// . we can't do this because we do not have the spiderReply!!!???
	// . MDW: no, we have to do it because tradesy.com has links to twitter
	//   on every page and twitter is not allowed so we continually
	//   re-scan a big spiderdblist for twitter's firstip. major performace
	//   degradation. so try to get ufn without reply. if we need
	//   a reply to get the ufn then this function should return -1 which
	//   means an unknown ufn and we'll add to waiting tree.
	// get ufn/priority,because if filtered we do not want to add to doledb
	// HACK: set isOutlink to true here since we don't know if we have sre
	int32_t ufn = ::getUrlFilterNum(sreq, NULL, nowGlobalMS, false, m_cr, true, -1);
	if (ufn >= 0) {
		// spiders disabled for this row in url filters?
		if (m_cr->m_maxSpidersPerRule[ufn] == 0) {
			logDebug(g_conf.m_logDebugSpider, "spider: request spidersoff ufn=%d url=%s", ufn, sreq->m_url);
			return true;
		}

		// do not add to doledb if bad
		if (m_cr->m_forceDelete[ufn]) {
			logDebug(g_conf.m_logDebugSpider, "spider: request %s is filtered ufn=%d", sreq->m_url, ufn);
			return true;
		}
	}

	// once in waiting tree, we will scan waiting tree and then lookup
	// each firstIp in waiting tree in spiderdb to get the best
	// SpiderRequest for that firstIp, then we can add it to doledb
	// as long as it can be spidered now
	bool added = addToWaitingTree(sreq->m_firstIp);

	// if already doled and we beat the priority/spidertime of what
	// was doled then we should probably delete the old doledb key
	// and add the new one. hmm, the waitingtree scan code ...

	// update the latest siteNumInlinks count for this "site"
	if (sreq->m_siteNumInlinksValid) {
		// updates m_siteNumInlinksTable
		updateSiteNumInlinksTable(sreq->m_siteHash32, sreq->m_siteNumInlinks, (time_t) sreq->m_addedTime);
		// clear error for this if there was any
		g_errno = 0;
	}

	// log it
	char ipbuf[16];
	logDebug( g_conf.m_logDebugSpider, "spider: %s request to waiting tree %s"
	          " uh48=%" PRIu64
	          " firstIp=%s "
	          " pageNumInlinks=%" PRIu32
	          " parentdocid=%" PRIu64
	          " isinjecting=%" PRId32
	          " ispagereindex=%" PRId32
	          " ufn=%" PRId32
	          " priority=%" PRId32
	          " addedtime=%" PRIu32,
	          added ? "ADDED" : "DIDNOTADD",
	          sreq->m_url,
	          sreq->getUrlHash48(),
	          iptoa(sreq->m_firstIp,ipbuf),
	          (uint32_t)sreq->m_pageNumInlinks,
	          sreq->getParentDocId(),
	          (int32_t)(bool)sreq->m_isInjecting,
	          (int32_t)(bool)sreq->m_isPageReindex,
	          (int32_t)sreq->m_ufn,
	          (int32_t)sreq->m_priority,
	          (uint32_t)sreq->m_addedTime );

	return true;
}

bool SpiderColl::printWaitingTree() {
	ScopedLock sl(m_waitingTree.getLock());

	for (int32_t node = m_waitingTree.getFirstNode_unlocked(); node >= 0;
	     node = m_waitingTree.getNextNode_unlocked(node)) {
		const key96_t *wk = reinterpret_cast<const key96_t*>(m_waitingTree.getKey_unlocked(node));
		// spider time is up top
		uint64_t spiderTimeMS = (wk->n1);
		spiderTimeMS <<= 32;
		spiderTimeMS |= ((wk->n0) >> 32);
		// then ip
		int32_t firstIp = wk->n0 & 0xffffffff;
		// show it
		char ipbuf[16];

		// for readable timestamp..
		time_t now_t = (time_t)(spiderTimeMS / 1000);
		struct tm tm_buf;
		struct tm *stm = gmtime_r(&now_t,&tm_buf);

		log(LOG_INFO,"dump: time=%" PRId64 " (%04d%02d%02d-%02d%02d%02d-%03d) firstip=%s",spiderTimeMS, stm->tm_year+1900,stm->tm_mon+1,stm->tm_mday,stm->tm_hour,stm->tm_min,stm->tm_sec,(int)(spiderTimeMS%1000), iptoa(firstIp,ipbuf));
	}
	return true;
}


//////
//
// . 1. called by addSpiderReply(). it should have the sameIpWait available
//      or at least that will be in the crawldelay cache table.
//      SpiderReply::m_crawlDelayMS. Unfortunately, no maxSpidersPerIP!!!
//      we just add a "0" in the waiting tree which means evalIpLoop() will
//      be called and can get the maxSpidersPerIP from the winning candidate
//      and add to the waiting tree based on that.
// . 2. called by addSpiderRequests(). It SHOULD maybe just add a "0" as well
//      to offload the logic. try that.
// . 3. called by populateWaitingTreeFromSpiderdb(). it just adds "0" as well,
//      if not doled
// . 4. UPDATED in evalIpLoop() if the best SpiderRequest for a firstIp is
//      in the future, this is the only time we will add a waiting tree key
//      whose spider time is non-zero. that is where we also take 
//      sameIpWait and maxSpidersPerIP into consideration. evalIpLoop() 
//      will actually REMOVE the entry from the waiting tree if that IP
//      already has the max spiders outstanding per IP. when a spiderReply
//      is received it will populate the waiting tree again with a "0" entry
//      and evalIpLoop() will re-do its check.
//
//////

// . returns true if we added to waiting tree, false if not
// . if one of these add fails consider increasing mem used by tree/table
// . if we lose an ip that sux because it won't be gotten again unless
//   we somehow add another request/reply to spiderdb in the future
bool SpiderColl::addToWaitingTree(int32_t firstIp) {
	char ipbuf[16];
	logDebug( g_conf.m_logDebugSpider, "spider: addtowaitingtree ip=%s", iptoa(firstIp,ipbuf) );

	// we are currently reading spiderdb for this ip and trying to find
	// a best SpiderRequest or requests to add to doledb. so if this 
	// happens, let the scan know that more replies or requests came in
	// while we were scanning so that it should not delete the rec from
	// waiting tree and not add to doledb, then we'd lose it forever or
	// until the next waitingtree rebuild was triggered in time.
	//
	// Before i was only setting this in addSpiderRequest() so if a new
	// reply came in it was not setting m_gotNewDataForScanninIp and
	// we ended up losing the IP from the waiting tree forever (or until
	// the next timed rebuild). putting it here seems to fix that.
	/// @todo ALC verify that we won't lose IP from waiting tree. Do we need to lock the whole evalIpLoop?
	if ( firstIp == m_scanningIp ) {
		m_gotNewDataForScanningIp = m_scanningIp.load();
		//log(LOG_DEBUG,"spider: got new data for %s",iptoa(firstIp));
		//return true;
	}

	// . this can now be only 0
	// . only evalIpLoop() will add a waiting tree key with a non-zero
	//   value after it figures out the EARLIEST time that a 
	//   SpiderRequest from this firstIp can be spidered.
	uint64_t spiderTimeMS = 0;

	// don't write to tree if we're shutting down
	if (g_process.isShuttingDown()) {
		log(LOG_WARN, "spider: addtowaitingtree: failed. shutting down");
		return false;
	}

	// only if we are the responsible host in the shard
	if ( ! isAssignedToUs ( firstIp ) ) 
		return false;

	// . do not add to waiting tree if already in doledb
	// . an ip should not exist in both doledb and waiting tree.
	// . waiting tree is meant to be a signal that we need to add
	//   a spiderrequest from that ip into doledb where it can be picked
	//   up for immediate spidering
	if (isInDoledbIpTable(firstIp)) {
		logDebug( g_conf.m_logDebugSpider, "spider: not adding to waiting tree, already in doleip table" );
		return false;
	}

	ScopedLock sl(m_waitingTree.getLock());

	// see if in tree already, so we can delete it and replace it below
	// . this is true if already in tree
	// . if spiderTimeMS is a sooner time than what this firstIp already
	//   has as its earliest time, then we will override it and have to
	//   update both m_waitingTree and m_waitingTable, however
	//   IF the spiderTimeMS is a later time, then we bail without doing
	//   anything at this point.
	int64_t sms;
	if (getFromWaitingTable(firstIp, &sms)) {
		// not only must we be a sooner time, but we must be 5-seconds
		// sooner than the time currently in there to avoid thrashing
		// when we had a ton of outlinks with this first ip within an
		// 5-second interval.
		//
		// i'm not so sure what i was doing here before, but i don't
		// want to starve the spiders, so make this 100ms not 5000ms
		if ( (int64_t)spiderTimeMS > sms - 100 ) {
			logDebug( g_conf.m_logDebugSpider, "spider: skip updating waiting tree" );
			return false;
		}

		// make the key then
		key96_t wk = makeWaitingTreeKey ( sms, firstIp );

		// must be there
		if (!m_waitingTree.deleteNode_unlocked(0, (char*)&wk, false)) {
			// sanity check. ensure waitingTable and waitingTree in sync
			gbshutdownLogicError();
		}

		// log the replacement
		logDebug( g_conf.m_logDebugSpider, "spider: replacing waitingtree key oldtime=%" PRIu32" newtime=%" PRIu32" firstip=%s",
		          (uint32_t)(sms/1000LL),
		          (uint32_t)(spiderTimeMS/1000LL),
		          iptoa(firstIp,ipbuf) );
	} else {
		// time of 0 means we got the reply for something we spidered
		// in doledb so we will need to recompute the best spider
		// requests for this first ip

		// log the replacement
		logDebug( g_conf.m_logDebugSpcache, "spider: adding new key to waitingtree newtime=%" PRIu32"%s firstip=%s",
		          (uint32_t)(spiderTimeMS/1000LL),
		          ( spiderTimeMS == 0 ) ? "(replyreset)" : "",
		          iptoa(firstIp,ipbuf) );
	}

	// what is this?
	if ( firstIp == 0 || firstIp == -1 ) {
		log(LOG_WARN, "spider: got ip of %s. cn=%" PRId32" "
		    "wtf? failed to add to "
		    "waiting tree, but return true anyway.",
		    iptoa(firstIp,ipbuf) ,
		    (int32_t)m_collnum);
		// don't return true lest m_waitingTreeNextKey never gets updated
		// and we end up in an infinite loop doing 
		// populateWaitingTreeFromSpiderdb()
		return true;
	}

	// grow the tree if too small!
	int32_t used = m_waitingTree.getNumUsedNodes_unlocked();
	int32_t max = m_waitingTree.getNumTotalNodes_unlocked();

	if ( used + 1 > max ) {
		int32_t more = (((int64_t)used) * 15) / 10;
		if ( more < 10 ) more = 10;
		if ( more > 100000 ) more = 100000;
		int32_t newNum = max + more;
		log(LOG_DEBUG, "spider: growing waiting tree to from %" PRId32" to %" PRId32" nodes for collnum %" PRId32,
		    max , newNum , (int32_t)m_collnum );
		if (!m_waitingTree.growTree_unlocked(newNum)) {
			log(LOG_ERROR, "Failed to grow waiting tree to add firstip %s", iptoa(firstIp,ipbuf));
			return false;
		}
		if (!setWaitingTableSize(newNum)) {
			log(LOG_ERROR, "Failed to grow waiting table to add firstip %s", iptoa(firstIp,ipbuf));
			return false;
		}
	}

	key96_t wk = makeWaitingTreeKey(spiderTimeMS, firstIp);

	// add that
	int32_t wn;
	if ((wn = m_waitingTree.addKey_unlocked(&wk)) < 0) {
		log(LOG_ERROR, "waitingtree add failed for ip=%s. increase max nodes lest we lose this IP forever. err=%s",
		    iptoa(firstIp,ipbuf), mstrerror(g_errno));
		return false;
	}

	// note it
	logDebug(g_conf.m_logDebugSpider, "spider: added time=%" PRId64" ip=%s to waiting tree node=%" PRId32,
	         spiderTimeMS, iptoa(firstIp,ipbuf), wn);

	// add to table now since its in the tree
	if (!addToWaitingTable(firstIp, spiderTimeMS)) {
		// remove from tree then
		m_waitingTree.deleteNode_unlocked(wn, false);

		log(LOG_ERROR, "waitingtable add failed for ip=%s. increase max nodes lest we lose this IP forever. err=%s",
		    iptoa(firstIp,ipbuf), mstrerror(g_errno));
		return false;
	}

	// tell caller there was no error
	return true;
}


// . this scan is started anytime we call addSpiderRequest() or addSpiderReply
// . if nothing is in tree it quickly exits
// . otherwise it scan the entries in the tree
// . each entry is a key with spiderTime/firstIp
// . if spiderTime > now it stops the scan
// . if the firstIp is already in doledb (m_doledbIpTable) then it removes
//   it from the waitingtree and waitingtable. how did that happen?
// . otherwise, it looks up that firstIp in spiderdb to get a list of all
//   the spiderdb recs from that firstIp
// . then it selects the "best" one and adds it to doledb. once added to
//   doledb it adds it to doleIpTable, and remove from waitingtree and 
//   waitingtable
// . returns false if blocked, true otherwise
int32_t SpiderColl::getNextIpFromWaitingTree() {
	// reset first key to get first rec in waiting tree
	m_waitingTreeKey.setMin();

	// current time on host #0
	uint64_t nowMS = gettimeofdayInMilliseconds();

	for (;;) {
		ScopedLock sl(m_waitingTree.getLock());

		// we might have deleted the only node below...
		if (m_waitingTree.isEmpty_unlocked()) {
			return 0;
		}

		// assume none
		int32_t firstIp = 0;
		// set node from wait tree key. this way we can resume from a prev key
		int32_t node = m_waitingTree.getNextNode_unlocked(0, (char *)&m_waitingTreeKey);
		// if empty, stop
		if (node < 0) {
			return 0;
		}

		// get the key
		const key96_t *k = reinterpret_cast<const key96_t *>(m_waitingTree.getKey_unlocked(node));

		// ok, we got one
		firstIp = (k->n0) & 0xffffffff;

		// sometimes we take over for a dead host, but if he's no longer
		// dead then we can remove his keys. but first make sure we have had
		// at least one ping from him so we do not remove at startup.
		// if it is in doledb or in the middle of being added to doledb
		// via msg4, nuke it as well!
		if (firstIp == 0 || firstIp == -1 || !isAssignedToUs(firstIp) || isInDoledbIpTable(firstIp)) {
			if (firstIp == 0 || firstIp == -1) {
				log(LOG_WARN, "spider: removing corrupt spiderreq firstip of %" PRId32"from waiting tree collnum=%i",
				    firstIp, (int)m_collnum);
			}

			// these operations should fail if writes have been disabled
			// and becase the trees/tables for spidercache are saving
			// in Process.cpp's g_spiderCache::save() call
			m_waitingTree.deleteNode_unlocked(node, true);

			char ipbuf[16];
			logDebug(g_conf.m_logDebugSpider, "spider: removed ip=%s from waiting tree. nn=%" PRId32,
			         iptoa(firstIp,ipbuf), m_waitingTree.getNumUsedNodes_unlocked());

			logDebug(g_conf.m_logDebugSpcache, "spider: erasing waitingtree key firstip=%s", iptoa(firstIp,ipbuf));

			// remove from table too!
			removeFromWaitingTable(firstIp);
			continue;
		}

		// spider time is up top
		uint64_t spiderTimeMS = (k->n1);
		spiderTimeMS <<= 32;
		spiderTimeMS |= ((k->n0) >> 32);

		// stop if need to wait for this one
		if (spiderTimeMS > nowMS) {
			return 0;
		}

		// save key for deleting when done
		m_waitingTreeKey.n1 = k->n1;
		m_waitingTreeKey.n0 = k->n0;
		m_waitingTreeKeyValid = true;
		m_scanningIp = firstIp;

		// compute the best request from spiderdb list, not valid yet
		m_lastReplyValid = false;

		// start reading spiderdb here
		m_nextKey = Spiderdb::makeFirstKey(firstIp);
		m_endKey = Spiderdb::makeLastKey(firstIp);

		// all done
		return firstIp;
	}
}

void SpiderColl::gotSpiderdbWaitingTreeListWrapper(void *state, RdbList *list, Msg5 *msg5) {
	SpiderColl *THIS = (SpiderColl *)state;

	// did our collection rec get deleted? since we were doing a read
	// the SpiderColl will have been preserved in that case but its
	// m_deleteMyself flag will have been set.
	if (tryToDeleteSpiderColl(THIS, "2")) {
		return;
	}

	THIS->m_gettingWaitingTreeList = false;

	THIS->populateWaitingTreeFromSpiderdb(true);
}



//////////////////
//////////////////
//
// THE BACKGROUND FUNCTION
//
// when the user changes the ufn table the waiting tree is flushed
// and repopulated from spiderdb with this. also used for repairs.
//
//////////////////
//////////////////

// . this stores an ip into the waiting tree with a spidertime of "0" so
//   it will be evaluate properly by populateDoledbFromWaitingTree()
//
// @@@ BR: "it seems they fall out over time" - wtf?
// . scan spiderdb to make sure each firstip represented in spiderdb is
//   in the waiting tree. it seems they fall out over time. we need to fix
//   that but in the meantime this should do a bg repair. and is nice to have
//
// . the waiting tree key is really just a spidertime and a firstip. so we will
//   still need populatedoledbfromwaitingtree to periodically scan firstips
//   that are already in doledb to see if it has a higher-priority request
//   for that firstip. in which case it can add that to doledb too, but then
//   we have to be sure to only grant one lock for a firstip to avoid hammering
//   that firstip
//
// . this should be called from a sleepwrapper, the same sleep wrapper we
//   call populateDoledbFromWaitingTree() from should be fine
void SpiderColl::populateWaitingTreeFromSpiderdb ( bool reentry ) {

	logTrace( g_conf.m_logTraceSpider, "BEGIN" );

	// skip if in repair mode
	if ( g_repairMode ) 
	{
		logTrace( g_conf.m_logTraceSpider, "END, in repair mode" );
		return;
	}

	// sanity
	if ( m_deleteMyself ) gbshutdownLogicError();
	// skip if spiders off
	if ( ! m_cr->m_spideringEnabled ) 
	{
		logTrace( g_conf.m_logTraceSpider, "END, spiders disabled" );
		return;
	}

	if ( ! g_hostdb.getMyHost( )->m_spiderEnabled ) 
	{
		logTrace( g_conf.m_logTraceSpider, "END, spiders disabled (2)" );
		return;
	}


	// skip if udp table is full
	if ( g_udpServer.getNumUsedSlotsIncoming() >= MAXUDPSLOTS ) 
	{
		logTrace( g_conf.m_logTraceSpider, "END, UDP table full" );
		return;
	}

	// if entering for the first time, we need to read list from spiderdb
	if ( ! reentry ) {
		// just return if we should not be doing this yet
		if ( ! m_waitingTreeNeedsRebuild ) 
		{
			logTrace( g_conf.m_logTraceSpider, "END, !m_waitingTreeNeedsRebuild" );
			return;
		}

		// a double call? can happen if list read is slow...
		if ( m_gettingWaitingTreeList )
		{
			logTrace( g_conf.m_logTraceSpider, "END, double call" );
			return;
		}

		// . borrow a msg5
		// . if none available just return, we will be called again
		//   by the sleep/timer function

		// . read in a replacement SpiderRequest to add to doledb from
		//   this ip
		// . get the list of spiderdb records
		// . do not include cache, those results are old and will mess
		//   us up
		char ipbuf[16];
		char keystrbuf[MAX_KEYSTR_BYTES];
		log(LOG_DEBUG,"spider: populateWaitingTree: calling msg5: startKey=0x%s firstip=%s",
		    KEYSTR(&m_waitingTreeNextKey,sizeof(m_waitingTreeNextKey),keystrbuf), iptoa(Spiderdb::getFirstIp(&m_waitingTreeNextKey),ipbuf));
		    
		// flag it
		m_gettingWaitingTreeList = true;
		// make state
		//int32_t state2 = (int32_t)m_cr->m_collnum;
		// read the list from local disk
		if(!SpiderdbRdbSqliteBridge::getList(m_cr->m_collnum,
						     &m_waitingTreeList,
						     m_waitingTreeNextKey,
						     *(const key128_t*)KEYMAX(),
						     SR_READ_SIZE))
		{
			if(!g_errno) {
				g_errno = EIO; //imprecise
				logTrace( g_conf.m_logTraceSpider, "END, got io-error from sqlite" );
				return;
			}
		}
	}

	// show list stats
	logDebug( g_conf.m_logDebugSpider, "spider: populateWaitingTree: got list of size %" PRId32, m_waitingTreeList.getListSize() );

	// unflag it
	m_gettingWaitingTreeList = false;

	// don't proceed if we're shutting down
	if (g_process.isShuttingDown()) {
		logTrace( g_conf.m_logTraceSpider, "END, process is shutting down" );
		return;
	}

	// ensure we point to the top of the list
	m_waitingTreeList.resetListPtr();
	// bail on error
	if ( g_errno ) {
		log(LOG_ERROR,"spider: Had error getting list of urls from spiderdb2: %s.", mstrerror(g_errno));
		//m_isReadDone2 = true;
		logTrace( g_conf.m_logTraceSpider, "END" );
		return;
	}

	int32_t lastOne = 0;
	// loop over all serialized spiderdb records in the list
	for ( ; ! m_waitingTreeList.isExhausted() ; ) {
		// get spiderdb rec in its serialized form
		const char *rec = m_waitingTreeList.getCurrentRec();
		// skip to next guy
		m_waitingTreeList.skipCurrentRecord();
		// negative? wtf?
		if ( (rec[0] & 0x01) == 0x00 ) {
			//logf(LOG_DEBUG,"spider: got negative spider rec");
			continue;
		}
		// if its a SpiderReply skip it
		if ( ! Spiderdb::isSpiderRequest ( (key128_t *)rec))
		{
			continue;
		}
			
		// cast it
		const SpiderRequest *sreq = reinterpret_cast<const SpiderRequest *>(rec);
		// get first ip
		int32_t firstIp = sreq->m_firstIp;

		// if same as last, skip it
		if ( firstIp == lastOne ) 
		{
			char ipbuf[16];
			logTrace( g_conf.m_logTraceSpider, "Skipping, IP [%s] same as last" , iptoa(firstIp,ipbuf));
			continue;
		}

		// set this lastOne for speed
		lastOne = firstIp;

		// if firstip already in waiting tree, skip it
		if (isInWaitingTable(firstIp)) {
			char ipbuf[16];
			logTrace( g_conf.m_logTraceSpider, "Skipping, IP [%s] already in waiting tree" , iptoa(firstIp,ipbuf));
			continue;
		}

		// skip if only our twin should add it to waitingtree/doledb
		if ( ! isAssignedToUs ( firstIp ) ) 
		{
			char ipbuf[16];
			logTrace( g_conf.m_logTraceSpider, "Skipping, IP [%s] not assigned to us" , iptoa(firstIp,ipbuf));
			continue;
		}

		// skip if ip already represented in doledb i guess otherwise
		// the populatedoledb scan will nuke it!!
		if (isInDoledbIpTable(firstIp)) {
			char ipbuf[16];
			logTrace( g_conf.m_logTraceSpider, "Skipping, IP [%s] already in doledb" , iptoa(firstIp,ipbuf));
			continue;
		}

		// not currently spidering either. when they got their
		// lock they called confirmLockAcquisition() which will
		// have added an entry to the waiting table. sometimes the
		// lock still exists but the spider is done. because the
		// lock persists for 5 seconds afterwards in case there was
		// a lock request for that url in progress, so it will be
		// denied.

		// . this is starving other collections , should be
		//   added to waiting tree anyway! otherwise it won't get
		//   added!!!
		// . so now i made this collection specific, not global
		if ( g_spiderLoop.getNumSpidersOutPerIp (firstIp,m_collnum)>0)
		{
			char ipbuf[16];
			logTrace( g_conf.m_logTraceSpider, "Skipping, IP [%s] is already being spidered" , iptoa(firstIp,ipbuf));
			continue;
		}

		// otherwise, we want to add it with 0 time so the doledb
		// scan will evaluate it properly
		// this will return false if we are saving the tree i guess
		if (!addToWaitingTree(firstIp)) {
			char ipbuf[16];
			log(LOG_INFO, "spider: failed to add ip %s to waiting tree. "
			              "ip will not get spidered then and our population of waiting tree will repeat until this add happens.",
			    iptoa(firstIp,ipbuf) );
			logTrace( g_conf.m_logTraceSpider, "END, addToWaitingTree for IP [%s] failed" , iptoa(firstIp,ipbuf));
			return;
		}
		else
		{
			char ipbuf[16];
			logTrace( g_conf.m_logTraceSpider, "IP [%s] added to waiting tree" , iptoa(firstIp,ipbuf));
		}

		// count it
		m_numAdded++;
		// ignore errors for this
		g_errno = 0;
	}


	// are we the final list in the scan?
	bool shortRead = ( m_waitingTreeList.getListSize() <= 0);

	m_numBytesScanned += m_waitingTreeList.getListSize();

	// reset? still left over from our first scan?
	if ( m_lastPrintCount > m_numBytesScanned )
		m_lastPrintCount = 0;

	// announce every 10MB
	if ( m_numBytesScanned - m_lastPrintCount > 10000000 ) {
		log(LOG_INFO, "spider: %" PRIu64" spiderdb bytes scanned for waiting tree re-population for cn=%" PRId32,m_numBytesScanned,
		    (int32_t)m_collnum);
		m_lastPrintCount = m_numBytesScanned;
	}

	// debug info
	log(LOG_DEBUG,"spider: Read2 %" PRId32" spiderdb bytes.",m_waitingTreeList.getListSize());
	// reset any errno cuz we're just a cache
	g_errno = 0;

	// if not done, keep going
	if ( ! shortRead ) {
		// . inc it here
		// . it can also be reset on a collection rec update
		key128_t lastKey  = *(key128_t *)m_waitingTreeList.getLastKey();

		if ( lastKey < m_waitingTreeNextKey ) {
			log(LOG_WARN, "spider: got corruption 9. spiderdb keys out of order for collnum=%" PRId32, (int32_t)m_collnum);

			// this should result in an empty list read for
			// our next scan of spiderdb. unfortunately we could
			// miss a lot of spider requests then
			KEYMAX((char*)&m_waitingTreeNextKey,sizeof(m_waitingTreeNextKey));
		}
		else {
			m_waitingTreeNextKey  = lastKey;
			m_waitingTreeNextKey++;
		}

		// watch out for wrap around
		if ( m_waitingTreeNextKey < lastKey ) shortRead = true;
		// nah, advance the firstip, should be a lot faster when
		// we are only a few firstips...
		if ( lastOne && lastOne != -1 ) { // && ! gotCorruption ) {
			key128_t cand = Spiderdb::makeFirstKey(lastOne+1);
			// corruption still seems to happen, so only
			// do this part if it increases the key to avoid
			// putting us into an infinite loop.
			if ( cand > m_waitingTreeNextKey ) m_waitingTreeNextKey = cand;
		}
	}

	if ( shortRead ) {
		// mark when the scan completed so we can do another one
		// like 24 hrs from that...
		m_lastScanTime = getTimeLocal();

		log(LOG_DEBUG, "spider: WaitingTree rebuild complete for %s. Added %" PRId32" recs to waiting tree, scanned %" PRId64" bytes of spiderdb.",
		    m_coll, m_numAdded, m_numBytesScanned);
		//printWaitingTree();

		// reset the count for next scan
		m_numAdded = 0 ;
		m_numBytesScanned = 0;
		// reset for next scan
		m_waitingTreeNextKey.setMin();
		// no longer need rebuild
		m_waitingTreeNeedsRebuild = false;
	}

	// free list to save memory
	m_waitingTreeList.freeList();
	// wait for sleepwrapper to call us again with our updated m_waitingTreeNextKey
	logTrace( g_conf.m_logTraceSpider, "END, done" );
	return;
}



//static bool    s_ufnTreeSet = false;
//static RdbTree s_ufnTree;
//static time_t  s_lastUfnTreeFlushTime = 0;

//////////////////////////
//////////////////////////
//
// The first KEYSTONE function.
//
// CALL THIS ANYTIME to load up doledb from waiting tree entries
//
// This is a key function.
//
// It is called from two places:
//
// 1) sleep callback
//
// 2) addToWaitingTree()
//    is called from addSpiderRequest() anytime a SpiderRequest
//    is added to spiderdb (or from addSpiderReply())
//
// It can only be entered once so will just return if already scanning 
// spiderdb.
//
//////////////////////////
//////////////////////////

// . for each IP in the waiting tree, scan all its SpiderRequests and determine
//   which one should be the next to be spidered. and put that one in doledb.
// . we call this a lot, like if the admin changes the url filters table
//   we have to re-scan all of spiderdb basically and re-do doledb
void SpiderColl::populateDoledbFromWaitingTree ( ) { // bool reentry ) {
	
	logTrace( g_conf.m_logTraceSpider, "BEGIN" );

	// only one loop can run at a time!
	if ( m_isPopulatingDoledb ) {
		logTrace( g_conf.m_logTraceSpider, "END, already populating doledb" );
		return;
	}
	
	// skip if in repair mode
	if ( g_repairMode ) {
		logTrace( g_conf.m_logTraceSpider, "END, in repair mode" );
		return;
	}

	// let's skip if spiders off so we can inject/popoulate the index quick
	// since addSpiderRequest() calls addToWaitingTree() which then calls
	// this. 
	if ( ! g_conf.m_spideringEnabled ) {
		logTrace( g_conf.m_logTraceSpider, "END, spidering not enabled" );
		return;
	}
	
	if ( ! g_hostdb.getMyHost( )->m_spiderEnabled ) {
		logTrace( g_conf.m_logTraceSpider, "END, spidering not enabled (2)" );
		return;
	}

	// skip if udp table is full
	if ( g_udpServer.getNumUsedSlotsIncoming() >= MAXUDPSLOTS ) {
		logTrace( g_conf.m_logTraceSpider, "END, no more UDP slots" );
		return;
	}

	// set this flag so we are not re-entered
	m_isPopulatingDoledb = true;

	for(;;) {
		// are we trying to exit? some firstip lists can be quite long, so
		// terminate here so all threads can return and we can exit properly
		if (g_process.isShuttingDown()) {
			m_isPopulatingDoledb = false;
			logTrace( g_conf.m_logTraceSpider, "END, shutting down" );
			return;
		}

		// . get next IP that is due to be spidered from
		// . also sets m_waitingTreeKey so we can delete it easily!
		int32_t ip = getNextIpFromWaitingTree();
		
		// . return if none. all done. unset populating flag.
		// . it returns 0 if the next firstip has a spidertime in the future
		if ( ip == 0 ) {
			m_isPopulatingDoledb = false;
			return;
		}

		// set read range for scanning spiderdb
		m_nextKey = Spiderdb::makeFirstKey(ip);
		m_endKey  = Spiderdb::makeLastKey (ip);

		char ipbuf[16];
		logDebug( g_conf.m_logDebugSpider, "spider: for cn=%i nextip=%s nextkey=%s",
			(int)m_collnum, iptoa(ip,ipbuf), KEYSTR( &m_nextKey, sizeof( key128_t ) ) );

		//////
		//
		// do TWO PASSES, one to count pages, the other to get the best url!!
		//
		//////
		// assume we don't have to do two passes
		m_countingPagesIndexed = false;

		// get the collectionrec
		const CollectionRec *cr = g_collectiondb.getRec ( m_collnum );
		// but if we have quota based url filters we do have to count
		if ( cr && cr->m_urlFiltersHavePageCounts ) {
			// tell evalIpLoop() to count first
			m_countingPagesIndexed = true;
			// reset this stuff used for counting UNIQUE votes
			m_lastReqUh48a = 0LL;
			m_lastReqUh48b = 0LL;
			m_lastRepUh48  = 0LL;
			// and setup the LOCAL counting table if not initialized
			if (!m_siteIndexedDocumentCount.isInitialized()) {
					m_siteIndexedDocumentCount.set(4, 4, 0, NULL, 0, false, "ltpct");
			}
			// otherwise, just reset it so we can repopulate it
			else m_siteIndexedDocumentCount.reset();
		}

		logDebug( g_conf.m_logDebugSpider, "spider: evalIpLoop: waitingtree nextip=%s numUsedNodes=%" PRId32,
			iptoa(ip,ipbuf), m_waitingTree.getNumUsedNodes() );

	//@@@@@@ BR: THIS SHOULD BE DEBUGGED AND ENABLED

		/*
		// assume using tree
		m_useTree = true;

		// . flush the tree every 12 hours
		// . i guess we could add incoming requests to the ufntree if
		//   they strictly beat the ufn tree tail node, HOWEVER, we
		//   still have the problem of that if a url we spidered is due
		//   to be respidered very soon we will miss it, as only the reply
		//   is added back into spiderdb, not a new request.
		int32_t nowLocal = getTimeLocal();
		// make it one hour so we don't cock-block a new high priority
		// request that just got added... crap, what if its an addurl
		// or something like that????
		if ( nowLocal - s_lastUfnTreeFlushTime > 3600 ) {
			s_ufnTree.clear();
			s_lastUfnTreeFlushTime = nowLocal;
		}

		int64_t uh48;

		//
		// s_ufnTree tries to cache the top X spiderrequests for an IP
		// that should be spidered next so we do not have to scan like
		// a million spiderrequests in spiderdb to find the best one.
		//

		// if we have a specific uh48 targetted in s_ufnTree then that
		// saves a ton of time!
		// key format for s_ufnTree:
		// iiiiiiii iiiiiiii iiiiiii iiiiiii  i = firstip
		// PPPPPPPP tttttttt ttttttt ttttttt  P = priority
		// tttttttt tttttttt hhhhhhh hhhhhhh  t = spiderTimeMS (40 bits)
		// hhhhhhhh hhhhhhhh hhhhhhh hhhhhhh  h = urlhash48
		key128_t key;
		key.n1 = ip;
		key.n1 <<= 32;
		key.n0 = 0LL;
		int32_t node = s_ufnTree.getNextNode_unlocked(0,(char *)&key);
		// cancel node if not from our ip
		if ( node >= 0 ) {
			key128_t *rk = (key128_t *)s_ufnTree.getKey ( node );
			if ( (rk->n1 >> 32) != (uint32_t)ip ) node = -1;
		}
		if ( node >= 0 ) {
			// get the key
			key128_t *nk = (key128_t *)s_ufnTree.getKey ( node );
			// parse out uh48
			uh48 = nk->n0;
			// mask out spidertimems
			uh48 &= 0x0000ffffffffffffLL;
			// use that to refine the key range immensley!
			m_nextKey = g_spiderdb.makeFirstKey2 (ip, uh48);
			m_endKey  = g_spiderdb.makeLastKey2  (ip, uh48);
			// do not add the recs to the tree!
			m_useTree = false;
		}
		*/

		// so we know if we are the first read or not...
		m_firstKey = m_nextKey;

		// . initialize this before scanning the spiderdb recs of an ip
		// . it lets us know if we recvd new spider requests for m_scanningIp
		//   while we were doing the scan
		m_gotNewDataForScanningIp = 0;

		m_lastListSize = -1;

		// let evalIpLoop() know it has not yet tried to read from spiderdb
		m_didRead = false;

		// reset this
		int32_t maxWinners = (int32_t)MAX_WINNER_NODES;

		if (m_winnerTree.getNumNodes() == 0 &&
		!m_winnerTree.set(-1, maxWinners, maxWinners * MAX_REQUEST_SIZE, true, "wintree", NULL,
				sizeof(key192_t), -1)) {
			m_isPopulatingDoledb = false;
			log(LOG_ERROR, "Could not initialize m_winnerTree: %s",mstrerror(g_errno));
			logTrace( g_conf.m_logTraceSpider, "END, after winnerTree.set" );
			return;
		}

		if ( ! m_winnerTable.isInitialized() &&
		! m_winnerTable.set ( 8 , // uh48 is key
					sizeof(key192_t) , // winnertree key is data
					64 , // 64 slots initially
					NULL ,
					0 ,
					false , // allow dups?
					"wtdedup" ) ) {
			m_isPopulatingDoledb = false;
			log(LOG_ERROR, "Could not initialize m_winnerTable: %s",mstrerror(g_errno));
			logTrace( g_conf.m_logTraceSpider, "END, after winnerTable.set" );
			return;
		}

		// clear it before evaluating this ip so it is empty
		m_winnerTree.clear();

		// and table as well now
		m_winnerTable.clear();

		// reset this as well
		m_minFutureTimeMS = 0LL;
		m_totalBytesScanned = 0LL;
		m_totalNewSpiderRequests = 0LL;
		m_lastOverflowFirstIp = 0;
		
		// . look up in spiderdb otherwise and add best req to doledb from ip
		// . if it blocks ultimately it calls gotSpiderdbListWrapper() which
		//   calls this function again with re-entry set to true
		if ( ! evalIpLoop () ) {
			logTrace( g_conf.m_logTraceSpider, "END, after evalIpLoop" );
			return ;
		}

		// oom error? i've seen this happen and we end up locking up!
		if ( g_errno ) {
			log( "spider: evalIpLoop: %s", mstrerror(g_errno) );
			m_isPopulatingDoledb = false;
			logTrace( g_conf.m_logTraceSpider, "END, error after evalIpLoop" );
			return;
		}
	}
}



///////////////////
//
// KEYSTONE FUNCTION
//
// . READ ALL spiderdb recs for IP of m_scanningIp
// . add winner to doledb
// . called ONLY by populateDoledbFromWaitingTree()
//
// . continually scan spiderdb requests for a particular ip, m_scanningIp
// . compute the best spider request to spider next
// . add it to doledb
// . getNextIpFromWaitingTree() must have been called to set m_scanningIp
//   otherwise m_bestRequestValid might not have been reset to false
//
///////////////////

bool SpiderColl::evalIpLoop ( ) {
	logTrace( g_conf.m_logTraceSpider, "BEGIN" );
	//testWinnerTreeKey ( );

	// sanity
	if ( m_scanningIp == 0 || m_scanningIp == -1 ) gbshutdownLogicError();

	// are we trying to exit? some firstip lists can be quite long, so
	// terminate here so all threads can return and we can exit properly
	if (g_process.isShuttingDown()) {
		logTrace( g_conf.m_logTraceSpider, "END, shutting down" );
		return true;
	}

	bool useCache = true;
	const CollectionRec *cr = g_collectiondb.getRec ( m_collnum );

	// did our collection rec get deleted? since we were doing a read
	// the SpiderColl will have been preserved in that case but its
	// m_deleteMyself flag will have been set.
	if ( tryToDeleteSpiderColl ( this ,"6" ) ) return false;

	// if doing site or page quotes for the sitepages or domainpages
	// url filter expressions, we can't muck with the cache because
	// we end up skipping the counting part.
	if ( ! cr )
		useCache = false;
	if ( cr && cr->m_urlFiltersHavePageCounts )
		useCache = false;
	if ( m_countingPagesIndexed )
		useCache = false;
	// assume not from cache
	if ( useCache ) {
		// if this ip is in the winnerlistcache use that. it saves us a lot of time.
		key96_t cacheKey;
		cacheKey.n0 = m_scanningIp;
		cacheKey.n1 = 0;
		char *doleBuf = NULL;
		size_t doleBufSize;
		//g_spiderLoop.m_winnerListCache.verify();
		FxBlobCacheLock<int32_t> rcl(g_spiderLoop.m_winnerListCache);
		bool inCache = g_spiderLoop.m_winnerListCache.lookup(m_scanningIp, (void**)&doleBuf, &doleBufSize);
		if ( inCache ) {
			int32_t crc = hash32 ( doleBuf + 4 , doleBufSize - 4 );
	
			char ipbuf[16];
			logDebug( g_conf.m_logDebugSpider, "spider: GOT %zu bytes of SpiderRequests "
				"from winnerlistcache for ip %s ptr=0x%" PTRFMT" crc=%" PRIu32,
				doleBufSize,
				iptoa(m_scanningIp,ipbuf),
				(PTRTYPE)doleBuf,
				crc);
	
			//copy doleuf out from cache so we can release the lock
			SafeBuf sb;
			sb.safeMemcpy(doleBuf,doleBufSize);

			rcl.unlock();

			// now add the first rec m_doleBuf into doledb's tree
			// and re-add the rest back to the cache with the same key.
			bool rc = addDoleBufIntoDoledb(&sb,true);
	
			logTrace( g_conf.m_logTraceSpider, "END, after addDoleBufIntoDoledb. returning %s", rc ? "true" : "false" );
			return rc;
		}
	}

 top:

	// did our collection rec get deleted? since we were doing a read
	// the SpiderColl will have been preserved in that case but its
	// m_deleteMyself flag will have been set.
	if ( tryToDeleteSpiderColl ( this, "4" ) ) {
		logTrace( g_conf.m_logTraceSpider, "END, after tryToDeleteSpiderColl (4)" );
		return false;
	}

	// if first time here, let's do a read first
	if ( ! m_didRead ) {
		// reset list size to 0
		m_list.reset();
		// assume we did a read now
		m_didRead = true;
		// reset some stuff
		m_lastScanningIp = 0;

		// reset these that need to keep track of requests for
		// the same url that might span two spiderdb lists or more
		m_lastSreqUh48 = 0LL;

		// do a read. if it blocks it will recall this loop
		if ( ! readListFromSpiderdb () ) {
			logTrace( g_conf.m_logTraceSpider, "END, readListFromSpiderdb returned false" );
			return false;
		}
	}

	for(;;) {
		// did our collection rec get deleted? since we were doing a read
		// the SpiderColl will have been preserved in that case but its
		// m_deleteMyself flag will have been set.
		if ( tryToDeleteSpiderColl ( this, "5" ) ) {
			// pretend to block since we got deleted!!!
			logTrace( g_conf.m_logTraceSpider, "END, after tryToDeleteSpiderColl (5)" );
			return false;
		}

		// . did reading the list from spiderdb have an error?
		// . i guess we don't add to doledb then
		if ( g_errno ) {
			log(LOG_ERROR,"spider: Had error getting list of urls from spiderdb: %s.",mstrerror(g_errno));

			// save mem
			m_list.freeList();

			logTrace( g_conf.m_logTraceSpider, "END, g_errno %" PRId32, g_errno );
			return true;
		}


		// if we started reading, then assume we got a fresh list here
		logDebug( g_conf.m_logDebugSpider, "spider: back from msg5 spiderdb read2 of %" PRId32" bytes (cn=%" PRId32")",
		         m_list.getListSize(), (int32_t)m_collnum );

		// . set the winning request for all lists we read so far
		// . if m_countingPagesIndexed is true this will just fill in
		//   quota info into m_localTable...
		scanListForWinners();

		// if list not empty, keep reading!
		if(m_list.isEmpty())
			break;
		// update m_nextKey for successive reads of spiderdb by
		// calling readListFromSpiderdb()
		key128_t lastKey  = *(key128_t *)m_list.getLastKey();
		// sanity
		//if ( endKey != finalKey ) gbshutdownLogicError();
		// crazy corruption?
		if ( lastKey < m_nextKey ) {
			char ipbuf[16];
			log(LOG_WARN, "spider: got corruption. spiderdb keys out of order for "
			    "collnum=%" PRId32" for evaluation of firstip=%s so terminating evaluation of that firstip." ,
			    (int32_t)m_collnum, iptoa(m_scanningIp,ipbuf));

			// this should result in an empty list read for
			// m_scanningIp in spiderdb
			m_nextKey  = m_endKey;
		}
		else {
			m_nextKey  = lastKey;
			m_nextKey++;
		}
		// . watch out for wrap around
		// . normally i would go by this to indicate that we are
		//   done reading, but there's some bugs... so we go
		//   by whether our list is empty or not for now
		if(m_nextKey < lastKey)
			m_nextKey = lastKey;
		// reset list to save mem
		m_list.reset();
		// read more! return if it blocked
		if(!readListFromSpiderdb())
			return false;
		// we got a list without blocking
	}


	// . we are all done if last list read was empty
	// . if we were just counting pages for quota, do a 2nd pass!
	if ( m_countingPagesIndexed ) {
		// do not do again. 
		m_countingPagesIndexed = false;
		// start at the top again
		m_nextKey = Spiderdb::makeFirstKey(m_scanningIp);
		// this time m_localTable should have the quota info in it so 
		// getUrlFilterNum() can use that
		m_didRead = false;
		// do the 2nd pass. read list from the very top.
		goto top;
	}

	// free list to save memory
	m_list.freeList();

	// . add all winners if we can in m_winnerTree into doledb
	// . if list was empty, then reading is all done so take the winner we 
	//   got from all the lists we did read for this IP and add him 
	//   to doledb
	// . if no winner exists, then remove m_scanningIp from m_waitingTree
	//   so we do not waste our time again. if url filters change then
	//   waiting tree will be rebuilt and we'll try again... or if
	//   a new spider request or reply for this ip comes in we'll try
	//   again as well...
	// . this returns false if blocked adding to doledb using msg1
	if ( ! addWinnersIntoDoledb() ) {
		logTrace( g_conf.m_logTraceSpider, "END, returning false. After addWinnersIntoDoledb" );
		return false;
	}

	// we are done...
	logTrace( g_conf.m_logTraceSpider, "END, all done" );
	return true;
}



// . this is ONLY CALLED from evalIpLoop() above
// . returns false if blocked, true otherwise
// . returns true and sets g_errno on error
bool SpiderColl::readListFromSpiderdb ( ) {
	logTrace( g_conf.m_logTraceSpider, "BEGIN" );
		
	if ( ! m_waitingTreeKeyValid ) gbshutdownLogicError();
	if ( ! m_scanningIp ) gbshutdownLogicError();

	const CollectionRec *cr = g_collectiondb.getRec ( m_collnum );
	if ( ! cr ) {
		log(LOG_ERROR,"spider: lost collnum %" PRId32,(int32_t)m_collnum);
		g_errno = ENOCOLLREC;
		
		logTrace( g_conf.m_logTraceSpider, "END, ENOCOLLREC" );
		return true;
	}

	// i guess we are always restricted to an ip, because
	// populateWaitingTreeFromSpiderdb calls its own msg5.
	int32_t firstIp0 = Spiderdb::getFirstIp(&m_nextKey);
	// sanity
	if ( m_scanningIp != firstIp0 ) gbshutdownLogicError();
	// sometimes we already have this ip in doledb/doleiptable
	// already and somehow we try to scan spiderdb for it anyway
	if (isInDoledbIpTable(firstIp0)) gbshutdownLogicError();
		
	// if it got zapped from the waiting tree by the time we read the list
	if (!isInWaitingTable(m_scanningIp)) {
		logTrace( g_conf.m_logTraceSpider, "END, IP no longer in waitingTree" );
		return true;
	}
	
	// sanity check
	if (!m_waitingTree.getNode(0, (char *)&m_waitingTreeKey)) {
		// it gets removed because addSpiderReply() calls addToWaitingTree
		// and replaces the node we are scanning with one that has a better
		// time, an earlier time, even though that time may have come and
		// we are scanning it now. perhaps addToWaitingTree() should ignore
		// the ip if it equals m_scanningIp?
		log(LOG_WARN, "spider: waiting tree key removed while reading list for %s (%" PRId32")", cr->m_coll,(int32_t)m_collnum);
		logTrace( g_conf.m_logTraceSpider, "END, waitingTree node was removed" );
		return true;
	}

	// . read in a replacement SpiderRequest to add to doledb from
	//   this ip
	// . get the list of spiderdb records
	// . do not include cache, those results are old and will mess
	//   us up
	if (g_conf.m_logDebugSpider ) {
		// got print each out individually because KEYSTR
		// uses a static buffer to store the string
		SafeBuf tmp;
		char ipbuf[16];
		tmp.safePrintf("spider: readListFromSpiderdb: calling msg5: ");
		tmp.safePrintf("firstKey=%s ", KEYSTR(&m_firstKey,sizeof(key128_t)));
		tmp.safePrintf("endKey=%s ", KEYSTR(&m_endKey,sizeof(key128_t)));
		tmp.safePrintf("nextKey=%s ", KEYSTR(&m_nextKey,sizeof(key128_t)));
		tmp.safePrintf("firstip=%s ", iptoa(m_scanningIp,ipbuf));
		tmp.safePrintf("(cn=%" PRId32")",(int32_t)m_collnum);
		log(LOG_DEBUG,"%s",tmp.getBufStart());
	}
	
	// log this better
	char ipbuf[16];
	logDebug(g_conf.m_logDebugSpider, "spider: readListFromSpiderdb: firstip=%s key=%s",
	         iptoa(m_scanningIp,ipbuf), KEYSTR( &m_nextKey, sizeof( key128_t ) ) );
		    
	// . read the list from local disk
	// . if a niceness 0 intersect thread is taking a LONG time
	//   then this will not complete in a long time and we
	//   end up timing out the round. so try checking for
	//   m_gettingList in spiderDoledUrls() and setting
	//   m_lastSpiderCouldLaunch
	if(!SpiderdbRdbSqliteBridge::getList(m_cr->m_collnum,
					     &m_list,
					     m_nextKey,
					     m_endKey,
					     SR_READ_SIZE))
	{
		if(!g_errno)
			g_errno = EIO; //imprecise
		logTrace( g_conf.m_logTraceSpider, "END, got io-error from sqlite" );
		return true;
	}

	// note its return
	logDebug( g_conf.m_logDebugSpider, "spider: back from msg5 spiderdb read of %" PRId32" bytes",m_list.getListSize());
		
	// got it without blocking. maybe all in tree or in cache
	logTrace( g_conf.m_logTraceSpider, "END, didn't block" );
	return true;
}



static int32_t s_lastIn  = 0;
static int32_t s_lastOut = 0;

bool SpiderColl::isFirstIpInOverflowList(int32_t firstIp) const {
	if ( ! m_overflowList ) return false;
	if ( firstIp == 0 || firstIp == -1 ) return false;
	if ( firstIp == s_lastIn ) return true;
	if ( firstIp == s_lastOut ) return false;
	for ( int32_t oi = 0 ; ; oi++ ) {
		// stop at end
		if ( ! m_overflowList[oi] ) break;
		// an ip of zero is end of the list
		if ( m_overflowList[oi] == firstIp ) {
			s_lastIn = firstIp;
			return true;
		}
	}
	s_lastOut = firstIp;
	return false;
}



// . ADDS top X winners to m_winnerTree
// . this is ONLY CALLED from evalIpLoop() above
// . scan m_list that we read from spiderdb for m_scanningIp IP
// . set m_bestRequest if an request in m_list is better than what is
//   in m_bestRequest from previous lists for this IP
bool SpiderColl::scanListForWinners ( ) {
	// if list is empty why are we here?
	if ( m_list.isEmpty() ) return true;

	// don't proceed if we're shutting down
	if (g_process.isShuttingDown()) {
		return true;
	}

	// ensure we point to the top of the list
	m_list.resetListPtr();

	// get this
	int64_t nowGlobalMS = gettimeofdayInMilliseconds();//Local();
	uint32_t nowGlobal   = nowGlobalMS / 1000;

	const SpiderReply *srep = NULL;
	int64_t      srepUh48 = 0;

	// if we are continuing from another list...
	if ( m_lastReplyValid ) {
		srep     = (SpiderReply *)m_lastReplyBuf;
		srepUh48 = srep->getUrlHash48();
	}

	// show list stats
	char ipbuf[16];
	logDebug( g_conf.m_logDebugSpider, "spider: readListFromSpiderdb: got list of size %" PRId32" for firstip=%s",
	          m_list.getListSize(), iptoa(m_scanningIp,ipbuf) );


	// if we don't read minRecSizes worth of data that MUST indicate
	// there is no more data to read. put this theory to the test
	// before we use it to indcate an end of list condition.
	if ( m_list.getListSize() > 0 && 
	     m_lastScanningIp == m_scanningIp &&
	     m_lastListSize < (int32_t)SR_READ_SIZE &&
	     m_lastListSize >= 0 ) {
		log(LOG_ERROR,"spider: shucks. spiderdb reads not full.");
	}

	m_lastListSize = m_list.getListSize();
	m_lastScanningIp = m_scanningIp;

	m_totalBytesScanned += m_list.getListSize();

	if ( m_list.isEmpty() ) {
		logDebug( g_conf.m_logDebugSpider, "spider: failed to get rec for ip=%s", iptoa(m_scanningIp,ipbuf) );
	}


	int32_t firstIp = m_waitingTreeKey.n0 & 0xffffffff;

	key128_t finalKey;
	int32_t recCount = 0;

	// loop over all serialized spiderdb records in the list
	for ( ; ! m_list.isExhausted() ; ) {
		// stop coring on empty lists
		if ( m_list.isEmpty() ) break;
		// get spiderdb rec in its serialized form
		char *rec = m_list.getCurrentRec();
		// count it
		recCount++;
		// sanity
		gbmemcpy ( (char *)&finalKey , rec , sizeof(key128_t) );
		// skip to next guy
		m_list.skipCurrentRecord();
		// negative? wtf?
		if ( (rec[0] & 0x01) == 0x00 ) {
			logf(LOG_DEBUG,"spider: got negative spider rec");
			continue;
		}
		// if its a SpiderReply set it for an upcoming requests
		if ( ! Spiderdb::isSpiderRequest ( (key128_t *)rec ) ) {

			// see if this is the most recent one
			const SpiderReply *tmp = (SpiderReply *)rec;

			// . MDW: we have to detect corrupt replies up here so
			//   they do not become the winning reply because
			//   their date is in the future!!

			if ( tmp->m_spideredTime > nowGlobal + 1 ) {
				if ( m_cr->m_spiderCorruptCount == 0 ) {
					log( LOG_WARN, "spider: got corrupt time spiderReply in scan uh48=%" PRId64" httpstatus=%" PRId32" datasize=%" PRId32" (cn=%" PRId32") ip=%s",
						tmp->getUrlHash48(),
						(int32_t)tmp->m_httpStatus,
						tmp->m_dataSize,
						(int32_t)m_collnum,
						iptoa(m_scanningIp,ipbuf));
				}
				m_cr->m_spiderCorruptCount++;
				// don't nuke it just for that...
				//srep = NULL;
				continue;
			}

			// . this is -1 on corruption
			// . i've seen -31757, 21... etc for bad http replies
			//   in the qatest123 doc cache... so turn off for that
			if ( tmp->m_httpStatus >= 1000 ) {
				if ( m_cr->m_spiderCorruptCount == 0 ) {
					log(LOG_WARN, "spider: got corrupt 3 spiderReply in scan uh48=%" PRId64" httpstatus=%" PRId32" datasize=%" PRId32" (cn=%" PRId32") ip=%s",
					    tmp->getUrlHash48(),
					    (int32_t)tmp->m_httpStatus,
					    tmp->m_dataSize,
					    (int32_t)m_collnum,
					    iptoa(m_scanningIp,ipbuf));
				}
				m_cr->m_spiderCorruptCount++;
				// don't nuke it just for that...
				//srep = NULL;
				continue;
			}

			// bad langid?
			if ( ! getLanguageAbbr (tmp->m_langId) ) {
				log(LOG_WARN, "spider: got corrupt 4 spiderReply in scan uh48=%" PRId64" langid=%" PRId32" (cn=%" PRId32") ip=%s",
				    tmp->getUrlHash48(),
				    (int32_t)tmp->m_langId,
				    (int32_t)m_collnum,
				    iptoa(m_scanningIp,ipbuf));
				m_cr->m_spiderCorruptCount++;
				//srep = NULL;
				continue;
			}

			// if we are corrupt, skip us
			if ( tmp->getRecSize() > (int32_t)MAX_SP_REPLY_SIZE )
				continue;

			// if we have a more recent reply already, skip this 
			if ( srep && 
			     srep->getUrlHash48() == tmp->getUrlHash48() &&
			     srep->m_spideredTime >= tmp->m_spideredTime )
				continue;
			// otherwise, assign it
			srep     = tmp;
			srepUh48 = srep->getUrlHash48();
			continue;
		}
		// cast it
		SpiderRequest *sreq = (SpiderRequest *)rec;

		// skip if our twin or another shard should handle it
		if ( ! isAssignedToUs ( sreq->m_firstIp ) ) {
			continue;
		}

		int64_t uh48 = sreq->getUrlHash48();

		// null out srep if no match
		if ( srep && srepUh48 != uh48 ) {
			srep = NULL;
		}

		// . ignore docid-based requests if spidered the url afterwards
		// . these are one-hit wonders
		// . once done they can be deleted
		if ( sreq->m_isPageReindex && srep && srep->m_spideredTime > sreq->m_addedTime ) {
			logDebug( g_conf.m_logDebugSpider, "spider: skipping9 %s", sreq->m_url );
			continue;
		}

		// if a replie-less new url spiderrequest count it
		// avoid counting query reindex requests
		if ( ! srep && m_lastSreqUh48 != uh48 && ! sreq->m_fakeFirstIp ) {
			m_totalNewSpiderRequests++;
		}

		int32_t cblock = ipdom ( sreq->m_firstIp );

		bool countIt = true;

		// reset page inlink count on url request change
		if ( m_lastSreqUh48 != uh48 ) {
			m_pageNumInlinks = 0;
			m_lastCBlockIp = 0;
		}


		if ( cblock == m_lastCBlockIp ||
		     // do not count manually added spider requests
		     sreq->m_isAddUrl || sreq->m_isInjecting ||
		     // 20 is good enough
		     m_pageNumInlinks >= 20 ) {
			countIt = false;
		}

		if ( countIt ) {
			int32_t ca;
			for ( ca = 0 ; ca < m_pageNumInlinks ; ca++ ) {
				if ( m_cblocks[ ca ] == cblock ) {
					break;
				}
			}

			// if found in our list, do not count it, already did
			if ( ca < m_pageNumInlinks ) {
				countIt = false;
			}
		}

		if ( countIt ) {
			m_cblocks[m_pageNumInlinks] = cblock;
			m_pageNumInlinks++;
			if ( m_pageNumInlinks > 20 ) gbshutdownAbort(true);
		}

		// set this now. it does increase with each request. so 
		// initial requests will not see the full # of inlinks.
		sreq->m_pageNumInlinks = (uint8_t)m_pageNumInlinks;

		m_lastSreqUh48 = uh48;
		m_lastCBlockIp = cblock;

		// only add firstip if manually added and not fake

		//
		// just calculating page counts? if the url filters are based
		// on the # of pages indexed per ip or subdomain/site then
		// we have to maintain a page count table. sitepages.
		//
		if ( m_countingPagesIndexed ) {
			// only add dom/site hash seeds if it is
			// a fake firstIp to avoid double counting seeds
			if ( sreq->m_fakeFirstIp ) continue;
			// count the manual additions separately. mangle their
			// hash with 0x123456 so they are separate.
			if ( (sreq->m_isAddUrl || sreq->m_isInjecting) &&
			     // unique votes per seed
			     uh48 != m_lastReqUh48a ) {
				// do not repeat count the same url
				m_lastReqUh48a = uh48;
				// sanity
				if ( ! sreq->m_siteHash32) gbshutdownAbort(true); //isj: this abort is questionable
				// do a little magic because we count
				// seeds as "manual adds" as well as normal pg
				int32_t h32;
				h32 = sreq->m_siteHash32 ^ 0x123456;
				            m_siteIndexedDocumentCount.addScore(h32);
			}
			// unique votes per other for quota
			if ( uh48 == m_lastReqUh48b ) continue;
			// update this to ensure unique voting
			m_lastReqUh48b = uh48;
			// now count pages indexed below here
			if ( ! srep ) continue;
			if ( srepUh48 == m_lastRepUh48 ) continue;
			m_lastRepUh48 = srepUh48;
			//if ( ! srep ) continue;
			// TODO: what is srep->m_isIndexedINValid is set????
			if ( ! srep->m_isIndexed ) continue;
			// keep count per site and firstip
			m_siteIndexedDocumentCount.addScore(sreq->m_siteHash32,1);

			const int32_t *tmpNum = (const int32_t *)m_siteIndexedDocumentCount.getValue( &( sreq->m_siteHash32 ) );
			logDebug( g_conf.m_logDebugSpider, "spider: sitequota: got %" PRId32" indexed docs for site from "
			          "firstip of %s from url %s", tmpNum ? *tmpNum : -1,
			          iptoa(sreq->m_firstIp,ipbuf),
			          sreq->m_url );
			continue;
		}


		// if the spiderrequest has a fake firstip that means it
		// was injected without doing a proper ip lookup for speed.
		// xmldoc.cpp will check for m_fakeFirstIp and it that is
		// set in the spiderrequest it will simply add a new request
		// with the correct firstip. it will be a completely different
		// spiderrequest key then. so no need to keep the "fakes".
		// it will log the EFAKEFIRSTIP error msg.
		if ( sreq->m_fakeFirstIp && srep && srep->m_spideredTime > sreq->m_addedTime ) {
			logDebug( g_conf.m_logDebugSpider, "spider: skipping6 %s", sreq->m_url );
			continue;
		}

		if (!sreq->m_urlIsDocId) {
			//delete request if it is an url we don't want to index
			Url url;
			url.set(sreq->m_url);
			if (url.hasNonIndexableExtension(TITLEREC_CURRENT_VERSION) || isUrlBlocked(url,NULL)) {
				if (srep && !srep->m_isIndexedINValid && srep->m_isIndexed) {
					log(LOG_DEBUG, "Found unwanted/non-indexable URL '%s' in spiderdb. Force deleting it", sreq->m_url);
					sreq->m_forceDelete = true;
				} else {
					log(LOG_DEBUG, "Found unwanted/non-indexable URL '%s' in spiderdb. Deleting it", sreq->m_url);
					SpiderdbUtil::deleteRecord(m_collnum, sreq->m_firstIp, uh48);
					continue;
				}
			}
		}

		if ( sreq->isCorrupt() ) {
			if ( m_cr->m_spiderCorruptCount == 0 )
				log( LOG_WARN, "spider: got corrupt xx spiderRequest in scan because url is %s (cn=%" PRId32")",
					sreq->m_url,(int32_t)m_collnum);
			m_cr->m_spiderCorruptCount++;
			continue;
		}

		if ( sreq->m_dataSize > (int32_t)sizeof(SpiderRequest) ) {
			if ( m_cr->m_spiderCorruptCount == 0 )
				log( LOG_WARN, "spider: got corrupt 11 spiderRequest in scan because rectoobig u=%s (cn=%" PRId32")",
					sreq->m_url,(int32_t)m_collnum);
			m_cr->m_spiderCorruptCount++;
			continue;
		}

		int32_t delta = sreq->m_addedTime - nowGlobal;
		if ( delta > 86400 ) {
			static bool s_first = true;
			if ( m_cr->m_spiderCorruptCount == 0 || s_first ) {
				s_first = false;
				log( LOG_WARN, "spider: got corrupt 6 spiderRequest in scan because added time is %" PRId32" (delta=%" PRId32" which is well into the future. url=%s (cn=%i)",
					(int32_t)sreq->m_addedTime, delta, sreq->m_url, (int)m_collnum);
			}
			m_cr->m_spiderCorruptCount++;
			continue;
		}


		// update SpiderRequest::m_siteNumInlinks to most recent value
		int32_t sni = sreq->m_siteNumInlinks;
		{
			ScopedLock sl(m_sniTableMtx);

			// get the # of inlinks to the site from our table
			const uint64_t *val = (const uint64_t *)m_sniTable.getValue32(sreq->m_siteHash32);
			// use the most recent sni from this table
			if (val)
				sni = (int32_t)((*val) >> 32);
				// if SpiderRequest is forced then m_siteHash32 is 0!
			else if (srep && srep->m_spideredTime >= sreq->m_addedTime)
				sni = srep->m_siteNumInlinks;
		}
		// assign
		sreq->m_siteNumInlinks = sni;

		// store error count in request so xmldoc knows what it is
		// and can increment it and re-add it to its spiderreply if
		// it gets another error
		if ( srep ) {
			// . assign this too from latest reply - smart compress
			// . this WAS SpiderReply::m_pubdate so it might be
			//   set to a non-zero value that is wrong now... but
			//   not a big deal!
			sreq->m_contentHash32 = srep->m_contentHash32;
			// if we tried it before
			sreq->m_hadReply = true;
		}

		// . get the url filter we match
		// . if this is slow see the TODO below in dedupSpiderdbList()
		//   which can pre-store these values assuming url filters do
		//   not change and siteNumInlinks is about the same.
		int32_t ufn = ::getUrlFilterNum(sreq, srep, nowGlobal, false, m_cr, false, -1);
		// sanity check
		if ( ufn == -1 ) { 
			log( LOG_WARN, "failed to match url filter for url='%s' coll='%s'", sreq->m_url, m_cr->m_coll );
			g_errno = EBADENGINEER;
			return true;
		}
		// set the priority (might be the same as old)
		int32_t priority = m_cr->m_spiderPriorities[ufn];
		// now get rid of negative priorities since we added a
		// separate force delete checkbox in the url filters
		if ( priority < 0 ) priority = 0;
		if ( priority >= MAX_SPIDER_PRIORITIES) gbshutdownLogicError();

		logDebug( g_conf.m_logDebugSpider, "spider: got ufn=%" PRId32" for %s (%" PRId64")",
		          ufn, sreq->m_url, sreq->getUrlHash48() );

		if ( srep )
			logDebug(g_conf.m_logDebugSpider, "spider: lastspidered=%" PRIu32, srep->m_spideredTime );

		// spiders disabled for this row in url filteres?
		if ( m_cr->m_maxSpidersPerRule[ufn] <= 0 ) {
			continue;
		}

		// skip if banned (unless need to delete from index)
		if (m_cr->m_forceDelete[ufn]) {
			// but if it is currently indexed we have to delete it
			if (!(srep && srep->m_isIndexed)) {
				// so only skip if it's not indexed
				continue;
			}
		}

		if ( m_cr->m_forceDelete[ufn] ) {
			logDebug( g_conf.m_logDebugSpider, "spider: force delete ufn=%" PRId32" url='%s'", ufn, sreq->m_url );
			// force it to a delete
			sreq->m_forceDelete = true;
		}

		int64_t spiderTimeMS = getSpiderTimeMS(sreq, ufn, srep, nowGlobalMS);

		// sanity
		if ( (int64_t)spiderTimeMS < 0 ) { 
			log( LOG_WARN, "spider: got corrupt 2 spiderRequest in scan (cn=%" PRId32")",
			    (int32_t)m_collnum);
			continue;
		}

		// save this shit for storing in doledb
		sreq->m_ufn = ufn;
		sreq->m_priority = priority;

		// if it is in future, skip it and just set m_futureTime and
		// and we will update the waiting tree
		// with an entry based on that future time if the winnerTree 
		// turns out to be empty after we've completed our scan
		if ( spiderTimeMS > nowGlobalMS ) {
			// if futuretime is zero set it to this time
			if ( ! m_minFutureTimeMS ) 
				m_minFutureTimeMS = spiderTimeMS;
			// otherwise we get the MIN of all future times
			else if ( spiderTimeMS < m_minFutureTimeMS )
				m_minFutureTimeMS = spiderTimeMS;
			if ( g_conf.m_logDebugSpider )
				log(LOG_DEBUG,"spider: skippingx %s",sreq->m_url);
			continue;
		}


		// we can't have negative priorities at this point because
		// the s_ufnTree uses priority as part of the key so it
		// can get the top 100 or so urls for a firstip to avoid
		// having to hit spiderdb for every one!
		//if ( priority < 0 ) gbshutdownLogicError();

		// bail if it is locked! we now call 
		// msg12::confirmLockAcquisition() after we get the lock,
		// which deletes the doledb record from doledb and doleiptable
		// rightaway and adds a "0" entry into the waiting tree so
		// that evalIpLoop() repopulates doledb again with that
		// "firstIp". this way we can spider multiple urls from the
		// same ip at the same time.
		int64_t key = makeLockTableKey ( sreq );

		logDebug( g_conf.m_logDebugSpider, "spider: checking uh48=%" PRId64" lockkey=%" PRId64" used=%" PRId32,
		          uh48, key, g_spiderLoop.getLockCount() );

		// MDW
		if (g_spiderLoop.isLocked(key)) {
			logDebug( g_conf.m_logDebugSpider, "spider: skipping url lockkey=%" PRId64" in lock table sreq.url=%s",
			          key, sreq->m_url );
			continue;
		}

		// make key
		key192_t wk = makeWinnerTreeKey( firstIp ,
						 priority ,
						 spiderTimeMS ,
						 uh48 );

		// assume our added time is the first time this url was added
		sreq->m_discoveryTime = sreq->m_addedTime;

		// if this url is already in the winnerTree then either we 
		// replace it or we skip ourselves. 
		//
		// watch out for dups in winner tree, the same url can have 
		// multiple spiderTimeMses somehow...
		// as well, resulting in different priorities...
		// actually the dedup table could map to a priority and a node
		// so we can kick out a lower priority version of the same url.
		int32_t winSlot = m_winnerTable.getSlot ( &uh48 );
		if ( winSlot >= 0 ) {
			const key192_t *oldwk = (const key192_t *)m_winnerTable.getValueFromSlot ( winSlot );

			SpiderRequest *wsreq = (SpiderRequest *)m_winnerTree.getData(0,(const char *)oldwk);
			
			if ( wsreq ) {
				// and the min added time as well!
				// get the oldest timestamp so
				// gbssDiscoveryTime will be accurate.
				if ( sreq->m_discoveryTime < wsreq->m_discoveryTime )
					wsreq->m_discoveryTime = sreq->m_discoveryTime;
					
				if ( wsreq->m_discoveryTime < sreq->m_discoveryTime )
					sreq->m_discoveryTime = wsreq->m_discoveryTime;
			}

			

			// are we lower priority? (or equal)
			// smaller keys are HIGHER priority.
			if(KEYCMP( (const char *)&wk, (const char *)oldwk, sizeof(key192_t)) >= 0)
			{
				continue;
			}
				
			// from table too. no it's a dup uh48!
			//m_winnerTable.deleteKey ( &uh48 );
			// otherwise we supplant it. remove old key from tree.
			m_winnerTree.deleteNode ( 0 , (char *)oldwk , false);
			// supplant in table and tree... just add below...
		}

		// get the top 100 spider requests by priority/time/etc.
		int32_t maxWinners = (int32_t)MAX_WINNER_NODES; // 40


//@todo BR: Why max winners based on bytes scanned??
		// if less than 10MB of spiderdb requests limit to 400
		if ( m_totalBytesScanned < 10000000 ) maxWinners = 400;

		// only put one doledb record into winner tree if
		// the list is pretty short. otherwise, we end up caching
		// too much. granted, we only cache for about 2 mins.
		// mdw: for testing take this out!
		if ( m_totalBytesScanned < 25000 ) maxWinners = 1;

		// sanity. make sure read is somewhat hefty for our maxWinners=1 thing
		static_assert(SR_READ_SIZE >= 500000, "ensure read size is big enough");

		{
			ScopedLock sl(m_winnerTree.getLock());
			// only compare to min winner in tree if tree is full
			if (m_winnerTree.getNumUsedNodes_unlocked() >= maxWinners) {
				// get that key
				int64_t tm1 = spiderTimeMS;
				// get the spider time of lowest scoring req in tree
				int64_t tm2 = m_tailTimeMS;
				// if they are both overdue, make them the same
				if (tm1 < nowGlobalMS) tm1 = 1;
				if (tm2 < nowGlobalMS) tm2 = 1;
				// skip spider request if its time is past winner's
				if (tm1 > tm2)
					continue;
				if (tm1 < tm2)
					goto gotNewWinner;
				// if tied, use priority
				if (priority < m_tailPriority)
					continue;
				if (priority > m_tailPriority)
					goto gotNewWinner;
				// if tied, use actual times. assuming both<nowGlobalMS
				if (spiderTimeMS > m_tailTimeMS)
					continue;
				if (spiderTimeMS < m_tailTimeMS)
					goto gotNewWinner;
				// all tied, keep it the same i guess
				continue;
				// otherwise, add the new winner in and remove the old
gotNewWinner:
				// get lowest scoring node in tree
				int32_t tailNode = m_winnerTree.getLastNode_unlocked();
				// from table too
				m_winnerTable.removeKey(&m_tailUh48);
				// delete the tail so new spiderrequest can enter
				m_winnerTree.deleteNode_unlocked(tailNode, true);
			}
		}

		// somestimes the firstip in its key does not match the
		// firstip in the record!
		if ( sreq->m_firstIp != firstIp ) {
			log(LOG_ERROR,"spider: request %s firstip does not match "
			    "firstip in key collnum=%i",sreq->m_url,
			    (int)m_collnum);
			log(LOG_ERROR,"spider: ip1=%s",iptoa(sreq->m_firstIp,ipbuf));
			log(LOG_ERROR,"spider: ip2=%s",iptoa(firstIp,ipbuf));
			continue;
		}


		// . add to table which allows us to ensure same url not 
		//   repeated in tree
		// . just skip if fail to add...
		if ( ! m_winnerTable.addKey ( &uh48 , &wk ) ) {
			log(LOG_WARN,"spider: skipping. could not add to winnerTable. %s. ip=%s", sreq->m_url,iptoa(m_scanningIp,ipbuf) );
			continue;
		}

		// use an individually allocated buffer for each spiderrequest
		// so if it gets removed from tree the memory can be freed by 
		// the tree which "owns" the data because m_winnerTree.set() 
		// above set ownsData
		// to true above.
		int32_t need = sreq->getRecSize();
		char *newMem = (char *)mdup ( sreq , need , "sreqbuf" );
		if ( ! newMem ) {
			log(LOG_WARN,"spider: skipping. could not alloc newMem. %s. ip=%s", sreq->m_url,iptoa(m_scanningIp,ipbuf) );
			continue;
		}

		{
			ScopedLock sl(m_winnerTree.getLock());
			// add it to the tree of the top urls to spider
			m_winnerTree.addNode_unlocked(0, (char *)&wk, (char *)newMem, need);

			// set new tail priority and time for next compare
			if (m_winnerTree.getNumUsedNodes_unlocked() >= maxWinners) {
				// for the worst node in the tree...
				int32_t tailNode = m_winnerTree.getLastNode_unlocked();
				if (tailNode < 0) gbshutdownAbort(true);
				// set new tail parms
				const key192_t *tailKey = reinterpret_cast<const key192_t *>(m_winnerTree.getKey_unlocked(tailNode));
				// convert to char first then to signed int32_t
				parseWinnerTreeKey(tailKey, &m_tailIp, &m_tailPriority, &m_tailTimeMS, &m_tailUh48);

				// sanity
				if (m_tailIp != firstIp) {
					gbshutdownAbort(true);
				}
			}
		}
	}

	// if no spiderreply for the current url, invalidate this
	m_lastReplyValid = false;
	// if read is not yet done, save the reply in case next list needs it
	if ( srep ) { // && ! m_isReadDone ) {
		int32_t rsize = srep->getRecSize();
		if((size_t)rsize > sizeof(m_lastReplyBuf))
			gbshutdownAbort(true);
		gbmemcpy ( m_lastReplyBuf, srep, rsize );
		m_lastReplyValid = true;
	}

	logDebug(g_conf.m_logDebugSpider, "spider: Checked list of %" PRId32" spiderdb bytes (%" PRId32" recs) "
		    "for winners for firstip=%s. winnerTreeUsedNodes=%" PRId32" #newreqs=%" PRId64,
	         m_list.getListSize(), recCount,
	         iptoa(m_scanningIp,ipbuf), m_winnerTree.getNumUsedNodes(), m_totalNewSpiderRequests );

	// reset any errno cuz we're just a cache
	g_errno = 0;


	/////
	//
	// BEGIN maintain firstip overflow list
	//
	/////
	bool overflow = false;
	// don't add any more outlinks to this firstip after we
	// have 10M spider requests for it.
	// lower for testing
	//if ( m_totalNewSpiderRequests > 1 )
// @todo BR: Another hardcoded limit..	
	if ( m_totalNewSpiderRequests > 10000000 )
		overflow = true;

	// need space
	if ( overflow && ! m_overflowList ) {
		int32_t need = OVERFLOWLISTSIZE*4;
		m_overflowList = (int32_t *)mmalloc(need,"list");
		m_overflowList[0] = 0;
	}
	//
	// ensure firstip is in the overflow list if we overflowed
	int32_t emptySlot = -1;
	bool found = false;
	int32_t oi;
	// if we dealt with this last round, we're done
	if ( m_lastOverflowFirstIp == firstIp )
		return true;
	m_lastOverflowFirstIp = firstIp;
	if ( overflow ) {
		logDebug( g_conf.m_logDebugSpider, "spider: firstip %s overflowing with %" PRId32" new reqs",
		          iptoa(firstIp,ipbuf), (int32_t)m_totalNewSpiderRequests );
	}

	for ( oi = 0 ; ; oi++ ) {
		// sanity
		if ( ! m_overflowList ) break;
		// an ip of zero is end of the list
		if ( ! m_overflowList[oi] ) break;
		// if already in there, we are done
		if ( m_overflowList[oi] == firstIp ) {
			found = true;
			break;
		}
		// -1 means empty slot
		if ( m_overflowList[oi] == -1 ) emptySlot = oi;
	}
	// if we need to add it...
	if ( overflow && ! found && m_overflowList ) {
		log(LOG_DEBUG,"spider: adding %s to overflow list",iptoa(firstIp,ipbuf));
		// reset this little cache thingy
		s_lastOut = 0;
		// take the empty slot if there is one
		if ( emptySlot >= 0 )
			m_overflowList[emptySlot] = firstIp;
		// or add to new slot. this is #defined to 200 last check
		else if ( oi+1 < OVERFLOWLISTSIZE ) {
			m_overflowList[oi] = firstIp;
			m_overflowList[oi+1] = 0;
		}
		else 
			log(LOG_ERROR,"spider: could not add firstip %s to "
			    "overflow list, full.", iptoa(firstIp,ipbuf));
	}
	// ensure firstip is NOT in the overflow list if we are ok
	for ( int32_t oi2 = 0 ; ! overflow ; oi2++ ) {
		// sanity
		if ( ! m_overflowList ) break;
		// an ip of zero is end of the list
		if ( ! m_overflowList[oi2] ) break;
		// skip if not a match
		if ( m_overflowList[oi2] != firstIp ) continue;
		// take it out of list
		m_overflowList[oi2] = -1;
		log(LOG_DEBUG, "spider: removing %s from overflow list",iptoa(firstIp,ipbuf));
		// reset this little cache thingy
		s_lastIn = 0;
		break;
	}
	/////
	//
	// END maintain firstip overflow list
	//
	/////

	// ok we've updated m_bestRequest!!!
	return true;
}



// . this is ONLY CALLED from evalIpLoop() above
// . add another 0 entry into waiting tree, unless we had no winner
// . add winner in here into doledb
// . returns false if blocked and doledWrapper() will be called
// . returns true and sets g_errno on error
bool SpiderColl::addWinnersIntoDoledb ( ) {

	if ( g_errno ) {
		log(LOG_ERROR,"spider: got error when trying to add winner to doledb: "
		    "%s",mstrerror(g_errno));
		return true;
	}

	// gotta check this again since we might have done a QUICKPOLL() above
	// to call g_process.shutdown() so now tree might be unwritable
	if (g_process.isShuttingDown()) {
		return true;
	}

	// ok, all done if nothing to add to doledb. i guess we were misled
	// that firstIp had something ready for us. maybe the url filters
	// table changed to filter/ban them all. if a new request/reply comes 
	// in for this firstIp then it will re-add an entry to waitingtree and
	// we will re-scan spiderdb. if we had something to spider but it was 
	// in the future the m_minFutureTimeMS will be non-zero, and we deal
	// with that below...
	if (m_winnerTree.isEmpty() && ! m_minFutureTimeMS ) {
		// if we received new incoming requests while we were
		// scanning, which is happening for some crawls, then do
		// not nuke! just repeat later in populateDoledbFromWaitingTree
		if ( m_gotNewDataForScanningIp ) {
			if ( g_conf.m_logDebugSpider )
				log(LOG_DEBUG, "spider: received new requests, not "
				    "nuking misleading key");
			return true;
		}
		// note it - this can happen if no more to spider right now!
		char ipbuf[16];
		if ( g_conf.m_logDebugSpider )
			log(LOG_WARN, "spider: nuking misleading waitingtree key "
			    "firstIp=%s", iptoa(m_scanningIp,ipbuf));

		ScopedLock sl(m_waitingTree.getLock());

		m_waitingTree.deleteNode_unlocked(0, (char *)&m_waitingTreeKey, true);
		m_waitingTreeKeyValid = false;

		// note it
		uint64_t timestamp64 = m_waitingTreeKey.n1;
		timestamp64 <<= 32;
		timestamp64 |= m_waitingTreeKey.n0 >> 32;
		int32_t firstIp = m_waitingTreeKey.n0 &= 0xffffffff;
		if ( g_conf.m_logDebugSpider )
			log(LOG_DEBUG,"spider: removed2 time=%" PRId64" ip=%s from "
			    "waiting tree. nn=%" PRId32".",
			    timestamp64, iptoa(firstIp,ipbuf),
			    m_waitingTree.getNumUsedNodes_unlocked());

		removeFromWaitingTable(firstIp);
		return true;
	}

	// i've seen this happen, wtf?
	if (m_winnerTree.isEmpty() && m_minFutureTimeMS ) {
		// this will update the waiting tree key with minFutureTimeMS
		addDoleBufIntoDoledb ( NULL , false );
		return true;
	}

	///////////
	//
	// make winner tree into doledb list to add
	//
	///////////
	// first 4 bytes is offset of next doledb record to add to doledb
	// so we do not have to re-add the dolebuf to the cache and make it
	// churn. it is really inefficient.
	SafeBuf doleBuf;
	doleBuf.pushLong(4);

	// i am seeing dup uh48's in the m_winnerTree
	int32_t firstIp = m_waitingTreeKey.n0 & 0xffffffff;

	{
		ScopedLock sl(m_winnerTree.getLock());

		int32_t ntn = m_winnerTree.getNumNodes_unlocked();

		HashTableX dedup;
		dedup.set(8, 0, (int32_t)2 * ntn, NULL, 0, false, "windt");

		int32_t added = 0;
		for (int32_t node = m_winnerTree.getFirstNode_unlocked();
		     node >= 0; node = m_winnerTree.getNextNode_unlocked(node)) {
			// get data for that
			const SpiderRequest *sreq2 = reinterpret_cast<const SpiderRequest *>(m_winnerTree.getData_unlocked(node));

			// sanity
			if (sreq2->m_firstIp != firstIp) gbshutdownAbort(true);
			//if ( sreq2->m_spiderTimeMS < 0 ) gbshutdownAbort(true);
			if (sreq2->m_ufn < 0) gbshutdownAbort(true);
			if (sreq2->m_priority == -1) gbshutdownAbort(true);
			// check for errors
			bool hadError = false;
			// parse it up
			int32_t winIp;
			int32_t winPriority;
			int64_t winSpiderTimeMS;
			int64_t winUh48;
			const key192_t *winKey = reinterpret_cast<const key192_t *>(m_winnerTree.getKey_unlocked(node));
			parseWinnerTreeKey(winKey, &winIp, &winPriority, &winSpiderTimeMS, &winUh48);

			// sanity
			if (winIp != firstIp) gbshutdownAbort(true);
			if (winUh48 != sreq2->getUrlHash48()) gbshutdownAbort(true);

			// make the doledb key
			key96_t doleKey = Doledb::makeKey(winPriority, winSpiderTimeMS / 1000, winUh48, false);

			// dedup. if we add dups the problem is is that they
			// overwrite the key in doledb yet the doleiptable count
			// remains undecremented and doledb is empty and never
			// replenished because the firstip can not be added to
			// waitingTree because doleiptable count is > 0. this was
			// causing spiders to hang for collections. i am not sure
			// why we should be getting dups in winnertree because they
			// have the same uh48 and that is the key in the tree.
			if (dedup.isInTable(&winUh48)) {
				log(LOG_WARN, "spider: got dup uh48=%" PRIu64" dammit", winUh48);
				continue;
			}
			// count it
			added++;
			// do not allow dups
			dedup.addKey(&winUh48);
			// store doledb key first
			if (!doleBuf.safeMemcpy(&doleKey, sizeof(key96_t)))
				hadError = true;
			// then size of spiderrequest
			if (!doleBuf.pushLong(sreq2->getRecSize()))
				hadError = true;
			// then the spiderrequest encapsulated
			if (!doleBuf.safeMemcpy(sreq2, sreq2->getRecSize()))
				hadError = true;
			// note and error
			if (hadError) {
				log(LOG_ERROR,"spider: error making doledb list: %s",
				    mstrerror(g_errno));
				return true;
			}
		}
	}

	return addDoleBufIntoDoledb ( &doleBuf , false );
}



bool SpiderColl::validateDoleBuf(const SafeBuf *doleBuf) {
	const char *doleBufEnd = doleBuf->getBufPtr();
	// get offset
	const char *pstart = doleBuf->getBufStart();
	const char *p = pstart;
	int32_t jump = *(int32_t *)p;
	p += 4;
	// sanity
	if ( jump < 4 || jump > doleBuf->length() )
		gbshutdownCorrupted();
	bool gotIt = false;
	for ( ; p < doleBuf->getBufPtr() ; ) {
		if ( p == pstart + jump )
			gotIt = true;
		// first is doledbkey
		p += sizeof(key96_t);
		// then size of spider request
		int32_t recSize = *(int32_t *)p;
		p += 4;
		// the spider request encapsulated
		SpiderRequest *sreq3;
		sreq3 = (SpiderRequest *)p;
		// point "p" to next spiderrequest
		if ( recSize != sreq3->getRecSize() ) gbshutdownCorrupted();
		p += recSize;//sreq3->getRecSize();
		// sanity
		if ( p > doleBufEnd ) gbshutdownCorrupted();
		if ( p < pstart     ) gbshutdownCorrupted();
	}
	if ( ! gotIt ) gbshutdownCorrupted();
	return true;
}



bool SpiderColl::addDoleBufIntoDoledb ( SafeBuf *doleBuf, bool isFromCache ) {
	////////////////////
	//
	// UPDATE WAITING TREE ENTRY
	//
	// Normally the "spidertime" is 0 for a firstIp. This will make it
	// a future time if it is not yet due for spidering.
	//
	////////////////////

	int32_t firstIp = m_waitingTreeKey.n0 & 0xffffffff;

	char ipbuf[16]; //for various log purposes
	{
		ScopedLock sl(m_waitingTree.getLock());

		/// @todo ALC could we avoid locking winnerTree?
		ScopedLock sl2(m_winnerTree.getLock());

		// sanity check. how did this happen? it messes up our crawl!
		// maybe a doledb add went through? so we should add again?
		int32_t wn = m_waitingTree.getNode_unlocked(0, (char *)&m_waitingTreeKey);
		if (wn < 0) {
			log(LOG_ERROR,"spider: waiting tree key removed while reading list for "
				    "%s (%" PRId32")", m_coll, (int32_t)m_collnum);
			return true;
		}

		// if best request has a future spiderTime, at least update
		// the wait tree with that since we will not be doling this request
		// right now.
		if (m_winnerTree.isEmpty_unlocked() && m_minFutureTimeMS && !isFromCache) {
			// save memory
			m_winnerTree.reset_unlocked();
			m_winnerTable.reset();

			// if in the process of being added to doledb or in doledb...
			if (isInDoledbIpTable(firstIp)) {
				// sanity i guess. remove this line if it hits this!
				log(LOG_ERROR, "spider: wtf????");
				//gbshutdownLogicError();
				return true;
			}

			// before you set a time too far into the future, if we
			// did receive new spider requests, entertain those
			if (m_gotNewDataForScanningIp &&
			    // we had twitter.com with a future spider date
			    // on the pe2 cluster but we kept hitting this, so
			    // don't do this anymore if we scanned a ton of bytes
			    // like we did for twitter.com because it uses all the
			    // resources when we can like 150MB of spider requests
			    // for a single firstip
			    m_totalBytesScanned < 30000) {
				logDebug(g_conf.m_logDebugSpider, "spider: received new requests, not updating waiting tree with future time");
				return true;
			}

			// get old time
			uint64_t oldSpiderTimeMS = m_waitingTreeKey.n1;
			oldSpiderTimeMS <<= 32;
			oldSpiderTimeMS |= (m_waitingTreeKey.n0 >> 32);
			// delete old node
			if (wn >= 0) {
				m_waitingTree.deleteNode_unlocked(wn, false);
			}

			// invalidate
			m_waitingTreeKeyValid = false;
			key96_t wk2 = makeWaitingTreeKey(m_minFutureTimeMS, firstIp);

			logDebug(g_conf.m_logDebugSpider, "spider: scan replacing waitingtree key oldtime=%" PRIu32" newtime=%" PRIu32" firstip=%s",
					(uint32_t)(oldSpiderTimeMS / 1000LL), (uint32_t)(m_minFutureTimeMS / 1000LL), iptoa(firstIp,ipbuf));

			// this should never fail since we deleted one above
			m_waitingTree.addKey_unlocked(&wk2);

			logDebug(g_conf.m_logDebugSpider, "spider: RE-added time=%" PRId64" ip=%s to waiting tree node",
			         m_minFutureTimeMS, iptoa(firstIp,ipbuf));

			// keep the table in sync now with the time
			addToWaitingTable(firstIp, m_minFutureTimeMS);
			return true;
		}
	}

	char *doleBufEnd = doleBuf->getBufPtr();

	// add it to doledb ip table now so that waiting tree does not
	// immediately get another spider request from this same ip added
	// to it while the msg4 is out. but if add failes we totally bail
	// with g_errno set

	// . MDW: now we have a list of doledb records in a SafeBuf:
	// . scan the requests in safebuf

	// get offset
	char *p = doleBuf->getBufStart();
	int32_t jump = *(int32_t *)p;
	// sanity
	if ( jump < 4 || jump > doleBuf->length() ) {
		gbshutdownCorrupted(); }
	// the jump includes itself
	p += jump;
	//for ( ; p < m_doleBuf.getBuf() ; ) {
	// save it
	char *doledbRec = p;
	// first is doledbkey
	p += sizeof(key96_t);
	// then size of spider request
	p += 4;
	// the spider request encapsulated
	SpiderRequest *sreq3;
	sreq3 = (SpiderRequest *)p;
	// point "p" to next spiderrequest
	p += sreq3->getRecSize();

	// sanity
	if ( p > doleBufEnd ) gbshutdownCorrupted();

	// for caching logic below, set this
	int32_t doledbRecSize = sizeof(key96_t) + 4 + sreq3->getRecSize();
	// process sreq3 my incrementing the firstip count in 
	// m_doledbIpTable
	if ( !addToDoledbIpTable(sreq3) ) return true;

	// now cache the REST of the spider requests to speed up scanning.
	// better than adding 400 recs per firstip to doledb because
	// msg5's call to RdbTree::getList() is way faster.
	// even if m_doleBuf is from the cache, re-add it to lose the
	// top rec.
	// allow this to add a 0 length record otherwise we keep the same
	// old url in here and keep spidering it over and over again!

	// remove from cache? if we added the last spider request in the
	// cached dolebuf to doledb then remove it from cache so it's not
	// a cached empty dolebuf and we recompute it not using the cache.
	if ( p >= doleBufEnd ) {
		FxBlobCacheLock<int32_t> rcl(g_spiderLoop.m_winnerListCache);
		g_spiderLoop.m_winnerListCache.remove(firstIp);
	} else {
		// insert (or replace) the3 list in the cache
		char *x = doleBuf->getBufStart();
		// the new offset is the next record after the one we
		// just added to doledb
		int32_t newJump = (int32_t)(p - x);
		int32_t oldJump = *(int32_t *)x;
		// NO! we do a copy in rdbcache and copy the thing over
		// since we promote it. so this won't work...
		*(int32_t *)x = newJump;
		if ( newJump >= doleBuf->length() ) gbshutdownCorrupted();
		if ( newJump < 4 ) gbshutdownCorrupted();
		logDebug(g_conf.m_logDebugSpider, "spider: rdbcache: updating %" PRId32" bytes of SpiderRequests "
			"to winnerlistcache for ip %s oldjump=%" PRId32" newJump=%" PRId32" ptr=0x%" PTRFMT,
		         doleBuf->length(),iptoa(firstIp,ipbuf),oldJump, newJump, (PTRTYPE)x);
		//validateDoleBuf ( doleBuf );
		// inherit timestamp. if 0, RdbCache will set to current time
		// don't re-add just use the same modified buffer so we
		// don't churn the cache.
		// but do add it to cache if not already in there yet.
		FxBlobCacheLock<int32_t> rcl(g_spiderLoop.m_winnerListCache);
		g_spiderLoop.m_winnerListCache.insert(firstIp, doleBuf->getBufStart(), doleBuf->length());
	}

	// keep it on stack now that doledb is tree-only
	RdbList tmpList;
	tmpList.setFromPtr ( doledbRec , doledbRecSize , RDB_DOLEDB );

	// now that doledb is tree-only and never dumps to disk, just
	// add it directly
	g_doledb.getRdb()->addList(m_collnum, &tmpList);

	logDebug(g_conf.m_logDebugSpider, "spider: adding doledb tree node size=%" PRId32, doledbRecSize);

	int32_t storedFirstIp = (m_waitingTreeKey.n0) & 0xffffffff;

	// log it
	if ( g_conf.m_logDebugSpcache ) {
		uint64_t spiderTimeMS = m_waitingTreeKey.n1;
		spiderTimeMS <<= 32;
		spiderTimeMS |= (m_waitingTreeKey.n0 >> 32);
		logf(LOG_DEBUG,"spider: removing doled waitingtree key"
		     " spidertime=%" PRIu64" firstIp=%s "
		     ,spiderTimeMS,
		     iptoa(storedFirstIp,ipbuf)
		     );
	}

	{
		ScopedLock sl(m_waitingTree.getLock());

		// before adding to doledb remove from waiting tree so we do not try
		// to readd to doledb...
		m_waitingTree.deleteNode_unlocked(0, (char *)&m_waitingTreeKey, true);
		removeFromWaitingTable(storedFirstIp);
	}

	// invalidate
	m_waitingTreeKeyValid = false;

	// note that ip as being in dole table
	if ( g_conf.m_logDebugSpider )
		log(LOG_WARN, "spider: added best sreq for ip=%s to doletable AND "
		    "removed from waiting table",
		    iptoa(firstIp,ipbuf));

	// save memory
	m_winnerTree.reset();
	m_winnerTable.reset();

	//validateDoleBuf( doleBuf );

	// add did not block
	return true;
}


uint64_t SpiderColl::getSpiderTimeMS(const SpiderRequest *sreq, int32_t ufn, const SpiderReply *srep, int64_t nowMS) {
	// . get the scheduled spiderTime for it
	// . assume this SpiderRequest never been successfully spidered
	int64_t spiderTimeMS = ((uint64_t)sreq->m_addedTime) * 1000LL;

	// if injecting for first time, use that!
	if (!srep && sreq->m_isInjecting) {
		return spiderTimeMS;
	}

	if (sreq->m_isPageReindex) {
		int64_t nextReindexTimeMS = m_lastReindexTimeMS + m_cr->m_spiderReindexDelayMS;
		if (nextReindexTimeMS > nowMS) {
			return nextReindexTimeMS;
		}

		m_lastReindexTimeMS = nowMS;

		return nextReindexTimeMS;
	}

	// to avoid hammering an ip, get last time we spidered it...
	RdbCacheLock rcl(m_lastDownloadCache);
	int64_t lastMS = m_lastDownloadCache.getLongLong(m_collnum, sreq->m_firstIp, -1, true);
	rcl.unlock();

	// -1 means not found
	if ((int64_t)lastMS == -1) {
		lastMS = 0;
	}

	// sanity
	if ((int64_t)lastMS < -1) {
		log(LOG_ERROR,"spider: corrupt last time in download cache. nuking.");
		lastMS = 0;
	}

	// min time we can spider it
	int64_t minSpiderTimeMS1 = lastMS + m_cr->m_spiderIpWaits[ufn];

	/////////////////////////////////////////////////
	/////////////////////////////////////////////////
	// crawldelay table check!!!!
	/////////////////////////////////////////////////
	/////////////////////////////////////////////////

	int64_t minSpiderTimeMS2 = 0;

	{
		ScopedLock sl(m_cdTableMtx);
		int32_t *cdp = (int32_t *)m_cdTable.getValue(&sreq->m_domHash32);
		if (cdp && *cdp >= 0) minSpiderTimeMS2 = lastMS + *cdp;
	}

	//  ensure min
	if ( spiderTimeMS < minSpiderTimeMS1 ) spiderTimeMS = minSpiderTimeMS1;
	if ( spiderTimeMS < minSpiderTimeMS2 ) spiderTimeMS = minSpiderTimeMS2;

	// if no reply, use that
	if (!srep) {
		return spiderTimeMS;
	}

	// if this is not the first try, then re-compute the spiderTime
	// based on that last time
	// sanity check
	if ( srep->m_spideredTime <= 0 ) {
		// a lot of times these are corrupt! wtf???
		//spiderTimeMS = minSpiderTimeMS;
		return spiderTimeMS;
		//gbshutdownAbort(true);
	}
	// compute new spiderTime for this guy, in seconds
	int64_t waitInSecs = (uint64_t)(m_cr->m_spiderFreqs[ufn]*3600*24.0);

	// when it was spidered
	int64_t lastSpideredMS = ((uint64_t)srep->m_spideredTime) * 1000;
	// . when we last attempted to spider it... (base time)
	// . use a lastAttempt of 0 to indicate never! 
	// (first time)
	int64_t minSpiderTimeMS3 = lastSpideredMS + (waitInSecs * 1000LL);
	//  ensure min
	if ( spiderTimeMS < minSpiderTimeMS3 ) spiderTimeMS = minSpiderTimeMS3;
	// sanity
	if ( (int64_t)spiderTimeMS < 0 ) { gbshutdownAbort(true); }

	return spiderTimeMS;
}

// . decrement priority
// . will also set m_sc->m_nextDoledbKey
// . will also set m_sc->m_msg5StartKey
void SpiderColl::devancePriority() {
	// try next
	m_pri2 = m_pri2 - 1;
	// how can this happen?
	if ( m_pri2 < -1 ) m_pri2 = -1;
	// bogus?
	if ( m_pri2 < 0 ) return;
	// set to next priority otherwise
	m_nextDoledbKey = m_nextKeys [m_pri2];
	// and the read key
	m_msg5StartKey = m_nextDoledbKey;
}



void SpiderColl::setPriority(int32_t pri) {
	m_pri2 = pri;
	m_nextDoledbKey = m_nextKeys [ m_pri2 ];
	m_msg5StartKey = m_nextDoledbKey;
}

bool SpiderColl::tryToDeleteSpiderColl ( SpiderColl *sc , const char *msg ) {
	// if not being deleted return false
	if ( ! sc->m_deleteMyself ) return false;
	// otherwise always return true
	if ( sc->m_isLoading ) {
		log(LOG_INFO, "spider: deleting sc=0x%" PTRFMT" for collnum=%" PRId32" "
			    "waiting3",
		    (PTRTYPE)sc,(int32_t)sc->m_collnum);
		return true;
	}
	// if ( sc->m_gettingWaitingTreeList ) {
	// 	log(LOG_INFO, "spider: deleting sc=0x%" PTRFMT" for collnum=%" PRId32"
	//"waiting6",
	// 	    (int32_t)sc,(int32_t)sc->m_collnum);
	// 	return true;
	// }
	// there's still a core of someone trying to write to someting
	// in "sc" so we have to try to fix that. somewhere in xmldoc.cpp
	// or spider.cpp. everyone should get sc from cr everytime i'd think
	log(LOG_INFO, "spider: deleting sc=0x%" PTRFMT" for collnum=%" PRId32" (msg=%s)",
	    (PTRTYPE)sc,(int32_t)sc->m_collnum,msg);
	// . make sure nobody has it
	// . cr might be NULL because Collectiondb.cpp::deleteRec2() might
	//   have nuked it
	//CollectionRec *cr = sc->m_cr;
	// use fake ptrs for easier debugging
	//if ( cr ) cr->m_spiderColl = (SpiderColl *)0x987654;//NULL;
	mdelete ( sc , sizeof(SpiderColl),"postdel1");
	delete ( sc );
	return true;
}


// . returns false with g_errno set on error
// . Rdb.cpp should call this when it receives a doledb key
// . when trying to add a SpiderRequest to the waiting tree we first check
//   the doledb table to see if doledb already has an sreq from this firstIp
// . therefore, we should add the ip to the dole table before we launch the
//   Msg4 request to add it to doledb, that way we don't add a bunch from the
//   same firstIP to doledb
bool SpiderColl::addToDoledbIpTable(const SpiderRequest *sreq) {
	ScopedLock sl(m_doledbIpTableMtx);

	// update how many per ip we got doled
	int32_t *score = (int32_t *)m_doledbIpTable.getValue32 ( sreq->m_firstIp );
	// debug point
	if ( g_conf.m_logDebugSpider ){//&&1==2 ) { // disable for now, spammy
		int64_t  uh48 = sreq->getUrlHash48();
		int64_t pdocid = sreq->getParentDocId();
		int32_t ss = 1;
		if ( score ) ss = *score + 1;
		// if for some reason this collides with another key
		// already in doledb then our counts are off
		char ipbuf[16];
		log(LOG_DEBUG, "spider: added to doletbl uh48=%" PRIu64" parentdocid=%" PRIu64" "
			    "ipdolecount=%" PRId32" ufn=%" PRId32" priority=%" PRId32" firstip=%s",
		    uh48,pdocid,ss,(int32_t)sreq->m_ufn,(int32_t)sreq->m_priority,
		    iptoa(sreq->m_firstIp,ipbuf));
	}


	// we had a score there already, so inc it
	if ( score ) {
		// inc it
		*score = *score + 1;
		// sanity check
		if ( *score <= 0 ) { gbshutdownAbort(true); }
		// only one per ip!
		// not any more! we allow MAX_WINNER_NODES per ip!
		char ipbuf[16];
		if ( *score > MAX_WINNER_NODES )
			log(LOG_ERROR,"spider: crap. had %" PRId32" recs in doledb for %s "
				    "from %s."
				    "how did this happen?",
			    (int32_t)*score,m_coll,iptoa(sreq->m_firstIp,ipbuf));
		// now we log it too
		if ( g_conf.m_logDebugSpider )
			log(LOG_DEBUG,"spider: added ip=%s to doleiptable "
				    "(score=%" PRId32")",
			    iptoa(sreq->m_firstIp,ipbuf),*score);
	}
	else {
		// ok, add new slot
		int32_t val = 1;
		char ipbuf[16];
		if ( ! m_doledbIpTable.addKey ( &sreq->m_firstIp , &val ) ) {
			// log it, this is bad
			log(LOG_ERROR,"spider: failed to add ip %s to dole ip tbl",
			    iptoa(sreq->m_firstIp,ipbuf));
			// return true with g_errno set on error
			return false;
		}
		// now we log it too
		if ( g_conf.m_logDebugSpider )
			log(LOG_DEBUG,"spider: added ip=%s to doleiptable "
				"(score=1)",iptoa(sreq->m_firstIp,ipbuf));
	}

	// . these priority slots in doledb are not empty
	// . unmark individual priority buckets
	// . do not skip them when scanning for urls to spiderd
	int32_t pri = sreq->m_priority;

	// reset scan for this priority in doledb
	m_nextKeys[pri] = Doledb::makeFirstKey2 ( pri );

	return true;
}

void SpiderColl::removeFromDoledbIpTable(int32_t firstIp) {
	ScopedLock sl(m_doledbIpTableMtx);

	// . decrement doledb table ip count for firstIp
	// . update how many per ip we got doled
	int32_t *score = (int32_t *)m_doledbIpTable.getValue32 ( firstIp );

	// wtf! how did this spider without being doled?
	if ( ! score ) {
		//if ( ! srep->m_fromInjectionRequest )
		char ipbuf[16];
		log(LOG_ERROR, "spider: corruption. received spider reply whose "
			    "ip has no entry in dole ip table. firstip=%s",
		    iptoa(firstIp,ipbuf));
		return;
	}

	// reduce it
	*score = *score - 1;

	// now we log it too
	if ( g_conf.m_logDebugSpider ) {
		char ipbuf[16];
		log(LOG_DEBUG,"spider: removed ip=%s from doleiptable (newcount=%" PRId32")",
		    iptoa(firstIp,ipbuf), *score);
	}


	// remove if zero
	if ( *score == 0 ) {
		// this can file if writes are disabled on this hashtablex
		// because it is saving
		m_doledbIpTable.removeKey ( &firstIp );
	}
	// wtf!
	if ( *score < 0 ) { gbshutdownAbort(true); }
	// all done?
	if ( g_conf.m_logDebugSpider ) {
		// log that too!
		char ipbuf[16];
		logf(LOG_DEBUG,"spider: discounting firstip=%s to %" PRId32,
		     iptoa(firstIp,ipbuf),*score);
	}
}

int32_t SpiderColl::getDoledbIpTableCount() const {
	ScopedLock sl(m_doledbIpTableMtx);
	return m_doledbIpTable.getNumUsedSlots();
}

bool SpiderColl::isInDoledbIpTable(int32_t firstIp) const {
	ScopedLock sl(m_doledbIpTableMtx);
	return m_doledbIpTable.isInTable(&firstIp);
}

bool SpiderColl::isDoledbIpTableEmpty() const {
	ScopedLock sl(m_doledbIpTableMtx);
	return m_doledbIpTable.isEmpty();
}

void SpiderColl::clearDoledbIpTable() {
	ScopedLock sl(m_doledbIpTableMtx);
	m_doledbIpTable.clear();
}

std::vector<uint32_t> SpiderColl::getDoledbIpTable() const {
	ScopedLock sl(m_doledbIpTableMtx);
	std::vector<uint32_t> r;
	r.reserve(m_doledbIpTable.getNumUsedSlots());
	for(int slotNumber=0; slotNumber<m_doledbIpTable.getNumSlots(); slotNumber++) {
		if(!m_doledbIpTable.isEmpty(slotNumber)) {
			r.push_back(*(const uint32_t*)m_doledbIpTable.getKeyFromSlot(slotNumber));
		}
	}
	return r;
}

bool SpiderColl::addToWaitingTable(int32_t firstIp, int64_t timeMs) {
	ScopedLock sl(m_waitingTableMtx);
	return m_waitingTable.addKey(&firstIp, &timeMs);
}

bool SpiderColl::getFromWaitingTable(int32_t firstIp, int64_t *timeMs) {
	ScopedLock sl(m_waitingTableMtx);

	int32_t ws = m_waitingTable.getSlot(&firstIp);
	if (ws < 0) {
		return false;
	}

	*timeMs = m_waitingTable.getScore64FromSlot(ws);
	return true;
}

void SpiderColl::removeFromWaitingTable(int32_t firstIp) {
	ScopedLock sl(m_waitingTableMtx);
	m_waitingTable.removeKey(&firstIp);
}

int32_t SpiderColl::getWaitingTableCount() const {
	ScopedLock sl(m_waitingTableMtx);
	return m_waitingTable.getNumUsedSlots();
}

bool SpiderColl::isInWaitingTable(int32_t firstIp) const {
	ScopedLock sl(m_waitingTableMtx);
	return m_waitingTable.isInTable(&firstIp);
}

bool SpiderColl::setWaitingTableSize(int32_t numSlots) {
	ScopedLock sl(m_waitingTableMtx);
	return m_waitingTable.setTableSize(numSlots, NULL, 0);
}

void SpiderColl::clearWaitingTable() {
	ScopedLock sl(m_waitingTableMtx);
	m_waitingTable.clear();
}
