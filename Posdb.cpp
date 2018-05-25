#include "Posdb.h"
#include "JobScheduler.h"
#include "Rebalance.h"
#include "RdbCache.h"
#include "Conf.h"
#include "Sanity.h"

#ifdef _VALGRIND_
#include <valgrind/memcheck.h>
#endif



// a global class extern'd in .h file
Posdb g_posdb;

// for rebuilding posdb
Posdb g_posdb2;



// resets rdb
void Posdb::reset() { 
	m_rdb.reset();
}

bool Posdb::init ( ) {
	// sanity check
	key144_t k;
	int64_t termId = 123456789LL;
	uint64_t docId = 34567292222LL;
	int32_t dist = MAXWORDPOS-1;//54415;
	int32_t densityRank = 10;
	int32_t diversityRank = MAXDIVERSITYRANK-1;//11;
	int32_t wordSpamRank = MAXWORDSPAMRANK-1;//12;
	int32_t siteRank = 13;
	int32_t hashGroup = 1;
	int32_t langId = 59;
	int32_t multiplier = 13;
	char shardedByTermId = 1;
	char isSynonym = 1;
	Posdb::makeKey ( &k ,
			  termId ,
			  docId,
			  dist,
			  densityRank , // 0-15
			  diversityRank,
			  wordSpamRank,
			  siteRank,
			  hashGroup ,
			  langId,
			  multiplier,
			  isSynonym , // syn?
			  false , // delkey?
			  shardedByTermId );
	// test it out
	if ( Posdb::getTermId ( &k ) != termId ) gbshutdownLogicError();
	//int64_t d2 = Posdb::getDocId(&k);
	if ( Posdb::getDocId (&k ) != docId ) gbshutdownLogicError();
	if ( Posdb::getHashGroup ( &k ) !=hashGroup) gbshutdownLogicError();
	if ( Posdb::getWordPos ( &k ) !=  dist ) gbshutdownLogicError();
	if ( Posdb::getDensityRank (&k)!=densityRank)gbshutdownLogicError();
	if ( Posdb::getDiversityRank(&k)!=diversityRank)gbshutdownLogicError();
	if ( Posdb::getWordSpamRank(&k)!=wordSpamRank)gbshutdownLogicError();
	if ( Posdb::getSiteRank (&k) != siteRank ) gbshutdownLogicError();
	if ( Posdb::getLangId ( &k ) != langId ) gbshutdownLogicError();
	if ( Posdb::getMultiplier ( &k ) !=multiplier)gbshutdownLogicError();
	if ( Posdb::getIsSynonym ( &k ) != isSynonym) gbshutdownLogicError();
	if ( Posdb::isShardedByTermId(&k)!=shardedByTermId)gbshutdownLogicError();
	// more tests
	setDocIdBits ( &k, docId );
	setMultiplierBits ( &k, multiplier );
	setSiteRankBits ( &k, siteRank );
	setLangIdBits ( &k, langId );
	// test it out
	if ( Posdb::getTermId ( &k ) != termId ) gbshutdownLogicError();
	if ( Posdb::getDocId (&k ) != docId ) gbshutdownLogicError();
	if ( Posdb::getWordPos ( &k ) !=  dist ) gbshutdownLogicError();
	if ( Posdb::getDensityRank (&k)!=densityRank)gbshutdownLogicError();
	if ( Posdb::getDiversityRank(&k)!=diversityRank)gbshutdownLogicError();
	if ( Posdb::getWordSpamRank(&k)!=wordSpamRank)gbshutdownLogicError();
	if ( Posdb::getSiteRank (&k) != siteRank ) gbshutdownLogicError();
	if ( Posdb::getHashGroup ( &k ) !=hashGroup) gbshutdownLogicError();
	if ( Posdb::getLangId ( &k ) != langId ) gbshutdownLogicError();
	if ( Posdb::getMultiplier ( &k ) !=multiplier)gbshutdownLogicError();
	if ( Posdb::getIsSynonym ( &k ) != isSynonym) gbshutdownLogicError();

	/*
	// more tests
	key144_t sk;
	key144_t ek;
	Posdb::makeStartKey(&sk,termId);
	Posdb::makeEndKey  (&ek,termId);

	RdbList list;
	list.set(NULL,0,NULL,0,0,true,true,18);
	key144_t ka;
	ka.n2 = 0x1234567890987654ULL;
	ka.n1 = 0x5566778899aabbccULL;
	ka.n0 = (uint16_t)0xbaf1;
	list.addRecord ( (char *)&ka,0,NULL,true );
	key144_t kb;
	kb.n2 = 0x1234567890987654ULL;
	kb.n1 = 0x5566778899aabbccULL;
	kb.n0 = (uint16_t)0xeef1;
	list.addRecord ( (char *)&kb,0,NULL,true );

	char *p = list.m_list;
	char *pend = p + list.m_listSize;
	for ( ; p < pend ; p++ )
		log("db: %02" PRId32") 0x%02" PRIx32,p-list.m_list,
		    (int32_t)(*(unsigned char *)p));
	list.resetListPtr();
	list.checkList_r(false,RDB_POSDB);
	gbshutdownLogicError();
	*/

	// make it lower now for debugging
	//maxTreeMem = 5000000;
	// . what's max # of tree nodes?
	// . each rec in tree is only 1 key (12 bytes)
	// . but has 12 bytes of tree overhead (m_left/m_right/m_parents)
	// . this is UNUSED for bin trees!!
	int32_t nodeSize      = (sizeof(key144_t)+12+4) + sizeof(collnum_t);
	int32_t maxTreeNodes = g_conf.m_posdbMaxTreeMem / nodeSize ;

	// . set our own internal rdb
	// . max disk space for bin tree is same as maxTreeMem so that we
	//   must be able to fit all bins in memory
	// . we do not want posdb's bin tree to ever hit disk since we
	//   dump it to rdb files when it is 90% full (90% of bins in use)
	return m_rdb.init ( "posdb",
	                    getFixedDataSize(),
	                    // -1 means look in CollectionRec::m_posdbMinFilesToMerge
	                    -1,
	                    g_conf.m_posdbMaxTreeMem,
	                    maxTreeNodes                ,
	                    getUseHalfKeys(),
			            getKeySize(),
			            true);
}

