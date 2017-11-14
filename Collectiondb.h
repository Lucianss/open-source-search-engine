// Matt Wells, copyright Feb 2001

// maintains a simple array of CollectionRecs

#ifndef GB_COLLECTIONDB_H
#define GB_COLLECTIONDB_H

#include <atomic>
#include "SafeBuf.h"
#include "rdbid_t.h"
#include "collnum_t.h"
#include "spider_status_t.h"
#include "GbMutex.h"


class Collectiondb  {

 public:
	Collectiondb();
	~Collectiondb();

	void reset() ;
	
	// called by main.cpp to fill in our m_recs[] array with
	// all the coll.*.*/coll.conf info
	bool loadAllCollRecs ( );

	// . this will save all conf files back to disk that need it
	// . returns false and sets g_errno on error, true on success
	bool save ( );

	bool isInitializing() const { return m_initializing; }

	// returns i so that m_recs[i].m_coll = coll
	collnum_t getCollnum(const char *coll, int32_t collLen) const;
	collnum_t getCollnum(const char *coll) const; // coll is NULL terminated here

	const char *getCollName(collnum_t collnum) const;

	// get coll rec specified in the HTTP request
	class CollectionRec *getRec ( class HttpRequest *r ,
				      bool useDefaultRec = true );

	//Returns the specified collection name, or the default collection if no collection name was specified
	const char *getDefaultColl(const char *collname_from_httprequest);

	// . get collectionRec from name
	// returns NULL if not available
	class CollectionRec *getRec ( const char *coll );

	class CollectionRec *getRec ( const char *coll , int32_t collLen );

	class CollectionRec *getRec ( collnum_t collnum);

	//class CollectionRec *getDefaultRec ( ) ;

	class CollectionRec *getFirstRec      ( ) ;
	collnum_t            getFirstCollnum() const ;

	int32_t getNumRecs() const { return m_numRecs; }
	int32_t getNumRecsUsed() const { return m_numRecsUsed; }

	// what collnum will be used the next time a coll is added?
	collnum_t reserveCollNum ( ) ;

	bool addExistingColl ( const char *coll, collnum_t collnum );

	bool addNewColl( const char *coll, collnum_t newCollnum ) ;

	bool addRdbBaseToAllRdbsForEachCollRec ( ) ;
	bool addRdbBasesForCollRec ( CollectionRec *cr ) ;

	// returns false if blocked, true otherwise.
	bool deleteRec2 ( collnum_t collnum );

	//void deleteSpiderColl ( class SpiderColl *sc );

	// returns false if blocked, true otherwise.
	bool resetColl2(collnum_t oldCollnum, collnum_t newCollnum);


private:
	// after main.cpp loads all rdb trees it calls this to remove
	// bogus collnums from the trees i guess
	bool cleanTrees();

	bool registerCollRec(CollectionRec *cr);

	bool growRecPtrBuf(collnum_t collnum);
	bool setRecPtr(collnum_t collnum, CollectionRec *cr);

	class CollectionRec  **m_recs;

	// m_recs[] points into a safebuf that is just an array
	// of collectionrec ptrs. so we have to grow that safebuf possibly
	// in order to add a new collection rec ptr to m_recs
	SafeBuf m_recPtrBuf;

	int32_t            m_numRecs;
	int32_t            m_numRecsUsed;

	int32_t m_wrapped;

	bool m_initializing;
};

extern class Collectiondb g_collectiondb;

// Matt Wells, copyright Feb 2002

// . a collection record specifies the spider/index/search parms of a
//   collection of web pages
// . there's a Msg class to send an update signal to all the hosts once
//   we've used Msg1 to add a new rec or delete an old.  The update signal
//   will make the receiving hosts flush their CollectionRec buf so they
//   have to send out a Msg0 to get it again
// . we have a default collection record, a main collection record and
//   then other collection records
// . the default collection record values override all
// . but the collection record values can override SiteRec values
// . so if spider is disabled in default collection record, then nobody
//   can spider!
// . override the g_conf.* vars where * is in this class to use
//   Collection db's default values
// . then add in the values of the specialzed collection record
// . so change "if ( g_conf.m_spideringEnabled )" to something like
//   Msg33 msg33;
//   if ( ! msg33.getCollectionRec ( m_coll, m_collLen ) ) return false;
//   CollectionRec *r = msg33.getRec();
//   CollectoinRec *d = msg33.getDefaultRec();
//   if ( ! r->m_spideringEnabled || ! d->m_spideringEnabled ) continue;
//   ... otherwise, spider for the m_coll collection
//   ... pass msg33 to Msg14::spiderDoc(), etc...

