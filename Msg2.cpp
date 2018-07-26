#include "Msg2.h"
#include "Stats.h"
#include "RdbList.h"
#include "Rdb.h"
#include "Posdb.h" // getTermId()
#include "Msg3a.h" // DEFAULT_POSDB_READ_SIZE
#include "HighFrequencyTermShortcuts.h"
#include "Sanity.h"
#include "Conf.h"
#include "ScopedLock.h"
#include "Mem.h"
#include "Errno.h"

#ifdef _VALGRIND_
#include <valgrind/memcheck.h>
#endif


static const int signature_init = 0x7e8a32f9;

// 90MB for 32 nodes we got now with about 1.3B docs
#define DEFAULT_POSDB_READSIZE 90000000


static int countWhitelistItems(const char *whitelist) {
	if(!whitelist)
		return 0;
	int c = 0;
	while(*whitelist) {
		while(*whitelist==' ')
			whitelist++;
		if(*whitelist)
			c++;
		while(*whitelist && *whitelist!=' ')
			whitelist++;
	}
	return c;
}



Msg2::Msg2()
  : m_whiteList(NULL),
    m_docIdStart(0),
    m_docIdEnd(0),
    m_p(NULL),
    m_w(0),
    m_whiteLists(NULL),
    m_numWhitelists(0),
    m_msg5(0),
    m_avail(0),
    m_errno(0),
    m_lists(NULL),
    m_qterms(NULL),
    m_numLists(0),
    m_getComponents(false),
    m_addToCache(false),
    m_collnum(0),
    m_allowHighFrequencyTermCache(false),
    m_numReplies(0),
    m_numRequests(0),
    m_requestsBeingSubmitted(false),
    m_state(NULL),
    m_callback(NULL),
    m_niceness(0),
    m_isDebug(false),
    m_startTime(0)
{
	set_signature();
}

Msg2::~Msg2() {
	reset();
	clear_signature();
}

void Msg2::reset ( ) {
	verify_signature();
	if(!allRequestsReplied())
		gbshutdownLogicError();
	m_numLists = 0;
	m_whiteList = 0;
	m_p = 0;
	delete[] m_msg5;
//if(m_msg5) {
//	for(int i=0; i<m_numLists+m_numWhitelists; i++)
//		(m_msg5+i)->~Msg5();
//	memset(m_msg5,-4,sizeof(*m_msg5)*(m_numLists+m_numWhitelists));
//}
	m_msg5 = 0;
	delete[] m_avail;
	m_avail = 0;
	m_lists = 0;
	delete[] m_whiteLists;
	m_whiteLists = NULL;
	m_numWhitelists = 0;
}


void Msg2::incrementRequestCount() {
	ScopedLock sl(m_mtxCounters);
	m_numRequests++;
}

bool Msg2::incrementReplyCount() {
	ScopedLock sl(m_mtxCounters);
	if(m_numReplies>=m_numRequests)
		gbshutdownCorrupted();
	m_numReplies++;
	return m_numReplies==m_numRequests && !m_requestsBeingSubmitted;
}

bool Msg2::allRequestsReplied()	{
	ScopedLock sl(m_mtxCounters);
	return (!m_requestsBeingSubmitted) && (m_numReplies==m_numRequests);
}


