
#include "Titledb.h"
#include "Collectiondb.h"
#include "JobScheduler.h"
#include "Rebalance.h"
#include "Process.h"
#include "Conf.h"
#include "XmlDoc.h"
#include "UrlBlockCheck.h"

Titledb g_titledb;
Titledb g_titledb2;

// reset rdb
void Titledb::reset() { m_rdb.reset(); }

// init our rdb
bool Titledb::init ( ) {

	// key sanity tests
	int64_t uh48  = 0x1234567887654321LL & 0x0000ffffffffffffLL;
	int64_t docId = 123456789;
	key96_t k = makeKey(docId,uh48,false);
	if ( getDocId(&k) != docId ) { g_process.shutdownAbort(true);}
	if ( getUrlHash48(&k) != uh48 ) { g_process.shutdownAbort(true);}

	const char *url = "http://.ezinemark.com/int32_t-island-child-custody-attorneys-new-york-visitation-lawyers-melville-legal-custody-law-firm-45f00bbed18.html";
	Url uu;
	uu.set(url);
	const char *d1 = uu.getDomain();
	int32_t  dlen1 = uu.getDomainLen();
	int32_t dlen2 = 0;
	const char *d2 = getDomFast ( url , &dlen2 );
	if ( !d1 || !d2 ) { g_process.shutdownAbort(true); }
	if ( dlen1 != dlen2 ) { g_process.shutdownAbort(true); }

	// another one
	url = "http://ok/";
	uu.set(url);
	const char *d1a = uu.getDomain();
	dlen1 = uu.getDomainLen();
	dlen2 = 0;
	const char *d2a = getDomFast ( url , &dlen2 );
	if ( d1a || d2a ) { g_process.shutdownAbort(true); }
	if ( dlen1 != dlen2 ) { g_process.shutdownAbort(true); }

	// . what's max # of tree nodes?
	// . assume avg TitleRec size (compressed html doc) is about 1k we get:
	// . NOTE: overhead is about 32 bytes per node
	int32_t maxTreeNodes  = g_conf.m_titledbMaxTreeMem / (1*1024);

	// initialize our own internal rdb
	return m_rdb.init("titledb",
	                  getFixedDataSize(),
	                  //g_conf.m_titledbMinFilesToMerge ,
	                  // this should not really be changed...
	                  -1,
	                  g_conf.m_titledbMaxTreeMem,
	                  maxTreeNodes,
	                  getUseHalfKeys(),
	                  getKeySize(),
	                  false);         //useIndexFile
}

// init the rebuild/secondary rdb, used by PageRepair.cpp
bool Titledb::init2 ( int32_t treeMem ) {
	// . what's max # of tree nodes?
	// . assume avg TitleRec size (compressed html doc) is about 1k we get:
	// . NOTE: overhead is about 32 bytes per node
	int32_t maxTreeNodes  = treeMem / (1*1024);
	// initialize our own internal rdb
	return m_rdb.init("titledbRebuild",
	                  getFixedDataSize(),
	                  240, // MinFilesToMerge
	                  treeMem,
	                  maxTreeNodes,
	                  getUseHalfKeys(),
	                  getKeySize(),
	                  false);         //useIndexFile
}

bool Titledb::verify(const char *coll) {
	log ( LOG_DEBUG, "db: Verifying Titledb for coll %s...", coll );

	Msg5 msg5;
	RdbList list;
	key96_t startKey;
	key96_t endKey;
	startKey.setMin();
	endKey.setMax();
	const CollectionRec *cr = g_collectiondb.getRec(coll);

	if ( ! msg5.getList ( RDB_TITLEDB   ,
			      cr->m_collnum       ,
			      &list         ,
			      &startKey     ,
			      &endKey       ,
			      1024*1024     , // minRecSizes   ,
			      true          , // includeTree   ,
			      0             , // startFileNum  ,
			      -1            , // numFiles      ,
			      NULL          , // state
			      NULL          , // callback
			      0             , // niceness
			      false         , // err correction?
			      -1            , // maxRetries
			      false))          // isRealMerge
	{
		log(LOG_DEBUG, "db: HEY! it did not block");
		return false;
	}

	int32_t count = 0;
	int32_t got   = 0;
	for ( list.resetListPtr() ; ! list.isExhausted() ;
	      list.skipCurrentRecord() ) {
		key96_t k = list.getCurrentKey();
		// skip negative keys
		if ( (k.n0 & 0x01) == 0x00 ) continue;
		count++;
		uint32_t shardNum = getShardNum ( RDB_TITLEDB, &k );
		if ( shardNum == getMyShardNum() ) got++;
	}
	if ( got != count ) {
		// tally it up
		g_rebalance.m_numForeignRecs += count - got;
		log ("db: Out of first %" PRId32" records in titledb, "
		     "only %" PRId32" belong to our shard. c=%s",count,got,coll);
		// exit if NONE, we probably got the wrong data
		if ( count > 10 && got == 0 ) 
			log("db: Are you sure you have the right "
				   "data in the right directory? "
			    "coll=%s "
			    "Exiting.",
			    coll);
		// repeat with log
		for ( list.resetListPtr() ; ! list.isExhausted() ;
		      list.skipCurrentRecord() ) {
			key96_t k = list.getCurrentKey();
			int32_t shardNum = getShardNum ( RDB_TITLEDB, &k );
			log("db: docid=%" PRId64" shard=%" PRId32,
			    getDocId(&k),shardNum);
		}

		// don't exit any more, allow it, but do not delete
		// recs that belong to different shards when we merge now!
		log ( "db: db shards unbalanced. "
		      "Click autoscale in master controls.");
		//return false;
		return true;
	}

	log ( LOG_DEBUG, "db: Titledb passed verification successfully for %" PRId32
			" recs.", count );
	// DONE
	return true;
}