// how many url filtering patterns?
#define MAX_FILTERS    96  // up to 96 url regular expression patterns

#define SUMMARYHIGHLIGHTTAGMAXSIZE 128

#include "max_coll_len.h"
#include "HashTableX.h"

// fake this for now
#define RDB_END2 80

class CollectionRec {

 public:

	// active linked list of collectionrecs used by spider.cpp
	class CollectionRec *m_nextActive;

	// these just set m_xml to NULL
	CollectionRec();
	virtual ~CollectionRec();

	int64_t getNumDocsIndexed();

	// . stuff used by Collectiondb
	// . do we need a save or not?
	bool save();
	void setNeedsSave() { m_needsSave = true; }

private:
	std::atomic<bool> m_needsSave;

public:
	bool      load ( const char *coll , int32_t collNum ) ;
	void reset();

	// Clear memory structures used by URL filters
	void clearUrlFilters();

	// for customcrawls
	bool rebuildUrlFilters();

	// for regular crawls
	bool rebuildUrlFilters2();

	bool rebuildLangRules( const char *lang , const char *tld );

	bool rebuildPrivacoreRules();

	bool m_urlFiltersHavePageCounts;

	// the all important collection name, NULL terminated
	char  m_coll [ MAX_COLL_LEN + 1 ] ;
	int32_t  m_collLen;

	// used by SpiderCache.cpp. g_collectiondb.m_recs[m_collnum] = this
	collnum_t m_collnum;

	// for doing DailyMerge.cpp stuff
	int32_t m_dailyMergeStarted; // time_t
	int32_t m_dailyMergeTrigger;

	char m_dailyMergeDOWList[48];

	int64_t m_spiderCorruptCount;

	// holds ips that have been detected as being throttled and we need
	// to backoff and use proxies on
	HashTableX m_twitchyTable;

	// spider controls for this collection
	bool m_spideringEnabled ;
	int32_t  m_spiderDelayInMilliseconds;
	int32_t m_spiderReindexDelayMS;

	// is in active list in spider.cpp?
	bool m_isActive;

	bool  m_makeImageThumbnails;

	int32_t m_thumbnailMaxWidthHeight ;

	bool  m_indexBody;

	bool  m_dedupingEnabled         ; // dedup content on same hostname
	bool  m_dupCheckWWW             ;
	bool  m_useSimplifiedRedirects  ;
	bool  m_useTimeAxis             ;
	bool  m_oneVotePerIpDom         ;
	bool  m_doUrlSpamCheck          ; //filter urls w/ naughty hostnames
	bool  m_doLinkSpamCheck         ; //filters dynamically generated pages
	bool  m_siteClusterByDefault    ;
	bool  m_useRobotsTxt            ;
	bool  m_obeyRelNoFollowLinks    ;
	bool  m_forceUseFloaters        ;
	bool  m_automaticallyUseProxies ;
	bool  m_automaticallyBackOff    ;
	bool  m_recycleContent          ;
	bool  m_getLinkInfo             ; // turn off to save seeks
	bool  m_computeSiteNumInlinks   ;

	int32_t  m_percentSimilarSummary       ; // Dedup by summary similiarity
	int32_t  m_summDedupNumLines           ;

	int32_t  m_maxQueryTerms;

	spider_status_t  m_spiderStatus;

	//ranking settings
	float m_sameLangWeight;
	float m_unknownLangWeight;

	// Language stuff
	char 			m_defaultSortLanguage2[6];


	SafeBuf m_collectionPasswords;
	SafeBuf m_collectionIps;

	// from Conf.h
	int32_t m_posdbMinFilesToMerge ;
	int32_t m_titledbMinFilesToMerge ;
	int32_t m_linkdbMinFilesToMerge ;
	int32_t m_tagdbMinFilesToMerge ;
	int32_t m_spiderdbMinFilesToMerge;

	bool  m_dedupResultsByDefault   ;
	bool  m_doTagdbLookups        ;
	bool  m_useCanonicalRedirects   ;

	int32_t  m_maxNumSpiders             ; // per local spider host

	int32_t m_lastResetCount;

	// controls for query-dependent summary/title generation
	int32_t m_titleMaxLen;
	int32_t m_summaryMaxLen;
	int32_t m_summaryMaxNumLines;
	int32_t m_summaryMaxNumCharsPerLine;

	bool m_getDocIdScoringInfo;

	// list of url patterns to be indexed.
	SafeBuf m_siteListBuf;

