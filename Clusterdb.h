// Copyright Matt Wells, Jul 2002

// . a clusterRec now no longer exists, per se
// . it is the same thing as the key of the titleRec in titledb
// . titleRecs now contain the site and content hashes in the low bits
//   of their key. 
// . this allows us to store much cluster info in Titledb's RdbMap
// . so to get cluster info, just read in the titleRec, you do not even
//   need to uncompress it, just get the info from its key
// . we still use the cache here, however, to cache the keys (clusterRecs)
// . later, i may have to do some fancy footwork if we want to store all
//   clusterRecs (titleKeys) in memory.
// . TODO: what if stored file offsets in tfndb, too, then titledb RdbMap
//   would not be necessary?
//
// . clusterdb will now serve to help do fast site clustering by retaining
//   docids and site hashes in memory
//
//   00000000 00000000 0000000d dddddddd  d = docid
//   dddddddd dddddddd dddddddd dddddfll  f = family filter bit
//   llllssss ssssssss ssssssss sssssshz
//                                        l = language bits
//   					  s = site hash
//   					  h = half bit
//   					  z = del bit

#ifndef GB_CLUSTERDB_H
#define GB_CLUSTERDB_H

#include "Rdb.h"

// these are now just TitleRec keys
#define CLUSTER_REC_SIZE (sizeof(key96_t))

class Clusterdb {
public:

	// reset rdb
	void reset();
	
	// set up our private rdb
	bool init ( );

	// init the rebuild/secondary rdb, used by PageRepair.cpp
	bool init2 ( int32_t treeMem );

	//bool addColl ( const char *coll, bool doVerify = true );

	Rdb *      getRdb()       { return &m_rdb; }
	const Rdb *getRdb() const { return &m_rdb; }

	// make the cluster rec key
	static key96_t makeClusterRecKey(int64_t     docId,
				       bool        familyFilter,
				       uint8_t     languageBits,
				       int32_t     siteHash,
				       bool        isDelKey,
				       bool        isHalfKey = false );

	static key96_t makeFirstClusterRecKey(int64_t docId) {
		return makeClusterRecKey ( docId, false, 0, 0, true ); }
	static key96_t makeLastClusterRecKey(int64_t docId) {
		return makeClusterRecKey ( docId, true, 0xff, 0xffffffff,
					   false, true ); }

	// NOTE: THESE NOW USE THE REAL CLUSTERDB REC
	// docId occupies the most significant bytes of the key
	// now docId occupies the bits after the first 23
	static int64_t getDocId ( const key96_t *k ) {
		int64_t docId = (k->n0) >> 35;
		docId |= ( ((uint64_t)(k->n1)) << 29 );
		return docId;
	}

	static uint32_t getSiteHash26 ( const key96_t *r ) {
		return (((uint64_t)r->n0) >> 2) & 0x03FFFFFF;
	}

	static uint32_t hasAdultContent ( const key96_t *r ) {
		return (((uint64_t)r->n0) >> 34) & 0x00000001;
	}

	static unsigned char getLanguage ( const key96_t *r ) {
		return (((uint64_t)r->n0) >> 28) & 0x0000003F;
	}

	static char getFamilyFilter ( const key96_t *r ) {
		if (((uint64_t)r->n0) & 0x0000000400000000LL ) return 1;
		return 0;
	}

private:
	// this rdb holds urls waiting to be spidered or being spidered
	Rdb m_rdb;
};

extern class Clusterdb g_clusterdb;
extern class Clusterdb g_clusterdb2;

#endif // GB_CLUSTERDB_H