bool Titledb::isLocal ( int64_t docId ) {
	return ( getShardNumFromDocId(docId) == getMyShardNum() );
}

// . make the key of a TitleRec from a docId
// . remember to set the low bit so it's not a delete
// . hi bits are set in the key
key96_t Titledb::makeKey ( int64_t docId, int64_t uh48, bool isDel ){
	key96_t key ;
	key.n1 = (uint32_t)(docId >> 6); // (NUMDOCIDBITS-32));

	int64_t n0 = (uint64_t)(docId&0x3f);
	// sanity check
	if ( uh48 & 0xffff000000000000LL ) { g_process.shutdownAbort(true); }
	// make room for uh48
	n0 <<= 48;
	n0 |= uh48;
	// 9 bits reserved
	n0 <<= 9;
	// final del bit
	n0 <<= 1;
	if ( ! isDel ) n0 |= 0x01;
	// store it
	key.n0 = n0;
	return key;
};

void Titledb::printKey(const char *k) {
	logf(LOG_TRACE, "k=%s "
		     "docId=%012" PRId64" "
		     "urlHash48=%02" PRId64" "
		     "isDel=%d",
	     KEYSTR(k, sizeof(key96_t)),
	     getDocId((key96_t*)k),
	     getUrlHash48((key96_t*)k),
	     KEYNEG(k));
}

void Titledb::validateSerializedRecord(const char *rec, int32_t recSize) {
	char *debugp = (char*)rec;
	key96_t debug_titleRecKey =  *(key96_t *)debugp;
	bool debug_keyneg = (debug_titleRecKey.n0 & 0x01) == 0x00;
	int64_t debug_docId = Titledb::getDocIdFromKey(&debug_titleRecKey);
	
	if ( debug_keyneg ) {
		logTrace(g_conf.m_logTraceTitledb, "TitleDB rec verified. Delete key for DocId=%" PRId64 "", debug_docId);
	}
	else {
		debugp += sizeof(key96_t);

		// the size of the data that follows
		int32_t debug_dataSize =  *(int32_t *) debugp;
		if( debug_dataSize < 4 ) {
			log(LOG_ERROR, "TITLEDB CORRUPTION. Record shows size of %" PRId32" which is too small. DocId=%" PRId64 "", debug_dataSize, debug_docId);
			gbshutdownLogicError();
		}
		debugp += 4;

		// what's the size of the uncompressed compressed stuff below here?
		int32_t debug_ubufSize = *(int32_t  *) debugp; 
		if( debug_ubufSize <= 0 ) {
			log(LOG_ERROR, "TITLEDB CORRUPTION. Record shows uncompressed size of %" PRId32". DocId=%" PRId64 "", debug_ubufSize, debug_docId);
			gbshutdownLogicError();
		}
		logTrace(g_conf.m_logTraceTitledb, "TitleDB rec verified. recSize %" PRId32 ", uncompressed %" PRId32". DocId=%" PRId64 "", recSize, debug_ubufSize, debug_docId);
	}

}



void filterTitledbList(RdbList *list) {
	char *newList = list->getList();
	char *dst = newList;
	char *lastKey = NULL;

	int32_t oldSize = list->getListSize();
	int32_t filteredCount = 0;
	for (list->resetListPtr(); !list->isExhausted();) {
		char *rec = list->getCurrentRec();
		int32_t  recSize = list->getCurrentRecSize();

		// pre skip it (necessary because we manipulate the raw list below)
		list->skipCurrentRecord();

		if (!KEYNEG(rec)) {
			XmlDoc xd;
			if (xd.set2(rec, recSize, "main", 0)) {
				if (isUrlBlocked(*(xd.getFirstUrl()))) {
					++filteredCount;
					continue;
				}
			}
		}

		lastKey = dst;
		memmove(dst, rec, recSize);
		dst += recSize;
	}

	// sanity check
	if ( dst < list->getList() || dst > list->getListEnd() ) {
		g_process.shutdownAbort(true);
	}

	// and stick our newly filtered list in there
	list->setListSize(dst - newList);
	// set to end i guess
	list->setListEnd(list->getList() + list->getListSize());
	list->setListPtr(dst);
	list->setListPtrHi(NULL);

	log(LOG_DEBUG, "db: filtered %" PRId32" entries of %" PRId32 " bytes out of %" PRId32 " bytes.",
	    filteredCount, oldSize - list->getListSize(), oldSize);

	if( !lastKey ) {
		logError("lastKey is null. Should not happen?");
	} else {
		list->setLastKey(lastKey);
	}
}