	// can be "web" "english" "romantic" "german" etc.
	SafeBuf m_urlFiltersProfile;

	// . now the url regular expressions
	// . we chain down the regular expressions
	// . if a url matches we use that tagdb rec #
	// . if it doesn't match any of the patterns, we use the default site #
	// . just one regexp per Pattern
	// . all of these arrays should be the same size, but we need to
	//   include a count because Parms.cpp expects a count before each
	//   array since it handle them each individually

	int32_t      m_numRegExs;
	// make this now use g_collectiondb.m_stringBuf safebuf and
	// make Parms.cpp use that stringbuf rather than store into here...
	SafeBuf		m_regExs[ MAX_FILTERS ];

	int32_t		m_numSpiderFreqs;	// useless, just for Parms::setParm()
	float		m_spiderFreqs[ MAX_FILTERS ];

	int32_t		m_numSpiderPriorities;	// useless, just for Parms::setParm()
	char		m_spiderPriorities[ MAX_FILTERS ];

	int32_t		m_numMaxSpidersPerRule;	// useless, just for Parms::setParm()
	int32_t		m_maxSpidersPerRule[ MAX_FILTERS ];

	// same ip waits now here instead of "page priority"
	int32_t		m_numSpiderIpWaits;	// useless, just for Parms::setParm()
	int32_t		m_spiderIpWaits[ MAX_FILTERS ];

	// same goes for max spiders per ip
	int32_t		m_numSpiderIpMaxSpiders;
	int32_t		m_spiderIpMaxSpiders [ MAX_FILTERS ];

	int32_t		m_numHarvestLinks;
	bool		m_harvestLinks[ MAX_FILTERS ];

	int32_t		m_numForceDelete;
	char		m_forceDelete[ MAX_FILTERS ];

	// dummy?
	int32_t      m_numRegExs9;

	bool m_doQueryHighlighting;

	char  m_summaryFrontHighlightTag[SUMMARYHIGHLIGHTTAGMAXSIZE] ;
	char  m_summaryBackHighlightTag [SUMMARYHIGHLIGHTTAGMAXSIZE] ;

	SafeBuf m_htmlRoot;
	SafeBuf m_htmlHead;
	SafeBuf m_htmlTail;

	class SpiderColl *m_spiderColl;
	GbMutex m_spiderCollMutex;

	int32_t m_overflow;
	int32_t m_overflow2;

	int32_t  m_maxAddUrlsPerIpDomPerDay;

	// . max content length of text/html or text/plain document
	// . we will not download, index or store more than this many bytes
	int32_t  m_maxTextDocLen;

	// . max content length of other (pdf, word, xls, ppt, ps)
	// . we will not download, index or store more than this many bytes
	// . if content would be truncated, we will not even download at all
	//   because the html converter needs 100% of the doc otherwise it
	//   will have an error
	int32_t  m_maxOtherDocLen;

	// . puts <br>s in the summary to keep its width below this
	// . but we exceed this width before we would split a word
	int32_t m_summaryMaxWidth;

	// how long a robots.txt can be in the cache (Msg13.cpp/Robotdb.cpp)
	int32_t m_maxRobotsCacheAge;

	int32_t m_crawlDelayDefaultForNoRobotsTxtMS;
	int32_t m_crawlDelayDefaultForRobotsTxtMS;


	// use query expansion for this collection?
	bool m_queryExpansion;
	// rewrite domain-like queries for this collection?
	bool m_modifyDomainLikeSearches;
	bool m_domainLikeSearchDisablesSiteCluster;
	// rewrite API-like queries?
	bool m_modifyAPILikeSearches;

	// read from cache
	bool m_rcache;

	bool m_hideAllClustered;

	// use this not m_bases to get the RdbBase
	class RdbBase *getBase(rdbid_t rdbId);

	// Rdb.cpp uses this after deleting an RdbBase and adding new one
	void           setBasePtr(rdbid_t rdbId, class RdbBase *base);

 private:
	// . now chuck this into CollectionRec instead of having a fixed
	//   array of them in Rdb.h called m_bases[]
	// . leave this out of any copy of course
	class RdbBase *m_bases[RDB_END2];

 public:
	// for poulating the sortbydate table
	class Msg5 *m_msg5;

	// each Rdb has a tree, so keep the pos/neg key count here so
	// that RdbTree does not have to have its own array limited by
	// MAX_COLLS which we did away with because we made this dynamic.
	int32_t m_numPosKeysInTree[RDB_END2];
	int32_t m_numNegKeysInTree[RDB_END2];
};

#endif // GB_COLLECTIONDB_H