// init the rebuild/secondary rdb, used by PageRepair.cpp
bool Posdb::init2 ( int32_t treeMem ) {
	//if ( ! setGroupIdTable () ) return false;
	// . what's max # of tree nodes?
	// . each rec in tree is only 1 key (12 bytes)
	// . but has 12 bytes of tree overhead (m_left/m_right/m_parents)
	// . this is UNUSED for bin trees!!
	int32_t nodeSize     = (sizeof(key144_t)+12+4) + sizeof(collnum_t);
	int32_t maxTreeNodes = treeMem  / nodeSize ;
	// . set our own internal rdb
	// . max disk space for bin tree is same as maxTreeMem so that we
	//   must be able to fit all bins in memory
	// . we do not want posdb's bin tree to ever hit disk since we
	//   dump it to rdb files when it is 90% full (90% of bins in use)
	return m_rdb.init("posdbRebuild",
	                  getFixedDataSize(),
	                  1000, // min files to merge
	                  treeMem,
	                  maxTreeNodes,
	                  getUseHalfKeys(),
	                  getKeySize(),
	                  true); //useIndex
}

// . see Posdb.h for format of the 12 byte key
// . TODO: substitute var ptrs if you want extra speed
void Posdb::makeKey ( void              *vkp            ,
		      int64_t          termId         ,
		      uint64_t docId          , 
		      int32_t               wordPos        ,
		      char               densityRank    ,
		      char               diversityRank  ,
		      char               wordSpamRank   ,
		      char               siteRank       ,
		      char               hashGroup      ,
		      char               langId         ,
		      int32_t               multiplier     ,
		      bool               isSynonym      ,
		      bool               isDelKey       ,
		      bool shardedByTermId ) {

	// sanity
	if ( siteRank      > MAXSITERANK      ) gbshutdownLogicError();
	if ( wordSpamRank  > MAXWORDSPAMRANK  ) gbshutdownLogicError();
	if ( densityRank   > MAXDENSITYRANK   ) gbshutdownLogicError();
	if ( diversityRank > MAXDIVERSITYRANK ) gbshutdownLogicError();
	if ( langId        > MAXLANGID        ) gbshutdownLogicError();
	if ( hashGroup     > MAXHASHGROUP     ) gbshutdownLogicError();
	if ( wordPos       > MAXWORDPOS       ) gbshutdownLogicError();
	if ( multiplier    > MAXMULTIPLIER    ) gbshutdownLogicError();

	key144_t *kp = (key144_t *)vkp;

	// make sure we mask out the hi bits we do not use first
	termId = termId & TERMID_MASK;
	kp->n2 = termId;
	// then 16 bits of docid
	kp->n2 <<= 16;
	kp->n2 |= docId >> (38-16); // 22

	// rest of docid (22 bits)
	kp->n1 = docId & (0x3fffff);
	// a zero bit for aiding b-stepping alignment issues
	kp->n1 <<= 1;
	kp->n1 |= 0x00;
	// 4 site rank bits
	kp->n1 <<= 4;
	kp->n1 |= siteRank;
	// 4 langid bits
	kp->n1 <<= 5;
	kp->n1 |= (langId & 0x1f);
	// the word position, 18 bits
	kp->n1 <<= 18;
	kp->n1 |= wordPos;
	// the hash group, 4 bits
	kp->n1 <<= 4;
	kp->n1 |= hashGroup;
	// the word span rank, 4 bits
	kp->n1 <<= 4;
	kp->n1 |= wordSpamRank;
	// the diversity rank, 4 bits
	kp->n1 <<= 4;
	kp->n1 |= diversityRank;
	// word form bits, F-bits. right now just use 1 bit
	kp->n1 <<= 2;
	if ( isSynonym ) kp->n1 |= 0x01;

	// density rank, 5 bits
	kp->n0 = densityRank;
	// is in outlink text? reserved
	kp->n0 <<= 1;
	// a 1 bit for aiding b-stepping
	kp->n0 <<= 1;
	kp->n0 |= 0x01;
	// multiplier bits, 5 bits
	kp->n0 <<= 5;
	kp->n0 |= multiplier;
	// one maverick langid bit, the 6th bit
	kp->n0 <<= 1;
	if ( langId & 0x20 ) kp->n0 |= 0x01;
	// compression bits, 2 of 'em
	kp->n0 <<= 2;
	// delbit
	kp->n0 <<= 1;
	if ( ! isDelKey ) kp->n0 |= 0x01;

	if ( shardedByTermId ) setShardedByTermIdBit ( kp );

	// get the one we lost
	// char *kstr = KEYSTR ( kp , sizeof(posdbkey_t) );
	// if (!strcmp(kstr,"0x0ca3417544e400000000000032b96bf8aa01"))
	// 	log("got lost key");
}

