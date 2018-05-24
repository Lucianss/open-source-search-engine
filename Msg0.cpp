#include "gb-include.h"

#include "Msg0.h"
#include "Conf.h"
#include "Clusterdb.h"
#include "Collectiondb.h"
#include "Stats.h"
#include "Tagdb.h"
#include "Posdb.h"
#include "Titledb.h"
#include "Spider.h"
#include "Linkdb.h"
#include "Msg5.h"                 // local getList()
#include "UdpSlot.h"
#include "UdpServer.h"
#include "XmlDoc.h"
#include "Process.h"
#include "ip.h"
#include "Mem.h"
#include "JobScheduler.h"
#include "SpiderdbRdbSqliteBridge.h"


class State00;

static void handleRequest0           ( UdpSlot *slot , int32_t niceness ) ;
static void gotListWrapper           ( void *state, RdbList *list, Msg5 *msg5);
static void doneSending_ass          ( void *state , UdpSlot *slot ) ;

static void handleSpiderdbRequest(State00 *state);
static void execSpiderdbRequest(void *pv);
static void doneSpiderdbRequest(void *state, job_exit_t exit_type);

static void handleLocalSpiderdbGetList(collnum_t collnum, const char *startKey, const char *endKey, int32_t minRecSizes, RdbList *list);


Msg0::Msg0 ( ) {
	constructor();
}

void Msg0::constructor ( ) {
	m_msg5  = NULL;
	m_mcast.constructor();
	m_numRequests = 0;
	m_numReplies  = 0;
	m_errno       = 0;
	// reply buf
	m_replyBuf     = NULL;
	m_replyBufSize = 0;

	// Coverity
	m_callback = NULL;
	m_state = NULL;
	m_hostId = 0;
	m_shardNum = 0;
	memset(m_request, 0, sizeof(m_request));
	m_requestSize = 0;
	m_list = NULL;
	m_fixedDataSize = 0;
	m_useHalfKeys = false;
	memset(m_startKey, 0, sizeof(m_startKey));
	memset(m_endKey, 0, sizeof(m_endKey));
	m_minRecSizes = 0;
	m_rdbId = RDB_NONE;
	m_collnum = 0;
	m_isRealMerge = false;
	m_startTime = 0;
	m_niceness = 0;
	m_ks = 0;
}

Msg0::~Msg0 ( ) {
	reset();
}

void Msg0::reset ( ) {
	if ( m_msg5  ) {
		mdelete ( m_msg5 , sizeof(Msg5) , "Msg0::Msg5" );
		delete ( m_msg5  );
	}
	m_msg5  = NULL;

	if ( m_replyBuf )
		mfree ( m_replyBuf, m_replyBufSize, "Msg0" );
	m_replyBuf = NULL;
	m_replyBufSize = 0;
}

bool Msg0::registerHandler ( ) {
	// . register ourselves with the udp server
	// . it calls our callback when it receives a msg of type 0x0A
	if ( ! g_udpServer.registerHandler ( msg_type_0, handleRequest0 ))
		return false;
	return true;
}