// . returns false if blocked, true otherwise
// . sets g_errno on error
// . componentCodes are used to collapse a series of termlists into a single
//   compound termlist. component termlists have their compound termlist number
//   as their componentCode, compound termlists have a componentCode of -1,
//   other termlists have a componentCode of -2. These are typically taken
//   from the Query.cpp class.
bool Msg2::getLists ( collnum_t collnum , // char    *coll        ,
		      bool     addToCache  ,
		      const QueryTerm *qterms,
		      int32_t numQterms,
		      // put list of sites to restrict to in here
		      // or perhaps make it collections for federated search?
		      const char *whiteList ,
		      int fileNum,
		      int64_t docIdStart,
		      int64_t docIdEnd,
		      // make max MAX_MSG39_LISTS
		      RdbList *lists       ,
		      void    *state       ,
		      void   (* callback)(void *state ) ,
		      bool allowHighFrequencyTermCache,
		      int32_t     niceness    ,
		      bool     isDebug ) {
#ifdef _VALGRIND_
	VALGRIND_CHECK_MEM_IS_ADDRESSABLE(qterms,numQterms*sizeof(*qterms));
#endif
	verify_signature();
	// warning
	if ( collnum < 0 ) log(LOG_LOGIC,"net: bad collection. msg2.");
	// save callback and state
	m_state       = state;
	m_callback    = callback;
	m_niceness    = niceness;
	m_isDebug     = isDebug;
	m_lists = lists;
	//m_totalRead   = 0;
	m_whiteList = whiteList;
	m_w = 0;
	m_p = whiteList;

	m_fileNum    = fileNum;
	m_docIdStart = docIdStart;
	m_docIdEnd   = docIdEnd;
	m_allowHighFrequencyTermCache = allowHighFrequencyTermCache;
	m_qterms              = qterms;
	m_getComponents       = false;
	m_addToCache          = addToCache;
	m_collnum             = collnum;
	// we haven't got any responses as of yet or sent any requests
	m_numReplies  = 0;
	m_numRequests = 0;
	// start the timer
	m_startTime = gettimeofdayInMilliseconds();
	// set this
	m_numLists = numQterms;
	m_numWhitelists = countWhitelistItems(whiteList);

	m_msg5 = new Msg5[m_numLists+m_numWhitelists];
	m_avail = new bool[m_numLists+m_numWhitelists];
	m_whiteLists = new RdbList[m_numWhitelists];
	for ( int32_t i = 0; i < m_numLists+m_numWhitelists; i++ )
		m_avail[i] = true;
		
	if ( m_isDebug ) {
		if ( m_getComponents ) log ("query: Getting components.");
		else                   log ("query: Getting lists.");
	}
	// reset error
	m_errno = 0;
	// fetch what we need
	return getLists ( );
}

