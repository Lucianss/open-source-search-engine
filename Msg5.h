// Matt Wells, copyright Sep 2001

// . get a lists from tree, cache and disk

#ifndef GB_MSG5_H
#define GB_MSG5_H

#include "Msg3.h"
#include "RdbList.h"
#include "JobScheduler.h" //job_exit_t
#include "rdbid_t.h"
#include "GbSignature.h"


extern int32_t g_numCorrupt;

extern bool g_isDumpingRdbFromMain;


class Msg2;


class Msg5 { 

 public:

	Msg5();

	~Msg5();

	// . set "list" with the asked for records
	// . returns false if blocked, true otherwise
	// . sets errno on error
	// . RdbList will be populated with the records you want
	// . we pass ourselves back to callback since requestHandler()s of
	//   various msg classes often do not have any context/state, and they
	//   construct us in a C wrapper so they need to delete us when 
	//   they're done with us
	// . if maxCacheAge is > 0, we lookup in cache first
	bool getList ( rdbid_t       rdbId,
		       collnum_t collnum ,
		       RdbList   *list          ,
		       const void      *startKey      ,
		       const void      *endKey        ,
		       int32_t       recSizes      , // requestd scan size(-1 all)
		       bool       includeTree   ,
		       int32_t       startFileNum  , // first file to scan
		       int32_t       numFiles      , // rel.to startFileNum,-1 all
		       void      *state         , // for callback
		       void (* callback ) ( void *state, RdbList *list, Msg5 *msg5 ),
		       int32_t       niceness      ,
		       bool       doErrorCorrection  ,
		       int32_t       maxRetries,
		       bool          isRealMerge);

	bool getSingleUnmergedList(rdbid_t       rdbId,
				   collnum_t     collnum,
				   RdbList      *list,
				   const void   *startKey,
				   const void   *endKey,
				   int32_t       recSizes, // requested scan size(-1 all)
				   int32_t       fileNum, // file to scan
				   void         *state, // for callback
				   void        (*callback)(void *state, RdbList *list, Msg5 *msg5),
				   int32_t       niceness);

	bool getTreeList(RdbList *result, rdbid_t rdbId, collnum_t collnum, const void *startKey, const void *endKey);
	bool getTreeList(RdbList *result, const void *startKey, const void *endKey, int32_t *numPositiveRecs, 
		int32_t *numNegativeRecs, int32_t *memUsedByTree, int32_t *numUsedNodes);

	// frees m_treeList, m_diskList (can be quite a lot of mem 2+ megs)
	void reset();

	bool isWaitingForList() const { return m_waitingForList; }

	int32_t minRecSizes() const { return m_minRecSizes; }

	declare_signature

	// we add our m_finalList(s) to this, the user's list
	RdbList  *m_list;

	// private:

	// holds all RdbLists from disk
	Msg3      m_msg3;

private:
	// holds list parms
	char      m_startKey[MAX_KEY_BYTES];
	char      m_endKey[MAX_KEY_BYTES];

	// called to read lists from disk using Msg3
	bool readList();

	// keep public for doneScanningWrapper to call
	bool gotList();
	bool gotList2();

	// does readList() need to be called again due to negative rec
	// annihilation?
	bool needsRecall();

	bool doneMerging   ();

	// . when a list is bad we try to patch it by getting a list from
	//   a host in our redundancy group
	// . we also get a local list from ALL files and tree to remove
	//   recs we already have from the remote list
	bool getRemoteList  ( );
	bool gotRemoteList  ( );

	// hold the caller of getList()'s callback here
	void    (* m_callback )( void *state , RdbList *list , Msg5 *msg );
	void    *m_state       ;
	char     m_calledCallback;

	// holds list from tree
	RdbList   m_treeList;

	RdbList   m_dummy;

	bool      m_includeTree;

	int32_t      m_numFiles;
	int32_t      m_numSources;
	int32_t      m_startFileNum;
	int32_t      m_minRecSizes;
	rdbid_t      m_rdbId;

	// . cache may modify these
	// . gotLists() may modify these before reading more
	char      m_fileStartKey[MAX_KEY_BYTES];
	int32_t      m_newMinRecSizes;
	int32_t      m_round;
	int32_t      m_totalSize;
	bool      m_readAbsolutelyNothing;

	int32_t      m_niceness;
	// error correction stuff
	bool      m_doErrorCorrection;
	bool      m_hadCorruption;
	class Msg0 *m_msg0;
	
	// for timing debug
	int64_t m_startTime;

	// hold pointers to lists to merge
	RdbList  *m_listPtrs [ MAX_RDB_FILES + 1 ]; // plus tree
	int32_t      m_numListPtrs;

	bool m_removeNegRecs;

	char m_minEndKey[MAX_KEY_BYTES];

	// info for truncating and passing to RdbList::indexMerge_r()
	char  m_prevKey[MAX_KEY_BYTES];

	int32_t  m_oldListSize;
	
	int32_t m_maxRetries;

	bool        m_isRealMerge;

	char m_ks;

	bool m_waitingForList;
	collnum_t m_collnum;

	int32_t m_errno;

	bool m_isSingleUnmergedListGet;

	static void gotListWrapper0(void *state);
	void gotListWrapper();
	
	static void mergeDoneWrapper(void *state, job_exit_t exit_type);
	void mergeDone(job_exit_t exit_type);
	static void gotRemoteListWrapper(void *state);
	
	static void mergeListsWrapper(void *state);

	void repairLists();
	void mergeLists();
};

#endif // GB_MSG5_H