// . THIS Msg0 class must be alloc'd, i.e. not on the stack, etc.
// . if list is stored locally this tries to get it locally
// . otherwise tries to get the list from the network
// . returns false if blocked, true otherwise
// . sets g_errno on error
// . NOTE: i was having problems with queries being cached too long, you
//   see the cache here is a NETWORK cache, so when the machines that owns
//   the list updates it on disk it can't flush our cache... so use a small
//   maxCacheAge of like , 30 seconds or so...
bool Msg0::getList ( int64_t hostId      , // host to ask (-1 if none)
		     rdbid_t   rdbId       , // specifies the rdb
		     collnum_t collnum ,
		     RdbList  *list        ,
		     const char     *startKey    ,
		     const char     *endKey      ,
		     int32_t      minRecSizes ,  // use -1 for no max
		     void     *state       ,
		     void    (* callback)(void *state ),//, RdbList *list ) ,
		     int32_t      niceness    ,
		     bool      doErrorCorrection ,
		     bool      includeTree ,
		     int32_t      firstHostId   ,
		     int32_t      startFileNum  ,
		     int32_t      numFiles      ,
		     int64_t      timeout       ,
		     bool      isRealMerge      ,
		     bool      noSplit ,
		     int32_t      forceParitySplit  ) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN. hostId: %" PRId64", rdbId: %d", hostId, (int)rdbId );

	// warning
	if ( collnum < 0 ) log(LOG_LOGIC,"net: NULL collection. msg0.");

	// reset the list they passed us
	list->reset();
	// get keySize of rdb
	m_ks = getKeySizeFromRdbId ( rdbId );

	// if startKey > endKey, don't read anything
	//if ( startKey > endKey ) return true;
	if ( KEYCMP(startKey,endKey,m_ks)>0 ) { g_process.shutdownAbort(true); }//rettrue
	// . reset hostid if it is dead
	// . this is causing UOR queries to take forever when we have a dead
	if ( hostId >= 0 && g_hostdb.isDead ( hostId ) ) hostId = -1;
	// no longer accept negative minrecsize
	if ( minRecSizes < 0 ) {
		g_errno = EBADENGINEER;
		logTrace( g_conf.m_logTraceMsg0, "END" );

		log(LOG_LOGIC, "net: msg0: Negative minRecSizes no longer supported.");
		g_process.shutdownAbort(true);
	}

	// callback is mandatory because msg0 can get from different host
	if (callback == nullptr) {
		logError("net: msg0: callback not supplied");
		gbshutdownLogicError();
	}

	// remember these
	m_state         = state;
	m_callback      = callback;
	m_list          = list;
	m_hostId        = hostId;
	m_niceness      = niceness;
	// . these define our request 100%
	KEYSET(m_startKey,startKey,m_ks);
	KEYSET(m_endKey,endKey,m_ks);
	m_minRecSizes   = minRecSizes;
	m_rdbId         = rdbId;
	m_collnum = collnum;//          = coll;
	m_isRealMerge   = isRealMerge;

	// . group to ask is based on the first key 
	// . we only do 1 group per call right now
	// . groupMask must turn on higher bits first (count downwards kinda)
	// . titledb and spiderdb use special masks to get groupId

	// did they force it? core until i figure out what this is
	if ( forceParitySplit >= 0 )
		m_shardNum = forceParitySplit;
	else
		m_shardNum = getShardNum ( m_rdbId , startKey );

	// if we are looking up a termlist in posdb that is split by termid and
	// not the usual docid then we have to set this posdb key bit that tells
	// us that ...
	if ( noSplit && m_rdbId == RDB_POSDB )
		m_shardNum = g_hostdb.getShardNumByTermId ( startKey );

	// . store these parameters
	// . get a handle to the rdb in case we can satisfy locally
	// . returns NULL and sets g_errno on error
	Rdb *rdb = getRdbFromId ( m_rdbId );
	if ( ! rdb ) return true;
	// we need the fixedDataSize
	m_fixedDataSize = rdb->getFixedDataSize();
	m_useHalfKeys   = rdb->useHalfKeys();

	// set this here since we may not call msg5 if list not local
	//m_list->setFixedDataSize ( m_fixedDataSize );

	// it it stored locally?
	bool isLocal = ( m_hostId == -1 && m_shardNum == getMyShardNum() );

	// but always local if only one host
	if ( g_hostdb.getNumHosts() == 1 ) isLocal = true;

	//if it is spiderdb then we only have it it we are a spider host too
	if((rdbId == RDB_SPIDERDB_DEPRECATED || rdbId == RDB2_SPIDERDB2_DEPRECATED) &&
	   isLocal &&
	   !g_hostdb.getMyHost()->m_spiderEnabled)
	{
		logTrace( g_conf.m_logTraceMsg0, "Not local (spiderdb and we're not a spider host");
		isLocal = false;
	}

	// . if the group is local then do it locally
	// . Msg5::getList() returns false if blocked, true otherwise
	// . Msg5::getList() sets g_errno on error
	// . don't do this if m_hostId was specified
	if ( isLocal ) {
		logTrace( g_conf.m_logTraceMsg0, "isLocal" );

		if(rdbId==RDB_SPIDERDB_DEPRECATED) {
			logTrace( g_conf.m_logTraceMsg0, "Jump to handleLocalSpiderdbGetList()" );
			handleLocalSpiderdbGetList(m_collnum,m_startKey,m_endKey,m_minRecSizes,m_list);
			logTrace( g_conf.m_logTraceMsg0, "END, jump to handleLocalSpiderdbGetList()" );
			return true;
		}
		try { m_msg5 = new ( Msg5 ); } 
		catch(std::bad_alloc&) {
			g_errno = ENOMEM;
			log(LOG_WARN, "net: Local alloc for disk read failed "
				"while tring to read data for %s. "
				"Trying remote request.",
				getDbnameFromId(m_rdbId));
			goto skip;
		}
		mnew ( m_msg5 , sizeof(Msg5) , "Msg0::Msg5" );

		if ( ! m_msg5->getList ( rdbId,
					 m_collnum ,
					 m_list ,
					 m_startKey ,
					 m_endKey   ,
					 m_minRecSizes ,
					 includeTree   , // include Tree?
					 startFileNum  ,
					 numFiles      ,
					 this ,
					 gotListWrapper2   ,
					 niceness          ,
					 doErrorCorrection ,
					 -1   , // maxRetries
					 m_isRealMerge ) ) {
			logTrace( g_conf.m_logTraceMsg0, "END, return false" );
			return false;
		}

		// nuke it
		reset();
		logTrace( g_conf.m_logTraceMsg0, "END, return true" );
		return true;
	}
