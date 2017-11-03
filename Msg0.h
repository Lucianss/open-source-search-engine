// Matt Wells, copyright Feb 2001

// . get a list from any rdb (hostmap,tagdb,termdb,titledb,quotadb)
// . TODO: support concatenation of lists from different groups

#ifndef GB_MSG0_H
#define GB_MSG0_H

#include "Multicast.h"
#include "Hostdb.h"
#include "RdbList.h"
#include "rdbid_t.h"

#define MSG0RDBIDOFFSET (8+4+4+4+4)

#define MSG0_REQ_SIZE (8 + 2 * MAX_KEY_BYTES + 16 + 5 + 4 + 1 + 1 )

static const int64_t msg0_getlist_infinite_timeout = 999999999999;

class Msg5;
class UdpSlot;


class Msg0 {

 public:

	Msg0  ( ) ;
	~Msg0 ( ) ;
	void reset ( ) ;
	void constructor ( );

	// . this should only be called once
	// . should also register our get record handlers with the udpServer
	static bool registerHandler();

	// . returns false if blocked, true otherwise
	// . sets errno on error
	// . "list" should NOT be on the stack in case we block
	// . caches "not founds" as well
	// . rdbIds: 0=hostdb,1=tagdb,2=termdb,3=titledb,4=quotadb
	// . rdbIds: see getRdb() below
	// . set niceness to 0 for highest priority, default is lowest priority
	// . maxCacheAge is the max cached list age in our NETWORK cache
	// . our disk cache gets flushed when the disk is updated so it's never
	//   out of sync with the data
	// . a maxCacheAge of 0 (or negative) means not to check the cache
	bool getList ( int64_t hostId      , // -1 if unspecified
		       rdbid_t   rdbId       , // specifies the rdb
		       collnum_t collnum ,
		       class RdbList  *list  ,
		       const char     *startKey    ,
		       const char     *endKey      ,
		       int32_t      minRecSizes ,  // Positive values only
		       void     *state       ,
		       void  (* callback)(void *state ),
		       int32_t      niceness    ,
		       bool      doErrorCorrection  ,
		       bool      includeTree        ,
		       int32_t      firstHostId          ,
		       int32_t      startFileNum        ,
		       int32_t      numFiles            ,
		       int64_t      timeout             ,
		       bool        isRealMerge     , // file merge?
		       bool        noSplit           , // MDW ????
		       int32_t        forceParitySplit     );

private:
	static void gotListWrapper2(void *state, RdbList *list, Msg5 *msg5);
	static void gotSingleReplyWrapper(void *state, UdpSlot *slot);
	static void gotMulticastReplyWrapper0(void *state, void *state2);
	void gotReply   ( char *reply , int32_t replySize , int32_t replyMaxSize );

	// callback info
	void    (*m_callback ) ( void *state );//, class RdbList *list );
	void     *m_state;      

	// host we sent RdbList request to 
	int64_t m_hostId;
	uint32_t  m_shardNum;

	// 2*4 + 1 + 2 * keySize
	char      m_request [ MSG0_REQ_SIZE ];
	int32_t      m_requestSize;

	// casting to multiple splits is obsolete, but for PageIndexdb.cpp
	// we still need to do it, but we alloc for it
	Multicast  m_mcast;

	int32_t      m_numRequests;
	int32_t      m_numReplies;
	int32_t      m_errno;
	// local reply, need to handle it for splitting
	char     *m_replyBuf;
	int32_t      m_replyBufSize;

	// ptr to passed list we're to fill
	class RdbList  *m_list;

	// . local rdb as specified by m_rdbId and gotten by getRdb(char rdbId)
	// . also, it's fixedDataSize
	//class Rdb      *m_rdb;
	int32_t      m_fixedDataSize;
	bool      m_useHalfKeys;

	// . parameters that define the RdbList we want
	// . we use precisely this block to define a network request 
	char  m_startKey[MAX_KEY_BYTES];
	char  m_endKey[MAX_KEY_BYTES];
	int32_t  m_minRecSizes ;
	rdbid_t  m_rdbId;
	collnum_t m_collnum;

	class Msg5  *m_msg5 ;
	bool         m_isRealMerge;

	// for timing the get
	int64_t m_startTime;

	// and for reporting niceness
	int32_t m_niceness;

	char m_ks;
};

#endif // GB_MSG0_H