bool Msg2::getLists ( ) {
	//log(LOG_TRACE,"Msg2(%p)::getLists()",this);
#ifdef _VALGRIND_
	VALGRIND_CHECK_MEM_IS_ADDRESSABLE(m_qterms,m_numLists*sizeof(*m_qterms));
#endif
	{
		ScopedLock sl(m_mtxCounters);
		m_requestsBeingSubmitted = true;
	}

	// . send out a bunch of msg5 requests
	// . make slots for all
	for(int m_i=0; m_i < m_numLists; m_i++) {
#ifdef _VALGRIND_
	VALGRIND_CHECK_MEM_IS_ADDRESSABLE(m_qterms,m_numLists*sizeof(*m_qterms));
#endif
		// sanity for Msg39's sake. do no breach m_lists[].
		if ( m_i >= ABS_MAX_QUERY_TERMS ) gbshutdownLogicError();
		// if any had error, forget the rest. do not launch any more
		if ( m_errno ) break;
		
		const QueryTerm *qt = &m_qterms[m_i];
		if ( qt->m_ignored ) //skip ignored terms
			continue;

		if ( m_isDebug ) {
			key144_t *sk ;
			key144_t *ek ;
			sk = (key144_t *)m_qterms[m_i].m_startKey;
			ek = (key144_t *)m_qterms[m_i].m_endKey;
			int64_t docId0 = Posdb::getDocId(sk);
			int64_t docId1 = Posdb::getDocId(ek);
			log("query: reading termlist #%" PRId32" "//from "
			    //"distributed cache on host #%" PRId32". "
			    "termId=%" PRId64". sk=%s ek=%s "
			    " (docid0=%" PRId64" to "
			    "docid1=%" PRId64").",
			    m_i,
			    //hostId, 
			    Posdb::getTermId(sk),
			    KEYSTR(sk,sizeof(posdbkey_t)),
			    KEYSTR(ek,sizeof(posdbkey_t)),
			    //sk->n2,
			    //sk->n1,
			    //(int32_t)sk->n0,
			    docId0,
			    docId1);
		}
		
		int32_t minRecSize = DEFAULT_POSDB_READSIZE;



		const char *sk2 = NULL;
		const char *ek2 = NULL;
		sk2 = qt->m_startKey;
		ek2 = qt->m_endKey;

		// if single word and not required, skip it
		if ( ! qt->m_isRequired && 
		     ! qt->m_isPhrase &&
		     ! qt->m_synonymOf )
			continue;

		//if the term is a high-frequency one then use the PosDB shortcuts
		const void *hfterm_shortcut_posdb_buffer;
		size_t hfterm_shortcut_buffer_bytes;
		char startKey[18], endKey[18];
		if(g_conf.m_useHighFrequencyTermCache &&
		   m_allowHighFrequencyTermCache &&
		   g_hfts.query_term_shortcut(m_qterms[m_i].m_termId,&hfterm_shortcut_posdb_buffer,&hfterm_shortcut_buffer_bytes,startKey,endKey))
		{
			log("query: term %" PRId64" (%*.*s) is a high-frequency term",
			    m_qterms[m_i].m_termId,qt->m_qword->m_wordLen,qt->m_qword->m_wordLen,qt->m_qword->m_word);
			//use PosDB shortcut buffer, put into RdbList and avoid actually going into PosDB
			
			char *rdblistmem = (char*)mmalloc(hfterm_shortcut_buffer_bytes,"RdbList");
			memcpy(rdblistmem,hfterm_shortcut_posdb_buffer,hfterm_shortcut_buffer_bytes);
			m_lists[m_i].set(rdblistmem,                           //list
			                 hfterm_shortcut_buffer_bytes,         //listSize
			                 rdblistmem,                           //alloc
			                 hfterm_shortcut_buffer_bytes,         //allocSize
			                 startKey,                             //startkey
			                 endKey,                               //endkey
			                 Posdb::getFixedDataSize(),
			                 true,                                 //owndata
			                 Posdb::getUseHalfKeys(),
			                 Posdb::getKeySize());
			char ek2_copy[18];
			memcpy(ek2_copy, ek2, sizeof(ek2_copy)); //RdbList::constrain() modifies endkey, so give it a copy
			m_lists[m_i].constrain(sk2, ek2_copy, -1, 0, NULL, RDB_POSDB, "highfrequencyterm");
			continue;
		}

		Msg5 *msg5 = getAvailMsg5();
		if(!msg5) gbshutdownLogicError();

		// . start up a Msg5 to get it
		// . this will return false if blocks
		// . no need to do error correction on this since only RdbMerge
		//   really needs to do it and he doesn't call Msg2
		// . this is really only used to get IndexLists
		// . we now always compress the list for 2x faster transmits
		if(m_fileNum>=0) {
			incrementRequestCount();
			if ( ! msg5->getSingleUnmergedList ( RDB_POSDB,
							m_collnum,
							&m_lists[m_i],  // listPtr
							sk2,
							ek2,
							minRecSize,
							m_fileNum,      // file num
							this,
							gotListWrapper,
							m_niceness) )
			{
				continue;
			}
			incrementReplyCount();
		} else if(m_fileNum==-1) {
			//get the tree
			if(!msg5->getTreeList(&m_lists[m_i],RDB_POSDB,m_collnum,sk2,ek2)) {
				log("query: Msg5::getTreeList() failed");
				goto skip;
			}
		} else
			gbshutdownLogicError();

		//log(LOG_TRACE,"Msg2::getLists(): msg5::getList() returned immediately");
		// we didn't block, so do this
		// return the msg5 now
		msg5->reset();
		returnMsg5 ( msg5 );
		// note it
		
		// break out on error and wait for replies if we blocked
		if ( g_errno!=0 ) {
			// report the error and return
			m_errno = g_errno;
			log("query: Got error reading termlist: %s.", mstrerror(g_errno));
			goto skip;
		}
	}

	//
	// now read in lists from the terms in the "whiteList"
	//

	// . loop over terms in the whitelist, space separated. 
	// . m_whiteList is NULL if none provided.
	for ( const char *p = m_p ; m_whiteList && *p ; m_w++ ) {
		// advance
		const char *current = p;
		for ( ; *p && *p != ' ' ; p++ );
		// save end of "current"
		const char *end = p;
		// advance to point to next item in whiteList
		for ( ; *p == ' ' ; p++ );
		// . convert whiteList term into key
		// . put the "site:" prefix before it first
		// . see XmlDoc::hashUrl() where prefix = "site"
		int64_t prefixHash = hash64b ( "site" );
		//int64_t termId = hash64(current,end-current);
		// crap, Query.cpp i guess turns xyz.com into http://xyz.com/
		int32_t conti = 0;
		int64_t termId = 0LL;
		termId = hash64_cont("http://",7,termId,&conti);
		termId = hash64_cont(current,end-current,termId,&conti);
		termId = hash64_cont("/",1,termId,&conti);
		int64_t finalTermId = hash64 ( termId , prefixHash );
		// mask to 48 bits
		finalTermId &= TERMID_MASK;
		// . make key. be sure to limit to provided docid range
		//   if we are doing docid range splits to prevent OOM
		// . these docid ranges were likely set in Msg39::
		//   doDocIdRangeSplitLoop(). it already applied them to
		//   the QueryTerm::m_startKey in Msg39.cpp so we have to
		//   apply here as well...
		char sk3[MAX_KEY_BYTES];
		char ek3[MAX_KEY_BYTES];
		Posdb::makeStartKey ( sk3 , finalTermId , m_docIdStart );
		Posdb::makeEndKey   ( ek3 , finalTermId , m_docIdEnd );
		// get one
		Msg5 *msg5 = getAvailMsg5();
		if(!msg5) gbshutdownLogicError();

		// advance cursor
		m_p = p;

		// sanity for Msg39's sake. do no breach m_lists[].
		if ( m_w >= m_numWhitelists ) gbshutdownLogicError();

		// like 90MB last time i checked. so it won't read more
		// than that...
		// MDW: no, it's better to print oom then not give all the
		// results leaving users scratching their heads. besides,
		// we should do docid range splitting before we go out of
		// mem. we should also report the size of each termlist
		// in bytes in the query info header.
		//int32_t minRecSizes = DEFAULT_POSDB_READSIZE;
		// MDW TODO fix this later we go oom too easily for queries
		// like 'www.disney.nl'
		int32_t minRecSizes = -1;

		// start up the read. thread will wait in thread queue to 
		// launch if too many threads are out.
		if(m_fileNum>=0) {
			incrementRequestCount();
			if ( ! msg5->getSingleUnmergedList ( RDB_POSDB,
							     m_collnum,
							     &m_whiteLists[m_w], // listPtr
							     sk3,
							     ek3,
							     minRecSizes,
							     m_fileNum,         // file num
							     this,
							     gotListWrapper,
							     m_niceness ) )
			{
				continue;
			}

			incrementReplyCount();
		} else if(m_fileNum==-1) {
			//get the tree
			if(!msg5->getTreeList(&m_whiteLists[m_w],RDB_POSDB,m_collnum,sk3,ek3)) {
				log("query: Msg5::getTreeList() failed");
				goto skip;
			}
		} else
			gbshutdownLogicError();
		// . return the msg5 now
		msg5->reset();
		returnMsg5 ( msg5 );
		
		// break out on error and wait for replies if we blocked
		if ( g_errno!=0 ) {
			// report the error and return
			m_errno = g_errno;
			log("query: Got error reading termlist: %s.", mstrerror(g_errno));
			goto skip;
		}
	}
		
 skip:

	{
		ScopedLock sl(m_mtxCounters);
		m_requestsBeingSubmitted = false;
		
		//if we have outstanding requests then return false (a callback will be called)
		if(m_numReplies!=m_numRequests)
			return false;
	}

// 	// . otherwise, we got everyone, so go right to the merge routine
	// . returns false if not all replies have been received 
	// . returns true if done
	// . sets g_errno on error
	return gotList();
}