skip:
	// debug msg
	if ( g_conf.m_logDebugQuery )
		log(LOG_DEBUG,"net: msg0: Sending request for data to "
		    "shard=%" PRIu32" "
		    "listPtr=%" PTRFMT" minRecSizes=%" PRId32" termId=%" PRIu64" "
		    "startKey.n1=%" PRIx64",n0=%" PRIx64" (niceness=%" PRId32")",
		    m_shardNum,
		    (PTRTYPE)m_list,
		    m_minRecSizes, Posdb::getTermId(m_startKey) ,
		    KEY1(m_startKey,m_ks),KEY0(m_startKey),
		    (int32_t)m_niceness);

	// . make a request with the info above (note: not in network order)
	// . IMPORTANT!!!!! if you change this change 
	//   Multicast.cpp::sleepWrapper1 too!!!!!!!!!!!!
	//   no, not anymore, we commented out that request peeking code
	char *p = m_request;
	*(int64_t *) p = -1        ; p += 8; // unused (syncPoint)
	*(int32_t      *) p = m_minRecSizes    ; p += 4;
	*(int32_t      *) p = startFileNum     ; p += 4;
	*(int32_t      *) p = numFiles         ; p += 4;
	*(int32_t      *) p = 0      ; p += 4; // unused (maxCacheAge)
	if ( p - m_request != MSG0RDBIDOFFSET ) { g_process.shutdownAbort(true); }
	*p               = m_rdbId          ; p++;
	*p               = false       ; p++; // unused (addToCache)
	*p               = doErrorCorrection; p++;
	*p               = includeTree      ; p++;
	*p               = (char)niceness   ; p++;
	*p               = true; p++; // unused (allowPageCache)
	KEYSET(p,m_startKey,m_ks);          ; p+=m_ks;
	KEYSET(p,m_endKey,m_ks);            ; p+=m_ks;
	*(collnum_t *)p = m_collnum; p += sizeof(collnum_t);
	m_requestSize    = p - m_request;
	// ask an individual host for this list if hostId is NOT -1
	if ( m_hostId != -1 ) {
		// get Host
		Host *h = g_hostdb.getHost ( m_hostId );
		if ( ! h ) { 
			g_errno = EBADHOSTID; 
			log(LOG_LOGIC,"net: msg0: Bad hostId of %" PRId64".", m_hostId);
			logTrace( g_conf.m_logTraceMsg0, "END, return true. Bad hostId" );
			return true;
		}

		uint16_t port = h->m_port ;
		// . returns false on error and sets g_errno, true otherwise
		// . calls callback when reply is received (or error)
		// . we return true if it returns false
		if (!g_udpServer.sendRequest(m_request, m_requestSize, msg_type_0, h->m_ip, port, m_hostId, NULL, this, gotSingleReplyWrapper, timeout, m_niceness)) {
			logTrace( g_conf.m_logTraceMsg0, "END, return true. Request sent" );
			return true;
		}
		
		// return false cuz it blocked
		logTrace( g_conf.m_logTraceMsg0, "END, return false. sendRequest blocked" );
		return false;
	}
	// timing debug
	if ( g_conf.m_logTimingNet )
		m_startTime = gettimeofdayInMilliseconds();
	else
		m_startTime = 0;

	// . get the top int32_t of the key
	// . i guess this will work for 128 bit keys... hmmmmm
	int32_t keyTop = hash32 ( (char *)startKey , m_ks );

	// . otherwise, multicast to a host in group "groupId"
	// . returns false and sets g_errno on error
	// . calls callback on completion
	// . select first host to send to in group based on upper 32 bits
	//   of termId (m_startKey.n1)
	// . need to send out to all the indexdb split hosts
	m_numRequests = 0;
	m_numReplies  = 0;

	// get the multicast
	Multicast *m = &m_mcast;

	// key is passed on startKey
	if (!m->send(m_request, m_requestSize, msg_type_0, false, m_shardNum, false, keyTop, this, NULL, gotMulticastReplyWrapper0, timeout * 1000, niceness, firstHostId, true)) {
		log(LOG_ERROR, "net: Failed to send request for data from %s in shard "
		    "#%" PRIu32" over network: %s.",
		    getDbnameFromId(m_rdbId),m_shardNum, mstrerror(g_errno));
		// but speed it up
		m_errno = g_errno;
		m->reset();
		if ( m_numRequests > 0 ) {
			logTrace( g_conf.m_logTraceMsg0, "END - returning false" );
			
			return false;
		}

		logTrace( g_conf.m_logTraceMsg0, "END - returning true" );
		return true;
	}

	m_numRequests++;

	// we blocked
	logTrace( g_conf.m_logTraceMsg0, "END - returning false, blocked" );
	return false;
}