RdbCache g_termFreqCache;
RdbCache g_termListSize;
static bool s_cacheInit = false;


static void initializeCaches() {
	if ( ! s_cacheInit ) {
		int32_t maxMem = 5000000; // 5MB now... save mem (was: 20000000)
		int32_t maxNodes = maxMem / 17; // 8+8+1
		if( ! g_termFreqCache.init ( maxMem   , // maxmem 20MB
					     8        , // fixed data size
					     maxNodes ,
					     "tfcache", // dbname
					     false    , // load from disk?
					     8        , // cache key size
					     -1))       // numPtrsMax
			log("posdb: failed to init termfreqcache: %s",
			    mstrerror(g_errno));
		if(!g_termListSize.init(maxMem   , // maxmem 20MB
					8        , // fixed data size
					maxNodes ,
					"tscache", // dbname
					false    , // load from disk?
					8        , // cache key size
		                        -1))       // numPtrsMax
			log("posdb: failed to init termlistsizecache: %s",
			    mstrerror(g_errno));
		// ignore errors
		g_errno = 0;
		s_cacheInit = true;
	}
}


// . accesses RdbMap to estimate size of the indexList for this termId
// . returns an UPPER BOUND
// . because this is over POSDB now and not indexdb, a document is counted
//   once for every occurence of term "termId" it has... :{
int64_t Posdb::getTermFreq ( collnum_t collnum, int64_t termId ) {
	initializeCaches();
	
	// . check cache for super speed
	// . colnum is 0 for now
	RdbCacheLock rcl(g_termFreqCache); //todo: we should really release the lock while scanning the posdb-freq
	int64_t val = g_termFreqCache.getLongLong2 ( collnum ,
						       termId  , // key
						       500   , // maxage secs
						       true    );// promote?



	// -1 means not found in cache. if found, return it though.
	if ( val >= 0 ) {
		//log("posdb: got %" PRId64" in cache",val);
		return val;
	}

	// . ask rdb for an upper bound on this list size
	// . but actually, it will be somewhat of an estimate 'cuz of RdbTree
	// establish the list boundary keys
	key144_t startKey;
	key144_t endKey;
	key144_t maxKey;
	makeStartKey(&startKey, termId);
	makeEndKey  (&endKey  , termId);

	int64_t maxRecs = m_rdb.estimateListSize(collnum,
						 (const char*)&startKey,
						 (const char*)&endKey,
						 (char *)&maxKey,
						 -1 ); //no truncation

	RdbBuckets *buckets = m_rdb.getBuckets();
	if( !buckets ) {
		log(LOG_LOGIC, "%s:%s:%d: No buckets!", __FILE__, __func__, __LINE__);
		gbshutdownLogicError();
	}

	int64_t numBytes = buckets->estimateListSize(collnum, (const char *)&startKey, (const char *)&endKey, NULL, NULL);

	// convert from size in bytes to # of recs
	maxRecs += numBytes / sizeof(posdbkey_t);

	// and assume each shard has about the same #
	maxRecs *= g_hostdb.m_numShards;

	// now cache it. it sets g_errno to zero.
	g_termFreqCache.addLongLong2 ( collnum, termId, maxRecs );
	// return it
	return maxRecs;
}


