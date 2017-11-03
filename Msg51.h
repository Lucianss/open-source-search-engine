// Matt Wells, copyright Jun 2007

// . gets the clusterRecs for a list of docIds
// . list of docids can be from an IndexList if provided, or a straightup array
// . meant as a replacement for some of Msg38
// . see Clusterdb.h for fomat of clusterRec
// . actually only stores the lower 64 bits of each cluster rec, that is all
//   that is interesting

#ifndef GB_MSG51_H
#define GB_MSG51_H

#include "Msg0.h"
#include "Clusterdb.h"
#include "RdbList.h"
#include "Msg5.h"
#include "GbSignature.h"
#include <pthread.h>

// . m_clusterLevels[i] takes on one of these values
// . these describe a docid
// . they tell us why the docid is not ok to be displayed in the search results
// . this is used as part of the post query filtering step, after we get the
//   resulting docids from Msg3a.
// . these are set some in Msg51.cpp but mostly in Msg40.cpp
enum {
	// if clusterdb rec was not found...
	CR_NOTFOUND        = 0 ,
	// clusterdb rec never set... how did this happen?
	CR_UNINIT          ,
	// we got the clusterdb rec, this is a transistional value.
	CR_GOT_REC         ,
	// had adult content
	CR_DIRTY           ,
	// language did not match the language filter (iff langFilter>0)
	CR_WRONG_LANG        ,
	// a 3rd+ result from the same hostname
	CR_CLUSTERED       ,
	// has xml tag syntax in the url
	CR_BAD_URL         ,
	// the url is banned in tagdb or url filters table
	CR_BANNED_URL      ,
	// the title & summary is empty
	CR_EMPTY_TITLE_SUMMARY   ,
	// error getting summary (Msg20::m_errno is set)
	CR_ERROR_SUMMARY   ,
	// a summary dup of a higher-scoring result
	CR_DUP_SUMMARY     ,
	// another error getting it... could be one of many
	CR_ERROR_CLUSTERDB ,
	// the url is a dup of a previous url (wiki pages capitalization)
	CR_DUP_URL         ,
	// the url doesn't have any content due to simplified redirection page/non-caconical page
	CR_EMPTY_REDIRECTION_PAGE,
	// the docid is ok to display!
	CR_OK              ,
	// from a blacklisted site hash
	CR_BLACKLISTED_SITE  ,
	// was filtered because of ruleset
	CR_RULESET_FILTERED ,
	// URL (or site) classified as malicious (spyware, trojan, phishing, ...)
	CR_MALICIOUS,
	// verify this is LAST entry cuz we use i<CR_END for ending for-loops
	CR_END
};


extern const char * const g_crStrings[];

bool setClusterLevels ( const key96_t   *clusterRecs,
			const int64_t *docIds,
			int32_t       numRecs              ,
			int32_t       maxDocIdsPerHostname ,
			bool       doHostnameClustering ,
			bool       familyFilter         ,
			bool       isDebug              ,
			// output to clusterLevels[]
			char      *clusterLevels        );

class Msg51 {

 public:

	Msg51();
	~Msg51();
	void reset();

	// . returns false if blocked, true otherwise
	// . sets g_errno on error
	// . we just store the "int32_t" part of the cluster rec
	bool getClusterRecs ( const int64_t     *docIds,
			      char          *clusterLevels            ,
			      key96_t         *clusterRecs              ,
			      int32_t           numDocIds                ,
			      collnum_t collnum ,
			      void          *state                    ,
			      void        (* callback)( void *state ) ,
			      int32_t           niceness                 ,
			      // output to clusterRecs[]
			      bool           isDebug                  ) ;

	// see Clusterdb.h for this bitmap. we store the lower 64 bits of
	// the clusterdb key into the "clusterRecs" array
	//bool isFamilyBitOn ( uint64_t clusterRec ) {
	//	return g_clusterdb.hasAdultContent((char *)&clusterRec); }
	//char     getLangId     ( uint64_t clusterRec ) {
	//	return g_clusterdb.getLanguage((char *)&clusterRec); }
	//uint32_t getSiteHash26   ( uint64_t clusterRec ) {
	//	return g_clusterdb.getSiteHash26((char *)&clusterRec); }


        key96_t getClusterRec ( int32_t i ) const { return m_clusterRecs[i]; }

private:
	bool sendRequests   ( int32_t k );
	bool sendRequests_unlocked(int32_t k);
	bool sendRequest    ( int32_t i );

	declare_signature

	// docIds we're getting clusterRecs for
	const int64_t   *m_docIds;
	int32_t         m_numDocIds;

	// the lower 64 bits of each cluster rec
	key96_t      *m_clusterRecs;
	char       *m_clusterLevels;

	void     (*m_callback ) ( void *state );
	void      *m_state;

	pthread_mutex_t m_mtx; //protects m_nexti, m_numXxxx, m_slot, etc.
	// next cluster rec # to get (for m_docIds[m_nexti])
	int32_t      m_nexti;

	// use to get the cluster recs
	int32_t       m_numRequests;
	int32_t       m_numReplies;
	int32_t       m_errno;

	int32_t       m_niceness;

	collnum_t m_collnum;

	bool       m_isDebug;

	struct Slot {
		Msg51     *m_msg51; //points to self
		Msg0       m_msg0;
		RdbList    m_list;
		bool       m_inUse;
		int32_t    m_ci;
	};
	Slot *m_slot;
	int32_t m_numSlots;

	static void gotClusterRecWrapper51(void *state);
	void gotClusterRec(Slot *slot);
};


class RdbCache;
extern RdbCache s_clusterdbQuickCache;

#endif // GB_MSG51_H