// . this is called when we got a local RdbList
// . we need to call it to call the original caller callback
void Msg0::gotListWrapper2(void *state, RdbList *list, Msg5 *msg5) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN" );

	Msg0 *THIS = reinterpret_cast<Msg0*>(state);
	THIS->reset(); // delete m_msg5
	THIS->m_callback ( THIS->m_state );//, THIS->m_list );

	logTrace( g_conf.m_logTraceMsg0, "END." ); //we cannot log any members here because we may have been deleted+destroyed
}


// . return false if you want this slot immediately nuked w/o replying to it
void Msg0::gotSingleReplyWrapper(void *state, UdpSlot *slot) {
	Msg0 *THIS = reinterpret_cast<Msg0*>(state);
	if ( ! g_errno ) { 
		int32_t  replySize    = slot->m_readBufSize;
		int32_t  replyMaxSize = slot->m_readBufMaxSize;
		char *reply        = slot->m_readBuf;
		THIS->gotReply( reply , replySize , replyMaxSize );
		// don't let UdpServer free this since we own it now
		slot->m_readBuf = NULL;
		slot->m_readBufSize = 0;
		slot->m_readBufMaxSize = 0;
	}
	// never let m_request (sendBuf) be freed
	slot->m_sendBufAlloc = NULL;
	// do the callback now
	THIS->m_callback ( THIS->m_state );// THIS->m_list );
}

void Msg0::gotMulticastReplyWrapper0(void *state, void *state2) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN" );

	Msg0 *THIS = reinterpret_cast<Msg0*>(state);

	if ( ! g_errno ) {
		int32_t  replySize;
		int32_t  replyMaxSize;
		bool  freeit;
		char *reply = THIS->m_mcast.getBestReply (&replySize,
							  &replyMaxSize,
							  &freeit);
		THIS->gotReply( reply , replySize , replyMaxSize ) ;
	}
	THIS->m_callback ( THIS->m_state );
	logTrace( g_conf.m_logTraceMsg0, "END" );
}