int64_t Posdb::estimateLocalTermListSize(collnum_t collnum, int64_t termId) {
	initializeCaches();
	
	// . check cache for super speed
	// . colnum is 0 for now
	RdbCacheLock rcl(g_termListSize); //todo: we should really release the lock while scanning the posdb-freq
	int64_t val = g_termListSize.getLongLong2(collnum,
						  termId,  // key
						  500,     // maxage secs
						  true);   // promote?



	// -1 means not found in cache. if found, return it though.
	if(val>=0) {
		//log("posdb: got %" PRId64" in cache",val);
		return val;
	}

	// . ask rdb for an upper bound on this list size
	// . but actually, it will be somewhat of an estimate 'cuz of RdbTree
	// establish the list boundary keys
	key144_t startKey;
	key144_t endKey;
	key144_t maxKey;
	makeStartKey(&startKey, termId);
	makeEndKey  (&endKey  , termId);

	int64_t maxBytes = m_rdb.estimateListSize(collnum,
						  (const char*)&startKey,
						  (const char*)&endKey,
						  (char *)&maxKey,
						  -1); //no truncation

	RdbBuckets *buckets = m_rdb.getBuckets();
	if(!buckets) {
		log(LOG_LOGIC, "%s:%s:%d: No buckets!", __FILE__, __func__, __LINE__);
		gbshutdownLogicError();
	}

	int64_t bucketsBytes = buckets->estimateListSize(collnum, (const char *)&startKey, (const char *)&endKey, NULL, NULL);

	maxBytes += bucketsBytes;

	// now cache it. it sets g_errno to zero.
	g_termListSize.addLongLong2(collnum, termId, maxBytes);

	return maxBytes;
}


