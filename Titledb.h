// Matt Wells, copyright Jun 2001

// . db of XmlDocs

#ifndef GB_TITLEDB_H
#define GB_TITLEDB_H

// how many bits is our docId? (4billion * 64 = 256 billion docs)
#define NUMDOCIDBITS 38
#define DOCID_MASK   (0x0000003fffffffffLL)
#define MAX_DOCID    DOCID_MASK

#include "TitleRecVersion.h"
#include "Rdb.h"
#include "Url.h"
#include "hash.h"

// new key format:
// . <docId>     - 38 bits
// . <urlHash48> - 48 bits  (used when looking up by url and not docid)
//   <reserved>  -  9 bits
// . <delBit>    -  1 bit

// dddddddd dddddddd dddddddd dddddddd      d: docId
// dddddduu uuuuuuuu uuuuuuuu uuuuuuuu      u: urlHash48
// uuuuuuuu uuuuuuuu uuuuuuxx xxxxxxxD      D: delBit

class Titledb {
public:
	// reset rdb
	void reset();

	bool verify(const char *coll);

	// init m_rdb
	bool init ();

	// init secondary/rebuild titledb
	bool init2 ( int32_t treeMem ) ;

	Rdb       *getRdb()       { return &m_rdb; }
	const Rdb *getRdb() const { return &m_rdb; }

	// . this is an estimate of the number of docs in the WHOLE db network
	// . we assume each group/cluster has about the same # of docs as us
	int64_t estimateGlobalNumDocs() const {
		return m_rdb.getNumTotalRecs() * (int64_t)g_hostdb.m_numShards;
	}

	// . the top NUMDOCIDBITs of "key" are the docId
	// . we use the top X bits of the keys to partition the records
	// . using the top bits to partition allows us to keep keys that
	//   are near each other (euclidean metric) in the same partition
	static int64_t getDocIdFromKey(const key96_t *key) {
		uint64_t docId = ((uint64_t)key->n1) << (NUMDOCIDBITS - 32);
		docId |= key->n0 >> (64 - (NUMDOCIDBITS - 32));
		return docId;
	}

	static int64_t getDocId(const key96_t *key) { return getDocIdFromKey(key); }

	static int64_t getUrlHash48 ( key96_t *k ) {
		return ((k->n0 >> 10) & 0x0000ffffffffffffLL);
	}

	// does this key/docId/url have it's titleRec stored locally?
	static bool isLocal(int64_t docId);

	// . make the key of a TitleRec from a docId
	// . remember to set the low bit so it's not a delete
	// . hi bits are set in the key
	static key96_t makeKey(int64_t docId, int64_t uh48, bool isDel);

	static key96_t makeFirstKey(int64_t docId) {
		return makeKey(docId, 0, true);
	}

	static key96_t makeLastKey(int64_t docId) {
		return makeKey(docId, 0xffffffffffffLL, false);
	}

	static void printKey(const char *key);
	static void validateSerializedRecord(const char *rec, int32_t recSize);

	// Rdb init variables
	static inline int32_t getFixedDataSize() { return -1; }
	static inline bool getUseHalfKeys() { return false; }
	static inline char getKeySize() { return 12; }

private:
	// holds binary format title entries
	Rdb m_rdb;
};

void filterTitledbList(RdbList *list);

extern class Titledb g_titledb;
extern class Titledb g_titledb2;

#endif // GB_TITLEDB_H