// . returns false and sets g_errno on error
// . we are responsible for freeing reply/replySize
void Msg0::gotReply ( char *reply , int32_t replySize , int32_t replyMaxSize ) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN" );

	// timing debug
	if ( g_conf.m_logTimingNet && m_rdbId==RDB_POSDB && m_startTime > 0 )
		log(LOG_TIMING,"net: msg0: Got termlist, termId=%" PRIu64". "
		    "Took %" PRId64" ms, replySize=%" PRId32" (niceness=%" PRId32").",
		    Posdb::getTermId ( m_startKey ) ,
		    gettimeofdayInMilliseconds()-m_startTime,
		    replySize,m_niceness);
	// TODO: insert some seals for security, may have to alloc
	//       separate space for the list then
	// set the list w/ the remaining data

	m_list->set ( reply                , 
		      replySize            , 
		      reply                , // alloc buf begins here, too
		      replyMaxSize         ,
		      m_startKey           , 
		      m_endKey             , 
		      m_fixedDataSize      ,
		      true                 , // ownData?
		      m_useHalfKeys        ,
		      m_ks                 );

	logTrace( g_conf.m_logTraceMsg0, "END" );
}


// this conflicts with the State0 class in PageResults.cpp, so make it State00
class State00 {
public:
	Msg5       m_msg5;
	RdbList    m_list;
	UdpSlot   *m_slot;
	int64_t  m_startTime;
	int32_t       m_niceness;
	collnum_t  m_collnum;
	rdbid_t    m_rdbId;
	char       m_ks;
	char       m_startKey[MAX_KEY_BYTES];
	char       m_endKey[MAX_KEY_BYTES];
	int32_t    m_minRecSizes;
};