const char *getHashGroupString ( unsigned char hg ) {
	if ( hg == HASHGROUP_BODY ) return "body";
	if ( hg == HASHGROUP_TITLE ) return "title";
	if ( hg == HASHGROUP_HEADING ) return "header";
	if ( hg == HASHGROUP_INLIST ) return "in list";
	if ( hg == HASHGROUP_INMETATAG ) return "meta tag";
	//if ( hg == HASHGROUP_INLINKTEXT ) return "offsite inlink text";
	if ( hg == HASHGROUP_INLINKTEXT ) return "inlink text";
	if ( hg == HASHGROUP_INTAG ) return "tag";
	if ( hg == HASHGROUP_NEIGHBORHOOD ) return "neighborhood";
	if ( hg == HASHGROUP_INTERNALINLINKTEXT) return "onsite inlink text";
	if ( hg == HASHGROUP_INURL ) return "in url";
	if ( hg == HASHGROUP_INMENU ) return "in menu";
	if ( hg == HASHGROUP_EXPLICIT_KEYWORDS ) return "in explicit-keywords";
	if ( hg == HASHGROUP_LEMMA ) return "in lemma";
	return "unknown!";
}

void Posdb::printKey(const char *k) {
	logf(LOG_TRACE, "k=%s "
			     "tid=%015" PRIu64" "
			     "docId=%012" PRId64" "
			     "siteRank=%02" PRId32" "
			     "langId=%02" PRId32" "
			     "pos=%06" PRId32" "
			     "hgrp=%02" PRId32" "
			     "spamRank=%02" PRId32" "
			     "divRank=%02" PRId32" "
			     "syn=%01" PRId32" "
			     "densRank=%02" PRId32" "
			     "mult=%02" PRId32" "
			     "shardByTermId=%d "
			     "isDel=%d",
	     KEYSTR(k, sizeof(key144_t)),
	     getTermId(k),
	     getDocId(k),
	     (int32_t)getSiteRank(k),
	     (int32_t)getLangId(k),
	     getWordPos(k),
	     (int32_t)getHashGroup(k),
	     (int32_t)getWordSpamRank(k),
	     (int32_t)getDiversityRank(k),
	     (int32_t)getIsSynonym(k),
	     (int32_t)getDensityRank(k),
	     (int32_t)getMultiplier(k),
	     isShardedByTermId(k),
	     KEYNEG(k));
}