Msg5 *Msg2::getAvailMsg5 ( ) {
	verify_signature();
	ScopedLock sl(m_mtxMsg5);
	for ( int32_t i = 0; i < m_numLists+m_numWhitelists; i++ ) {
		if(m_avail[i]) {
			m_avail[i] = false;
			return &m_msg5[i];
		}
	}
	return NULL;
}

void Msg2::returnMsg5 ( Msg5 *msg5 ) {
	verify_signature();
	ScopedLock sl(m_mtxMsg5);
	if(msg5 < m_msg5)
		gbshutdownLogicError();
	if(msg5 >= m_msg5+m_numLists+m_numWhitelists)
		gbshutdownLogicError();
	int32_t i = (int32_t)(msg5-m_msg5);
	if(m_avail[i])
		gbshutdownLogicError();
	msg5->reset();
//	m_avail[i] = true;
	verify_signature();
}


void Msg2::gotListWrapper(void *state, RdbList *rdblist, Msg5 *msg5) {
	Msg2 *that = static_cast<Msg2*>(state);
	that->gotListWrapper(msg5);
}


void Msg2::gotListWrapper( Msg5 *msg5 ) {
	verify_signature();
	RdbList *list = msg5->m_list;
	// note it
	if ( g_errno ) {
		log ("msg2: error reading list: %s",mstrerror(g_errno));
		m_errno = g_errno;
		g_errno = 0;
	}
	// identify the msg0 slot we use
	int32_t i  = list - m_lists;
	msg5->reset();
	returnMsg5 ( msg5 );
	// note it
	if ( m_isDebug ) {
		if ( ! list )
			logf(LOG_DEBUG,"query: got NULL list #%" PRId32,  i);
		else
			logf(LOG_DEBUG,"query: got list #%" PRId32" size=%" PRId32,
			     i,list->getListSize() );
	}
	
	bool done = incrementReplyCount();
	if(!done)
		return; //still more to go
	// set g_errno if any one list read had error
	if ( m_errno ) g_errno = m_errno;
	// now call callback, we're done
	m_callback ( m_state );
}