// . reply to a request for an RdbList
// . MUST call g_udpServer::sendReply or sendErrorReply() so slot can
//   be destroyed
void handleRequest0 ( UdpSlot *slot , int32_t netnice ) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN. Got request for an RdbList" );

	// get the request
	char *request     = slot->m_readBuf;
	int32_t  requestSize = slot->m_readBufSize;

	// parse the request
	char *p                  = request;
	p += 8; // syncPoint
	int32_t      minRecSizes        = *(int32_t      *)p ; p += 4;
	int32_t      startFileNum       = *(int32_t      *)p ; p += 4;
	int32_t      numFiles           = *(int32_t      *)p ; p += 4;
	p += 4; // maxCacheAge
	rdbid_t   rdbId              = ((rdbid_t)*p++);
	p++; // addToCache
	char      doErrorCorrection  = *p++;
	char      includeTree        = *p++;
	// this was messing up our niceness conversion logic
	int32_t      niceness           = slot->getNiceness();
	// still need to skip it though!
	p++;
	p++; // allowPageCache
	char ks = getKeySizeFromRdbId ( rdbId );
	char     *startKey           = p; p+=ks;
	char     *endKey             = p; p+=ks;
	collnum_t collnum = *(collnum_t *)p; p += sizeof(collnum_t);

	CollectionRec *xcr = g_collectiondb.getRec ( collnum );
	if ( ! xcr ) g_errno = ENOCOLLREC;

	if( g_conf.m_logTraceMsg0 ) {
		logTrace( g_conf.m_logTraceMsg0, "rdbId....... %d", (int)rdbId );
		logTrace( g_conf.m_logTraceMsg0, "key size.... %d", (int)ks );
		logTrace( g_conf.m_logTraceMsg0, "startFileNum %" PRId32, startFileNum );
		logTrace( g_conf.m_logTraceMsg0, "numFiles.... %" PRId32, numFiles );
	}


	// error set from XmlDoc::cacheTermLists()?
	if ( g_errno ) {
		logTrace( g_conf.m_logTraceMsg0, "END. Invalid collection" );

		log(LOG_ERROR,"%s:%s:%d: call sendErrorReply. Invalid collection", __FILE__, __func__, __LINE__);
		g_udpServer.sendErrorReply(slot, EBADRDBID);
		return;
	}

	// . get the rdb we need to get the RdbList from
	// . returns NULL and sets g_errno on error
	Rdb *rdb = getRdbFromId(rdbId);
	if (!rdb) {
		logTrace( g_conf.m_logTraceMsg0, "END. Invalid rdbId" );
		
		log(LOG_ERROR,"%s:%s:%d: call sendErrorReply. Invalid rdbId", __FILE__, __func__, __LINE__);
		g_udpServer.sendErrorReply(slot, EBADRDBID);
		return;
	}

	// keep track of stats
	rdb->readRequestGet ( requestSize );

	// . do a local get
	// . create a msg5 to get the list
	State00 *st0 ;
	try { st0 = new (State00); }
	catch(std::bad_alloc&) {
		g_errno = ENOMEM;
		log(LOG_WARN, "Msg0: new(%" PRId32"): %s",
		    (int32_t)sizeof(State00),mstrerror(g_errno));
		    
		log(LOG_ERROR,"%s:%s:%d: call sendErrorReply.", __FILE__, __func__, __LINE__);
		g_udpServer.sendErrorReply(slot, g_errno);
		return; 
	}
	mnew ( st0 , sizeof(State00) , "Msg0:State00" );
	// timing debug
	if ( g_conf.m_logTimingNet )
		st0->m_startTime = gettimeofdayInMilliseconds();
	// save slot in state
	st0->m_slot = slot;
	// init this one
	st0->m_niceness = niceness;
	st0->m_collnum = collnum;
	st0->m_rdbId    = rdbId;
	st0->m_ks = ks;
	memcpy(st0->m_startKey,startKey,ks);
	memcpy(st0->m_endKey,endKey,ks);
	st0->m_minRecSizes = minRecSizes;

	//spiderdb is sqlite-based, not Rdb-based
	if(rdbId==RDB_SPIDERDB_DEPRECATED) {
		handleSpiderdbRequest(st0);
		return;
	}

	// . if this request came over on the high priority udp server
	//   make sure the priority gets passed along
	// . return if this blocks
	// . we'll call sendReply later
	if ( ! st0->m_msg5.getList ( rdbId             ,
				     collnum           ,
				     &st0->m_list      ,
				     startKey          ,
				     endKey            ,
				     minRecSizes       ,
				     includeTree       , // include tree?
				     startFileNum      ,
				     numFiles          ,
				     st0               ,
				     gotListWrapper    ,
				     niceness          ,
				     doErrorCorrection ,
				     2    , // maxRetries
				     false ) ) {
		logTrace( g_conf.m_logTraceMsg0, "END. m_msg5.getList returned false" );
		return;
	}

	// call wrapper ouselves
	logTrace( g_conf.m_logTraceMsg0, "Calling gotListWrapper" );

	gotListWrapper ( st0 , NULL , NULL );

	logTrace( g_conf.m_logTraceMsg0, "END" );
}