int Posdb::printList ( RdbList &list ) {
	posdbkey_t lastKey;
	// loop over entries in list
	for ( list.resetListPtr(); ! list.isExhausted(); list.skipCurrentRecord() ) {
		key144_t k; list.getCurrentKey(&k);
		// compare to last
		const char *err = "";
		if ( KEYCMP((char *)&k,(char *)&lastKey,sizeof(key144_t))<0 ) 
			err = " (out of order)";
		lastKey = k;
		// is it a delete?
		const char *dd = "";
		if ( (k.n0 & 0x01) == 0x00 ) dd = " (delete)";
		int64_t d = Posdb::getDocId(&k);
		uint8_t dh = Titledb::getDomHash8FromDocId(d);
		char *rec = list.getCurrentRec();
		int32_t recSize = 18;
		if ( rec[0] & 0x04 ) recSize = 6;
		else if ( rec[0] & 0x02 ) recSize = 12;
		// alignment bits check
		if ( recSize == 6  && !(rec[1] & 0x02) ) {
			int64_t nd1 = Posdb::getDocId(rec+6);
			// seems like nd2 is it, so it really is 12 bytes but
			// does not have the alignment bit set...
			//int64_t nd2 = Posdb::getDocId(rec+12);
			//int64_t nd3 = Posdb::getDocId(rec+18);
			// what size is it really?
			// seems like 12 bytes
			//log("debug1: d=%" PRId64" nd1=%" PRId64" nd2=%" PRId64" nd3=%" PRId64,
			//d,nd1,nd2,nd3);
			err = " (alignerror1)";
			if ( nd1 < d ) err = " (alignordererror1)";
			//g_process.shutdownAbort(true);
		}
		if ( recSize == 12 && !(rec[1] & 0x02) )  {
			//int64_t nd1 = Posdb::getDocId(rec+6);
			// seems like nd2 is it, so it really is 12 bytes but
			// does not have the alignment bit set...
			int64_t nd2 = Posdb::getDocId(rec+12);
			//int64_t nd3 = Posdb::getDocId(rec+18);
			// what size is it really?
			// seems like 12 bytes
			//log("debug1: d=%" PRId64" nd1=%" PRId64" nd2=%" PRId64" nd3=%" PRId64,
			//d,nd1,nd2,nd3);
			//if ( nd2 < d ) gbshutdownLogicError();
			//g_process.shutdownAbort(true);
			err = " (alignerror2)";
			if ( nd2 < d ) err = " (alignorderrror2)";
		}
		// if it 
		if ( recSize == 12 &&  (rec[7] & 0x02)) { 
			//int64_t nd1 = Posdb::getDocId(rec+6);
			// seems like nd2 is it, so it really is 12 bytes but
			// does not have the alignment bit set...
			int64_t nd2 = Posdb::getDocId(rec+12);
			//int64_t nd3 = Posdb::getDocId(rec+18);
			// what size is it really?
			// seems like 12 bytes really as well!
			//log("debug2: d=%" PRId64" nd1=%" PRId64" nd2=%" PRId64" nd3=%" PRId64,
			//d,nd1,nd2,nd3);
			//g_process.shutdownAbort(true);
			err = " (alignerror3)";
			if ( nd2 < d ) err = " (alignordererror3)";
		}

		log(
		       "k=%s "
		       "tid=%015" PRIu64" "
		       "docId=%012" PRId64" "

		       "siterank=%02" PRId32" "
		       "langid=%02" PRId32" "
		       "pos=%06" PRId32" "
		       "hgrp=%02" PRId32" "
		       "spamrank=%02" PRId32" "
		       "divrank=%02" PRId32" "
		       "syn=%01" PRId32" "
		       "densrank=%02" PRId32" "
		       "mult=%02" PRId32" "

		       "dh=0x%02" PRIx32" "
		       "rs=%" PRId32 //recSize
		       "%s" // dd
		       "%s" // err
		       "\n" ,
		       KEYSTR(&k,sizeof(key144_t)),
		       (int64_t)Posdb::getTermId(&k),
		       d ,
		       (int32_t)Posdb::getSiteRank(&k),
		       (int32_t)Posdb::getLangId(&k),
		       (int32_t)Posdb::getWordPos(&k),
		       (int32_t)Posdb::getHashGroup(&k),
		       (int32_t)Posdb::getWordSpamRank(&k),
		       (int32_t)Posdb::getDiversityRank(&k),
		       (int32_t)Posdb::getIsSynonym(&k),
		       (int32_t)Posdb::getDensityRank(&k),
		       (int32_t)Posdb::getMultiplier(&k),
		       (int32_t)dh,
		       recSize,
		       dd ,
		       err );
	}

	// startKey = *(key144_t *)list.getLastKey();
	// startKey += (uint32_t) 1;
	// // watch out for wrap around
	// if ( startKey < *(key144_t *)list.getLastKey() ) return;
	// goto loop;
	return 1;
}