// . returns false if not all replies have been received (or timed/erroredout)
// . returns true if done (or an error finished us)
// . sets g_errno on error
// . "list" is NULL if we got all lists w/o blocking and called this
bool Msg2::gotList() {
	verify_signature();

	// wait until we got all the replies before we attempt to merge
	if(!allRequestsReplied())
		return false;

	// . return true on error
	// . no, wait to get all the replies because we destroy ourselves
	//   by calling the callback, and another reply may come back and
	//   think we're still around. so, ideally, destroy those udp slots
	//   OR just wait for all replies to come in.
	//if ( g_errno ) return true;
	if ( m_errno )
		log("net: Had error fetching data from %s: %s.", 
		    getDbnameFromId(RDB_POSDB),
		    mstrerror(m_errno) );

	// note it
	if ( m_isDebug ) {
		for ( int32_t i = 0 ; i < m_numLists ; i++ ) {
			log("msg2: read termlist #%" PRId32" size=%" PRId32,
			    i,m_lists[i].getListSize());
		}
	}

	// bitch if we hit our max read sizes limit, we are losing docids!
	for ( int32_t i = 0 ; i < m_numLists ; i++ ) {
		if ( m_lists[i].getListSize() < DEFAULT_POSDB_READSIZE ) continue;
		if ( m_lists[i].getListSize() == 0 ) continue;

		log("msg2: read termlist #%" PRId32" size=%" PRId32" "
		    "maxSize=%" PRId32". losing docIds!",
		    i,m_lists[i].getListSize(),DEFAULT_POSDB_READSIZE);
	}

	// set this i guess
	g_errno = m_errno;

	// all done
	return true;
}