// . slot should be auto-nuked upon transmission or error
// . TODO: ensure if this sendReply() fails does it really nuke the slot?
void gotListWrapper ( void *state , RdbList *listb , Msg5 *msg5xx ) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN" );
	
	// get the state
	State00 *st0 = (State00 *)state;
	// extract the udp slot and list and msg5
	UdpSlot   *slot =  st0->m_slot;
	RdbList   *list = &st0->m_list;
	Msg5      *msg5 = &st0->m_msg5;

	// timing debug
	if ( g_conf.m_logTimingNet || g_conf.m_logDebugNet ) {
		//log("Msg0:hndled request %" PRIu64,gettimeofdayInMilliseconds());
		int32_t size = -1;
		if ( list ) size     = list->getListSize();
		char ipbuf[16];
		log(LOG_TIMING|LOG_DEBUG,
		    "net: msg0: Handled request for data. "
		    "Now sending data termId=%" PRIu64" size=%" PRId32
		    " transId=%" PRId32" ip=%s port=%i took=%" PRId64" "
		    "(niceness=%" PRId32").",
		    Posdb::getTermId(st0->m_startKey),
		    size,slot->getTransId(),
		    iptoa(slot->getIp(),ipbuf), slot->getPort(),
		    gettimeofdayInMilliseconds() - st0->m_startTime ,
		    st0->m_niceness );
	}

	// on error nuke the list and it's data
	if ( g_errno ) {
		mdelete ( st0 , sizeof(State00) , "Msg0:State00" );
		delete (st0);
		// TODO: free "slot" if this send fails
		
		log(LOG_ERROR,"%s:%s:%d: call sendErrorReply. error=%s", __FILE__, __func__, __LINE__, mstrerror(g_errno));
		g_udpServer.sendErrorReply(slot, g_errno);
		return;
	}

	// point to the serialized list in "list"
	char *data      = list->getList();
	int32_t  dataSize  = list->getListSize();
	char *alloc     = list->getAlloc();
	int32_t  allocSize = list->getAllocSize();
	// tell list not to free the data since it is a reply so UdpServer
	// will free it when it destroys the slot
	list->setOwnData ( false );

	// keep track of stats
	Rdb *rdb = getRdbFromId ( st0->m_rdbId );
	if ( rdb ) {
		rdb->sentReplyGet ( dataSize );
	}

	// TODO: can we free any memory here???

	// keep track of how long it takes to complete the send
	st0->m_startTime = gettimeofdayInMilliseconds();
	// debug point
	int32_t oldSize = msg5->minRecSizes();
	int32_t newSize = msg5->minRecSizes() + 20;
	// watch for wrap around
	if ( newSize < oldSize ) newSize = 0x7fffffff;
	if ( dataSize > newSize && list->getFixedDataSize() == 0 &&
	     // do not annoy me with these linkdb msgs
	     dataSize > newSize+100 ) 
		log(LOG_LOGIC,"net: msg0: Sending more data than what was "
		    "requested. Ineffcient. Bad engineer. dataSize=%" PRId32" "
		    "minRecSizes=%" PRId32".",dataSize,oldSize);
		    
	//
	// for linkdb lists, remove all the keys that have the same IP32
	// and store a count of what we removed somewhere
	//
	if ( st0->m_rdbId == RDB_LINKDB ) {
		// store compressed list on itself
		char *dst = list->getList();
		// keep stats
		int32_t totalOrigLinks = 0;
		int32_t ipDups = 0;
		int32_t lastIp32 = 0;
		char *listEnd = list->getListEnd();
		// compress the list
		for ( ; ! list->isExhausted() ; list->skipCurrentRecord() ) {
			// count it
			totalOrigLinks++;
			// get rec
			char *rec = list->getCurrentRec();
			int32_t ip32 = Linkdb::getLinkerIp_uk((key224_t *)rec );
			// same as one before?
			if ( ip32 == lastIp32 && 
			     // are we the last rec? include that for
			     // advancing the m_nextKey in Linkdb more 
			     // efficiently.
			     rec + LDBKS < listEnd ) {
				ipDups++;
				continue;
			}
			// store it
			gbmemcpy (dst , rec , LDBKS );
			dst += LDBKS;
			// update it
			lastIp32 = ip32;
		}
		// . if we removed one key, store the stats
		// . caller should recognize reply is not a multiple of
		//   the linkdb key size LDBKS and no its there!
		if ( ipDups ) {
			//*(int32_t *)dst = totalOrigLinks;
			//dst += 4;
			//*(int32_t *)dst = ipDups;
			//dst += 4;
		}
		// update list parms
		list->setListSize(dst - list->getList());
		list->setListEnd(list->getList() + list->getListSize());
		data      = list->getList();
		dataSize  = list->getListSize();
	}

	// . TODO: dataSize may not equal list->getListMaxSize() so Mem class may show an imblanace
	// . now g_udpServer is responsible for freeing data/dataSize
	// . the "true" means to call doneSending_ass() from the signal handler
	//   if need be
	g_udpServer.sendReply(data, dataSize, alloc, allocSize, slot, st0, doneSending_ass);

	logTrace( g_conf.m_logTraceMsg0, "END" );
}	


// . this may be called from a signal handler
// . we call from a signal handler to keep msg21 zippy
// . this may be called twice, onece from sig handler and next time not
//   from the sig handler
void doneSending_ass ( void *state , UdpSlot *slot ) {
	// point to our state
	State00 *st0 = (State00 *)state;
	// this is nULL if we hit the cache above
	if ( ! st0 ) return;
	// this might be inaccurate cuz sig handler can't call it!
	int64_t now = gettimeofdayInMilliseconds();
	// log the stats
	if ( g_conf.m_logTimingNet ) {
		double mbps ;
		mbps = (((double)slot->m_sendBufSize) * 8.0 / (1024.0*1024.0))/
			(((double)slot->getStartTime())/1000.0);
		log(LOG_DEBUG, "net: msg0: Sent %" PRId32" bytes of data in %" PRId64" ms (%3.1fMbps) "
		      "(niceness=%" PRId32").",
		      slot->m_sendBufSize , now - slot->getStartTime() , mbps ,
		      st0->m_niceness );
	}
	// . mark it in pinkish purple
	// . BUT, do not add stats here for tagdb, we get WAY too many lookups
	//   and it clutters the performance graph
	if ( st0->m_rdbId == RDB_TAGDB ) {
	}
	else if(slot->getNiceness() > 0) {
		g_stats.addStat_r(slot->m_sendBufSize, st0->m_startTime, now, 0x00aa00aa);
	} 
	else {
		g_stats.addStat_r(slot->m_sendBufSize, st0->m_startTime, now, 0x00ff00ff);
	}


	// release st0 now
	mdelete ( st0 , sizeof(State00) , "Msg0:State00" );
	delete ( st0 );
}


static void handleSpiderdbRequest(State00 *state) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN");
	if(!g_jobScheduler.submit(execSpiderdbRequest, doneSpiderdbRequest,
				  state,
				  thread_type_spider_read,
				  0))
	{
		log(LOG_ERROR,"Could not submit job for spiderdb-read");
		g_udpServer.sendErrorReply(state->m_slot, g_errno);
		mdelete ( state , sizeof(State00) , "Msg0:State00" );
		delete state;
	}
	logTrace( g_conf.m_logTraceMsg0, "END");
}


static void execSpiderdbRequest(void *pv) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN");
	State00 *state = reinterpret_cast<State00*>(pv);
	if(!SpiderdbRdbSqliteBridge::getList(state->m_collnum,
					     &state->m_list,
					     *(reinterpret_cast<const u_int128_t*>(state->m_startKey)),
					     *(reinterpret_cast<const u_int128_t*>(state->m_endKey)),
					     state->m_minRecSizes)) {
		g_udpServer.sendErrorReply(state->m_slot, g_errno);
		mdelete(state, sizeof(State00), "Msg0:State00");
		delete state;
		logTrace( g_conf.m_logTraceMsg0, "END");
		return;
	}
	state->m_list.setOwnData(false);
	g_udpServer.sendReply(state->m_list.getList(), state->m_list.getListSize(), state->m_list.getAlloc(), state->m_list.getAllocSize(), state->m_slot, state, doneSending_ass);
	logTrace( g_conf.m_logTraceMsg0, "END");
}


static void doneSpiderdbRequest(void *pv, job_exit_t exit_type) {
	logTrace( g_conf.m_logTraceMsg0, "BEGIN");
	if(exit_type!=job_exit_normal) {
		State00 *state = reinterpret_cast<State00*>(pv);
		g_udpServer.sendErrorReply(state->m_slot, g_errno);
		mdelete(state, sizeof(State00), "Msg0:State00");
		delete state;
	}
	logTrace( g_conf.m_logTraceMsg0, "END");
}



static void handleLocalSpiderdbGetList(collnum_t collnum, const char *startKey, const char *endKey, int32_t minRecSizes, RdbList *list) {
	g_errno = 0;
	SpiderdbRdbSqliteBridge::getList(collnum,
					 list,
					 *(reinterpret_cast<const u_int128_t*>(startKey)),
					 *(reinterpret_cast<const u_int128_t*>(endKey)),
					 minRecSizes);
}
