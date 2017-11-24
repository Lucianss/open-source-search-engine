// Matt Wells, copyright Apr 2009

// . 2. you can also call setTitleRec() and then call getMetaList()
// . this class is used by Repair.cpp and by Msg7 (inject) and SpiderLoop.cpp
// . Msg7 and Repair.cpp and injections can also set more than just 
//   m_firstUrl, like m_content, etc. or whatever elements are known, but
//   they must also set the corresponding "valid" flags of those elements
// . both methods must yield exactly the same result, the same "meta list"
// . after setting the contained classes XmlDoc::setMetaList() makes the list
//   of rdb records to be added to all the rdbs, this is the "meta list"
// . the meta list is made by hashing all the termIds/scores into some hash
//   tables in order to accumulate scores, then the hash table are serialized
//   into the "meta list"
// . the meta list is added to all rdbs with a simple call to 
//   Msg4::addMetaList(), which is only called by Msg14 or Repair.cpp for now


#ifndef GB_XMLDOC_H
#define GB_XMLDOC_H

#include "Lang.h"
#include "Words.h"
#include "Bits.h"
#include "Pos.h"
#include "Phrases.h"
#include "Xml.h"
#include "SafeBuf.h"
#include "Images.h"
#include "Sections.h"
#include "Msge0.h"
#include "Msge1.h"
#include "Msg4Out.h"

#include "SearchInput.h"
#include "Msg40.h"
#include "Msg0.h"
#include "Msg22.h"
#include "Tagdb.h"
#include "Url.h"
#include "Linkdb.h"
#include "MsgC.h"
#include "Msg13.h"
#include "RdbList.h"
#include "SiteGetter.h"
#include "Msg20.h"
#include "Matches.h"
#include "Query.h"
#include "Title.h"
#include "Summary.h"
#include "Spider.h" // SpiderRequest/SpiderReply definitions
#include "HttpMime.h" // ET_DEFLAT
#include "Json.h"
#include "Posdb.h"

// forward declaration
class GetMsg20State;

namespace GbDns {
	struct DnsResponse;
}

#define MAXFRAGWORDS 80000

#define MAX_TAG_PAIR_HASHES 100

#include "Msg40.h"

#define POST_VECTOR_SIZE   (32*4)

#define MAX_LINK_TEXT_LEN     512
#define MAX_SURROUNDING_TEXT_WIDTH 600
#define MAX_RSSITEM_SIZE  30000

bool getDensityRanks ( const int64_t *wids,
		       int32_t nw,
		       int32_t hashGroup ,
		       SafeBuf *densBuf ,
		       const Sections *sections);

// diversity vector
bool getDiversityVec ( const Words *words ,
		       const Phrases *phrases ,
		       class HashTableX *countTable ,
		       class SafeBuf *sbWordVec );

float computeSimilarity ( const int32_t   *vec0,
			  const int32_t   *vec1,
			  // corresponding scores vectors
			  const int32_t   *s0,
			  const int32_t   *s1,
			  class Query  *q    ,
			  // only Sections::addDateBasedImpliedSections()
			  // sets this to true right now. if set to true
			  // we essentially dedup each vector, although
			  // the score is compounded into the remaining 
			  // occurence. i'm not sure if that is the right
			  // behavior though.
			  bool dedupVecs = false );


// tell zlib to use our malloc/free functions
int gbuncompress(unsigned char *dest,
		 uint32_t *destLen,
		 const unsigned char *source,
		 uint32_t  sourceLen);

int gbcompress(unsigned char *dest,
	       uint32_t *destLen,
	       const unsigned char *source,
	       uint32_t  sourceLen);

// . for Msg13.cpp
// . *pend must equal \0
int32_t getContentHash32Fast ( unsigned char *p , int32_t plen ) ;

bool getWordPosVec ( const Words *words,
		     const Sections *sections,
		     int32_t startDist,
		     const char *fragVec,
		     SafeBuf *wpos );


#define ROOT_TITLE_BUF_MAX 512

class XmlDoc {

public:

	/// @warning Do NOT change. titlerec binary compatibility header

	//
	// BEGIN WHAT IS STORED IN THE TITLE REC (Titledb.h)
	//

	// headerSize = this->ptr_firstUrl - this->m_headerSize
	uint16_t  m_headerSize; 
	uint16_t  m_version;

	// these flags are used to indicate which ptr_ members are present:
	uint32_t  m_internalFlags1;
	int32_t      m_ip;
	int32_t      m_crawlDelay;

	// . use this to quickly detect if doc is unchanged
	// . we can avoid setting Xml and Words classes etc...
	int32_t      m_contentHash32;

	// this is a hash of all adjacent tag pairs for templated identificatn
	uint32_t  m_tagPairHash32;
	int32_t      m_siteNumInlinks;

	// this is non-zero if we decided not to index the doc
	int32_t m_indexCode;

	int32_t    m_reserved2;
	uint32_t   m_spideredTime; // time_t
	uint32_t  m_indexedTime; // slightly > m_spideredTime (time_t)
	uint32_t  m_reserved32;
	uint32_t  m_reserved33;
	uint32_t    m_firstIndexedDate; // time_t
	uint32_t    m_outlinksAddedDate; // time_t

	uint16_t  m_charset; // the ORIGINAL charset, we are always utf8!
	uint16_t  m_countryId;

	int32_t      m_reserved3;

	uint8_t   m_metaListCheckSum8; // bring it back!!
	char      m_reserved3b;
	uint16_t  m_bodyStartPos;
	uint16_t  m_reserved5;

	uint16_t  m_unused0;

	int16_t   m_httpStatus; // -1 if not found (empty http reply)
	
	int8_t    m_hopCount;
	uint8_t   m_langId;
	uint8_t   m_reserved6;
	uint8_t   m_contentType;


	// bit flags
	uint16_t  m_isRSS:1;
	uint16_t  m_isPermalink:1;
	uint16_t  m_isAdult:1;
	uint16_t  m_wasContentInjected:1;
	uint16_t  m_spiderLinks:1;
	uint16_t  m_isContentTruncated:1;
	uint16_t  m_isLinkSpam:1;
	uint16_t  m_reserved796:1;
	uint16_t  m_reserved797:1;
	uint16_t  m_reserved798:1;
	uint16_t  m_reserved799:1;
	uint16_t  m_isSiteRoot:1;

	uint16_t  m_reserved800:1;
	uint16_t  m_reserved801:1;
	uint16_t  m_reserved802:1;
	uint16_t  m_useTimeAxis:1;
	uint16_t  m_reserved805:1;
	uint16_t  m_reserved806:1;
	uint16_t  m_reserved807:1;
	uint16_t  m_reserved808:1;
	uint16_t  m_reserved809:1;
	uint16_t  m_reserved810:1;
	uint16_t  m_reserved811:1;
	uint16_t  m_reserved812:1;
	uint16_t  m_reserved813:1;
	uint16_t  m_reserved814:1;
	uint16_t  m_reserved815:1;
	uint16_t  m_reserved816:1;

	//end of titlerec binary compatibility header

	/// @warning Do NOT change the structure of the following until m_dummyEnd.
	/// check in XmlDoc::set2 (sanity check. must match exactly)

	char      *ptr_firstUrl;
	char      *ptr_redirUrl;
	char      *ptr_rootTitleBuf;
	int32_t      *ptr_unused12;
	int32_t      *ptr_unused13;
	void      *ptr_unused8;
	int64_t *ptr_unused10;
	float  *ptr_unused11;
	char      *ptr_imageData;
	int32_t      *ptr_unused6;
	int32_t      *ptr_unused7;
	char      *ptr_unused1;
	char      *ptr_unused2;
	char      *ptr_unused3;
	char      *ptr_utf8Content;
	char      *ptr_unused5;

	// do not let SiteGetter change this when we re-parse!
	char      *ptr_site;
	LinkInfo  *ptr_linkInfo1;
	char      *ptr_linkdbData;
	char      *ptr_unused14;
	char      *ptr_tagRecData;
	LinkInfo  *ptr_unused9;

	int32_t       size_firstUrl;
	int32_t       size_redirUrl;
	int32_t       size_rootTitleBuf;
	int32_t       size_unused12;
	int32_t       size_unused13;
	int32_t       size_unused8;
	int32_t       size_unused10;
	int32_t       size_unused11;
	int32_t       size_imageData;
	int32_t       size_unused6;
	int32_t       size_unused7;
	int32_t       size_unused1;
	int32_t       size_unused2;
	int32_t       size_unused3;
	int32_t       size_utf8Content;
	int32_t       size_unused5;
	int32_t       size_site;
	int32_t       size_linkInfo1;
	int32_t       size_linkdbData;
	int32_t       size_unused14;
	int32_t       size_tagRecData;
	int32_t       size_unused9;

	char      m_dummyEnd;

	//
	// END WHAT IS STORED IN THE TITLE REC (Titledb.h)
	//

	char		*ptr_scheme;	
	int32_t		size_scheme;	


 public:
	bool set2 ( char *titleRec,
		    int32_t maxSize, 
		    const char *coll,
		    class SafeBuf *p,
		    int32_t niceness ,
		    class SpiderRequest *sreq = NULL );

	// . since being set from a docId, we will load the old title rec
	//   and use that!
	// . used by PageGet.cpp
	bool set3 ( int64_t  docId       , 
		    const char   *coll        ,
		    int32_t       niceness    );

	bool set4 ( class SpiderRequest *sreq  , 
		    const key96_t       *doledbKey,
		    const char      *coll      ,
		    class SafeBuf   *pbuf      , 
		    int32_t          niceness  ,
		    char            *utf8Content = NULL ,
		    bool             deleteFromIndex = false ,
		    int32_t             forcedIp = 0 ,
		    uint8_t          contentType = CT_HTML ,
		    uint32_t           spideredTime = 0 , // time_t
		    bool             contentHasMime = false );

	// we now call this right away rather than at download time!
	int32_t getSpideredTime();

	// time right before adding the termlists to the index, etc.
	// whereas spider time is the download time
	int32_t getIndexedTime();

	// another entry point, like set3() kinda
	bool loadFromOldTitleRec ();

	XmlDoc() ; 
	~XmlDoc() ; 
	void nukeDoc ( class XmlDoc *);
	void reset ( ) ;
	bool setFirstUrl ( const char *u ) ;
	void setStatus ( const char *s ) ;
	void setCallback ( void *state, void (*callback) (void *state) ) ;
	void setCallback ( void *state, bool (*callback) (void *state) ) ;
	void getRevisedSpiderRequest ( class SpiderRequest *revisedReq );
	void getRebuiltSpiderRequest ( class SpiderRequest *sreq ) ;
	bool indexDoc ( );
	bool indexDoc2 ( );

	char *prepareToMakeTitleRec ( ) ;
	// store TitleRec into "buf" so it can be added to metalist
	bool setTitleRecBuf ( SafeBuf *buf , int64_t docId, int64_t uh48 );
	// sets m_titleRecBuf/m_titleRecBufValid/m_titleRecKey[Valid]
	SafeBuf *getTitleRecBuf ( );

	char *getIsAdult ( ) ;

	bool *checkBlockList();

	bool *parseRobotsMetaTag();
	void parseRobotsMetaTagContent(const char *content, int32_t contentLen);

	char *getIsPermalink ( ) ;
	char *getIsUrlPermalinkFormat ( ) ;
	char *getIsRSS ( ) ;
	bool *getIsSiteMap ( ) ;
	class Xml *getXml ( ) ;
	uint8_t *getLangVector ( ) ;	
	uint8_t *getLangId ( ) ;

	lang_t getSummaryLangIdCLD2();

	lang_t getContentLangIdCLD2();
	lang_t getContentLangIdCLD3();

	uint8_t computeLangId ( Sections *sections ,Words *words , char *lv ) ;
	class Words *getWords ( ) ;
	class Bits *getBits ( ) ;
	class Bits *getBitsForSummary ( ) ;
	class Pos *getPos ( );
	class Phrases *getPhrases ( ) ;
	class Sections *getSections ( ) ;
	int32_t *getLinkSiteHashes ( );
	class Links *getLinks ( bool doQuickSet = false ) ;
	class HashTableX *getCountTable ( ) ;
	bool hashString_ct ( class HashTableX *ht, char *s , int32_t slen ) ;
	int32_t *getSummaryVector ( ) ;
	int32_t *getPageSampleVector ( ) ;
	int32_t *getPostLinkTextVector ( int32_t linkNode ) ;
	int32_t computeVector ( class Words *words, uint32_t *vec , int32_t start = 0 , int32_t end = -1 );
	float *getPageSimilarity ( class XmlDoc *xd2 ) ;
	float *getPercentChanged ( );
	int64_t *getExactContentHash64();
	class RdbList *getDupList ( ) ;
	char *getIsDup ( ) ;
	char *getMetaDescription( int32_t *mdlen ) ;
	char *getMetaSummary ( int32_t *mslen ) ;
	char *getMetaKeywords( int32_t *mklen ) ;
	char *getMetaGeoPlacename( int32_t *mgplen );

	class Url *getCurrentUrl ( ) ;
	class Url *getFirstUrl() ;
	int64_t getFirstUrlHash48();
	int64_t getFirstUrlHash64();
	class Url **getRedirUrl() ;
	class Url **getMetaRedirUrl() ;
	class Url *getCanonicalUrl();
	class Url **getCanonicalRedirUrl ( ) ;
	int32_t *getFirstIndexedDate ( ) ;
	int32_t *getOutlinksAddedDate ( ) ;
	uint16_t *getCountryId ( ) ;
	class XmlDoc **getOldXmlDoc ( ) ;
	class XmlDoc **getExtraDoc(const char *url, int32_t maxCacheAge = 0);
	bool getIsPageParser ( ) ;
	class XmlDoc **getRootXmlDoc ( int32_t maxCacheAge = 0 ) ;
	char **getOldTitleRec ( );
	char **getRootTitleRec ( ) ;
	int64_t *getDocId ( ) ;
	char *getIsIndexed ( ) ;
	class TagRec *getTagRec ( ) ;
	// non-dup/nondup addresses only
	int32_t *getFirstIp ( ) ;
	int32_t *getSiteNumInlinks ( ) ;
	class LinkInfo *getSiteLinkInfo() ;
	int32_t *getIp ( ) ;
	std::vector<std::string>* getHostNameServers(const char *hostname, size_t hostnameLen);
	static void gotHostNameServersWrapper(GbDns::DnsResponse *response, void *state);
	static void gotIpWrapper(GbDns::DnsResponse *response, void *state);
	bool *getIsAllowed ( ) ;
	int32_t *getFinalCrawlDelay();
	int32_t      m_finalCrawlDelay;
	char *getIsWWWDup ( ) ;
	class LinkInfo *getLinkInfo1 ( ) ;
	char *getSite ( ) ;
	const char *getScheme ( ) ;
	
	void  gotSite ( ) ;
	int32_t *getSiteHash32 ( ) ;
	char **getHttpReply ( ) ;
	char **getHttpReply2 ( ) ;
	char **gotHttpReply ( ) ;
	char *getIsContentTruncated ( );
	int32_t *getDownloadStatus ( ) ;
	int64_t *getDownloadEndTime ( ) ;
	int16_t *getHttpStatus ( );
	class HttpMime *getMime () ;
	char **getContent ( ) ;
	uint8_t *getContentType ( ) ;
	uint16_t *getCharset ( ) ;
	char **getFilteredContent ( ) ;
	void filterStart_r ( bool amThread ) ;
	char **getRawUtf8Content ( ) ;
	char **getExpandedUtf8Content ( ) ;
	char **getUtf8Content ( ) ;
	// we download large files to a file on disk, like warcs and arcs
	int32_t *getContentHash32 ( ) ;
	int32_t *getContentHashJson32 ( ) ;
	int32_t     *getTagPairHashVector ( ) ;
	uint32_t *getTagPairHash32 ( ) ;
	int32_t getHostHash32a ( ) ;
	int32_t getDomHash32 ( );
	char **getThumbnailData();
	class Images *getImages ( ) ;
	class TagRec ***getOutlinkTagRecVector () ;
	int32_t **getOutlinkFirstIpVector () ;
	char *getIsSiteRoot ( ) ;
	int8_t *getHopCount ( ) ;
	char *getSpiderLinks ( ) ;
	bool getIsInjecting();
	int32_t *getSpiderPriority ( ) ;
	int32_t *getIndexCode ( ) ;
	SafeBuf *getNewTagBuf ( ) ;

	void logIt ( class SafeBuf *bb = NULL ) ;
	bool m_doConsistencyTesting;
	bool doConsistencyTest ( bool forceTest ) ;

	void printMetaList() const;
	void printMetaList ( char *metaList , char *metaListEnd ,
			     class SafeBuf *pbuf );
	bool verifyMetaList ( char *p , char *pend , bool forDelete ) ;
	bool hashMetaList ( class HashTableX *ht        ,
			    char       *p         ,
			    char       *pend      ,
			    bool        checkList ) ;

	char *getMetaList ( bool forDelete = false );

	uint64_t m_downloadStartTime;

	uint64_t m_ipStartTime;
	uint64_t m_ipEndTime;

	bool m_updatedMetaData;

	void copyFromOldDoc ( class XmlDoc *od ) ;

	class SpiderReply *getFakeSpiderReply ( );

	// we add a SpiderReply to spiderdb when done spidering, even if
	// m_indexCode or g_errno was set!
	class SpiderReply *getNewSpiderReply ( );

	void  setSpiderReqForMsg20 ( class SpiderRequest *sreq , 
				     class SpiderReply   *srep );


	char *addOutlinkSpiderRecsToMetaList ( );

	int32_t getSiteRank ();
	bool addTable144 ( class HashTableX *tt1 , 
			   int64_t docId ,
			   class SafeBuf *buf = NULL );

	bool addTable224 ( HashTableX *tt1 ) ;

	bool hashNoSplit ( class HashTableX *tt ) ;
	char *hashAll ( class HashTableX *table ) ;
	bool hashMetaTags ( class HashTableX *table ) ;
	bool hashContentType ( class HashTableX *table ) ;
	
	bool hashLinks ( class HashTableX *table ) ;
	bool getUseTimeAxis ( ) ;
	SafeBuf *getTimeAxisUrl ( );
	bool hashUrl ( class HashTableX *table, bool urlOnly );
	bool hashDateNumbers ( class HashTableX *tt );
	bool hashIncomingLinkText(HashTableX *table);
	bool hashLinksForLinkdb ( class HashTableX *table ) ;
	bool hashNeighborhoods ( class HashTableX *table ) ;
	bool hashTitle ( class HashTableX *table );
	bool hashBody2 ( class HashTableX *table );
	bool hashMetaKeywords ( class HashTableX *table );
	bool hashMetaGeoPlacename( class HashTableX *table );
	bool hashMetaSummary ( class HashTableX *table );
	bool hashLanguage ( class HashTableX *table ) ;
	bool hashLanguageString ( class HashTableX *table ) ;
	bool hashCountry ( class HashTableX *table ) ;

	class Url *getBaseUrl ( ) ;

	void setMsg20Request(Msg20Request *req);
	class Msg20Reply *getMsg20Reply ( ) ;
	class Msg20Reply *getMsg20ReplyStepwise();
	void loopUntilMsg20ReplyReady(GetMsg20State *);
	static void getMsg20ReplyThread(void *pv);
	void getMsg20ReplyThread();
	static void msg20Done(void *pv, job_exit_t exit_type);
	void msg20Done(job_exit_t exit_type);
	Query *getQuery() ;
	Matches *getMatches () ;
	char *getDescriptionBuf ( char *displayMetas , int32_t *dlen ) ;
	SafeBuf *getHeaderTagBuf();
	class Title *getTitle ();
	class Summary *getSummary () ;
	char *getHighlightedSummary ( bool *isSetFromTagsPtr );
	bool *getIsNoArchive();
	bool *getIsNoFollow();
	bool *getIsNoIndex();
	bool *getIsNoSnippet();
	int32_t *getUrlFilterNum();
	char *getIsLinkSpam ( ) ;
	char *getIsErrorPage ( ) ;
	const char* matchErrorMsg(char* p, char* pend );

	bool hashWords( class HashInfo *hi );
	bool hashSingleTerm( const char *s, int32_t slen, class HashInfo *hi );
	bool hashString( char *s, int32_t slen, class HashInfo *hi );

	bool hashWords3( class HashInfo *hi, const Words *words, class Phrases *phrases,
					 class Sections *sections, class HashTableX *countTable, char *fragVec, char *wordSpamVec,
					 char *langVec, class HashTableX *wts, class SafeBuf *wbuf );

	bool hashString3( char *s, int32_t slen, class HashInfo *hi, class HashTableX *countTable,
			  class HashTableX *wts, class SafeBuf *wbuf);

	bool hashNumberForSorting( const char *beginBuf ,
			  const char *buf , 
			  int32_t bufLen , 
			  class HashInfo *hi ) ;

	bool hashNumberForSortingAsInt32 ( int32_t x,
			   class HashInfo *hi ,
			   const char *gbsortByStr ) ;

	// print out for PageTitledb.cpp and PageParser.cpp
	bool printDoc ( class SafeBuf *pbuf );
	bool printMenu ( class SafeBuf *pbuf );
	bool printDocForProCog ( class SafeBuf *sb , HttpRequest *hr ) ;
	bool printGeneralInfo ( class SafeBuf *sb , HttpRequest *hr ) ;
	bool printRainbowSections ( class SafeBuf *sb , HttpRequest *hr );
	bool printSiteInlinks ( class SafeBuf *sb , HttpRequest *hr );
	bool printPageInlinks ( class SafeBuf *sb , HttpRequest *hr );
	bool printTermList ( class SafeBuf *sb , HttpRequest *hr );
	bool printSpiderStats ( class SafeBuf *sb , HttpRequest *hr );
	bool printCachedPage ( class SafeBuf *sb , HttpRequest *hr );

	void printTermList() const;

	char *getTitleBuf             ( );
	char *getRootTitleBuf         ( );
	char *getFilteredRootTitleBuf ( );

 public:

	// stuff set from the key of the titleRec, above the compression area
	int64_t m_docId;

	char     *m_ubuf;
	int32_t      m_ubufSize;
	int32_t      m_ubufAlloc;

	// private:

	// we we started spidering it, in milliseconds since the epoch
	int64_t    m_startTime;
	int64_t    m_injectStartTime;

	class XmlDoc *m_prevInject;
	class XmlDoc *m_nextInject;

	// when set() was called by Msg20.cpp so we can time how long it took
	// to generate the summary
	int64_t    m_setTime;
	int64_t    m_cpuSummaryStartTime;

	// . these should all be set using set*() function calls so their
	//   individual validity flags can bet set to true, and successive
	//   calls to their corresponding get*() functions will not core
	// . these particular guys are set immediately on set(char *titleRec)

	Url        m_redirUrl;
	Url       *m_redirUrlPtr;
	SafeBuf    m_redirCookieBuf;
	Url        m_metaRedirUrl;
	Url       *m_metaRedirUrlPtr;
	Url        m_canonicalUrl;
	Url       *m_canonicalRedirUrlPtr;
	int32_t       m_redirError;
	bool       m_allowSimplifiedRedirs;
	Url        m_firstUrl;
	int64_t  m_firstUrlHash48;
	int64_t  m_firstUrlHash64;
	Url        m_currentUrl;

	collnum_t      m_collnum;
	class CollectionRec *getCollRec ( ) ;
	bool setCollNum ( const char *coll ) ;


	char      *m_content;
	int32_t       m_contentLen;

	char *m_metaList;
	int32_t  m_metaListSize;

	int32_t m_addedSpiderRequestSize;
	int32_t m_addedSpiderReplySize;

	SafeBuf  m_metaList2;

	// used by msg7 to store udp slot
	class UdpSlot *m_injectionSlot;

	// . same thing, a little more complicated
	// . these classes are only set on demand
	Xml        m_xml;
	Links      m_links;
	Words      m_words;
	Bits       m_bits;
	Bits       m_bits2;
	Pos        m_pos;
	Phrases    m_phrases;
	SafeBuf    m_synBuf;
	Sections   m_sections;

	// . for rebuild logging of what's changed
	// . Repair.cpp sets these based on titlerec
	char m_logLangId;
	int32_t m_logSiteNumInlinks;

	SafeBuf m_timeAxisUrl;

	bool isFirstUrlRobotsTxt();
	bool m_isRobotsTxtUrl;

	bool isFirstUrlCanonical();
	bool m_isUrlCanonical;

	Images     m_images;
	HashTableX m_countTable;
	HttpMime   m_mime;
	TagRec     m_tagRec;
	SafeBuf    m_tagRecBuf;
	SafeBuf    m_newTagBuf;
	SafeBuf    m_fragBuf;
	SafeBuf    m_wordSpamBuf;
	SafeBuf    m_finalSummaryBuf;
	bool m_isFinalSummarySetFromTags;
	int32_t       m_firstIp;

	class SafeBuf     *m_savedSb;
	class HttpRequest *m_savedHr;

	// validity flags. on reset() all these are set to false.
	char     m_VALIDSTART;
	// DO NOT add validity flags above this line!
	bool m_metaListValid;
	bool m_addedSpiderRequestSizeValid;
	bool m_addedSpiderReplySizeValid;
	bool m_downloadStartTimeValid;
	bool m_siteValid;
	bool m_startTimeValid;
	bool m_currentUrlValid;
	bool m_useTimeAxisValid;
	bool m_timeAxisUrlValid;
	bool m_firstUrlValid;
	bool m_firstUrlHash48Valid;
	bool m_firstUrlHash64Valid;
	bool m_docIdValid;
	bool m_tagRecValid;
	bool m_robotsTxtLenValid;
	bool m_tagRecDataValid;
	bool m_newTagBufValid;
	bool m_rootTitleBufValid;
	bool m_filteredRootTitleBufValid;
	bool m_titleBufValid;
	bool m_fragBufValid;
	bool m_isRobotsTxtUrlValid;
	bool m_isUrlCanonicalValid;
	bool m_wordSpamBufValid;
	bool m_finalSummaryBufValid;

	bool m_hopCountValid;
	bool m_isInjectingValid;
	bool m_isImportingValid;
	bool m_metaListCheckSum8Valid;
	bool m_contentValid;
	bool m_filteredContentValid;
	bool m_charsetValid;
	bool m_langVectorValid;
	bool m_langIdValid;
	bool m_isRSSValid;
	bool m_isSiteMapValid;
	bool m_isContentTruncatedValid;
	bool m_xmlValid;
	bool m_linksValid;
	bool m_wordsValid;
	bool m_bitsValid;
	bool m_bits2Valid;
	bool m_posValid;
	bool m_phrasesValid;
	bool m_sectionsValid;

	bool m_imageDataValid;
	bool m_imagesValid;
	bool m_sreqValid;
	bool m_srepValid;

	bool m_ipValid;
	bool m_firstIpValid;
	bool m_spideredTimeValid;
	bool m_indexedTimeValid;
	bool m_isInIndexValid;
	bool m_wasInIndexValid;
	bool m_outlinksAddedDateValid;
	bool m_countryIdValid;
	bool m_bodyStartPosValid;

	bool m_httpStatusValid;
	bool m_crawlDelayValid;
	bool m_finalCrawlDelayValid;
	bool m_titleRecKeyValid;
	bool m_versionValid;
	bool m_rawUtf8ContentValid;
	bool m_expandedUtf8ContentValid;
	bool m_utf8ContentValid;
	bool m_isAllowedValid;
	bool m_redirUrlValid;
	bool m_redirCookieBufValid;
	bool m_metaRedirUrlValid;
	bool m_canonicalUrlValid;
	bool m_canonicalRedirUrlValid;
	bool m_statusMsgValid;
	bool m_mimeValid;
	bool m_hostHash32aValid;
	bool m_indexCodeValid;
	bool m_priorityValid;
	bool m_downloadStatusValid;
	bool m_downloadEndTimeValid;
	bool m_redirErrorValid;
	bool m_domHash32Valid;
	bool m_contentHash32Valid;
	bool m_tagPairHash32Valid;

	bool m_spiderLinksValid;
	bool m_firstIndexedDateValid;
	bool m_isPermalinkValid;

	bool m_isAdultValid;
	bool m_isUrlPermalinkFormatValid;
	bool m_percentChangedValid;
	bool m_unchangedValid;
	bool m_countTableValid;
	bool m_tagPairHashVecValid;
	bool m_summaryVecValid;
	bool m_pageSampleVecValid;
	bool m_postVecValid;
	bool m_dupListValid;
	bool m_isDupValid;
	bool m_metaDescValid;
	bool m_metaSummaryValid;
	bool m_metaKeywordsValid;
	bool m_metaGeoPlacenameValid;
	bool m_oldDocValid;
	bool m_extraDocValid;
	bool m_rootDocValid;
	bool m_oldTitleRecValid;
	bool m_rootTitleRecValid;
	bool m_isIndexedValid;
	bool m_siteNumInlinksValid;
	bool m_siteLinkInfoValid;
	bool m_isWWWDupValid;
	bool m_linkInfo1Valid;
	bool m_linkSiteHashesValid;
	bool m_siteHash32Valid;
	bool m_httpReplyValid;
	bool m_contentTypeValid;
	bool m_outlinkTagRecVectorValid;
	bool m_outlinkIpVectorValid;
	bool m_isSiteRootValid;
	bool m_wasContentInjectedValid;
	bool m_outlinkHopCountVectorValid;
	bool m_urlFilterNumValid;
	bool m_numOutlinksAddedValid;
	bool m_baseUrlValid;
	bool m_replyValid;
	bool m_isPageParserValid;
	bool m_imageUrlValid;
	bool m_imageUrl2Valid;
	bool m_queryValid;
	bool m_matchesValid;
	bool m_dbufValid;
	bool m_titleValid;
	bool m_htbValid;
	bool m_collnumValid;
	bool m_summaryValid;
	bool m_titleRecBufValid;
	bool m_isLinkSpamValid;
	bool m_isErrorPageValid;
	bool m_exactContentHash64Valid;
	bool m_jpValid;
	bool m_blockedDocValid;
	bool m_hostNameServersValid;
	bool m_isSiteMap;

	// shadows
	char m_isRSS2;
	char m_isPermalink2;
	char m_isAdult2;
	char m_spiderLinks2;		// May be -1
	char m_isContentTruncated2;
	char m_isLinkSpam2;
	char m_isSiteRoot2;

	// DO NOT add validity flags below this line!
	char     m_VALIDEND;

	bool m_printedMenu;
	char m_isUrlPermalinkFormat;
	int32_t m_tagPairHashVec[MAX_TAG_PAIR_HASHES];
	int32_t m_tagPairHashVecSize;
	int32_t m_summaryVec [SAMPLE_VECTOR_SIZE/4];
	int32_t m_summaryVecSize;
	int32_t m_pageSampleVec[SAMPLE_VECTOR_SIZE/4];
	int32_t m_pageSampleVecSize;
	int32_t m_postVec[POST_VECTOR_SIZE/4];
	int32_t m_postVecSize;
	float m_pageSimilarity;
	float m_percentChanged;
	bool  m_unchanged;
	// what docids are similar to us? docids are in this list
	RdbList m_dupList;
	int64_t m_exactContentHash64;
	Msg0 m_msg0;
	char m_isDup;	// may be -1
	int64_t m_docIdWeAreADupOf;
	Msg22Request m_msg22Request;
	Msg22 m_msg22a;
	Msg22 m_msg22b;
	Msg22 m_msg22e;
	Msg22 m_msg22f;
	// these now reference directly into the html src so our 
	// WordPosInfo::m_wordPtr algo works in seo.cpp
	char *m_metaDesc;
	int32_t  m_metaDescLen;
	char *m_metaSummary;
	int32_t  m_metaSummaryLen;
	char *m_metaKeywords;
	int32_t  m_metaKeywordsLen;
	
	char *m_metaGeoPlacename;
	int32_t  m_metaGeoPlacenameLen;

	class XmlDoc *m_oldDoc;
	class XmlDoc *m_extraDoc;
	class XmlDoc *m_rootDoc;
	char   *m_oldTitleRec;
	int32_t    m_oldTitleRecSize;
	char   *m_rootTitleRec;
	int32_t    m_rootTitleRecSize;
	char    m_isIndexed;	// may be -1

	// confusing, i know! these are used exclsusively by
	// getNewSpiderReply() for now
	bool m_isInIndex;
	bool m_wasInIndex;

	Msg8a   m_msg8a;

	Url   m_extraUrl;
	SafeBuf m_mySiteLinkInfoBuf;
	SafeBuf m_myPageLinkInfoBuf;

	bool m_isInjecting;
	bool m_isImporting;
	bool m_useFakeMime;
	bool m_useSiteLinkBuf;
	bool m_usePageLinkBuf;
	bool m_printInXml;

	SafeBuf m_tmpBuf11;
	SafeBuf m_tmpBuf12;
	Multicast m_mcast11;
	Multicast m_mcast12;
	bool m_isAllowed;
	bool m_isChildDoc;
	Msg13 m_msg13;
	Msg13Request m_msg13Request;
	bool m_isSpiderProxy;
	// for limiting # of iframe tag expansions
	int32_t m_numExpansions;
	char m_newOnly;
	bool m_skipContentHashCheck;
	char m_isWWWDup;	// May be -1

	SafeBuf m_linkSiteHashBuf;
	SafeBuf m_linkdbDataBuf;
	SafeBuf m_langVec;

	SiteGetter m_siteGetter;
	int32_t m_siteHash32;
	char *m_httpReply;
	bool m_useRobotsTxt;
	int32_t m_robotsTxtLen;
	bool m_robotsTxtHttpStatusDisallowed;
	bool m_robotsTxtErrorDisallowed;
	int32_t m_httpReplySize;
	int32_t m_httpReplyAllocSize;
	char *m_filteredContent;
	int32_t m_filteredContentLen;
	int32_t m_filteredContentAllocSize;
	int32_t m_filteredContentMaxSize;
	bool m_calledThread;
	int32_t m_errno;
	int32_t m_hostHash32a;
	int32_t m_domHash32;

	Msge0 m_msge0;
	Msge1 m_msge1;

	Json *getParsedJson();
	// object that parses the json
	Json m_jp;

	// flow flags

	bool m_computedMetaListCheckSum;

	// cachedb related args
	bool    m_allHashed;

	int8_t *m_outlinkHopCountVector;
	int32_t  m_outlinkHopCountVectorSize;
	int32_t m_urlFilterNum;
	int32_t m_numOutlinksAdded;
	int32_t m_numRedirects;
	bool m_isPageParser;
	Url m_baseUrl;
	Msg20Reply m_reply;
	Msg20Request *m_req;
	bool m_abortMsg20Generation;
	char  m_linkTextBuf[MAX_LINK_TEXT_LEN];
	char m_surroundingTextBuf[MAX_SURROUNDING_TEXT_WIDTH];
	char m_rssItemBuf[MAX_RSSITEM_SIZE];

	const char *m_note;
	Query m_query;
	Matches m_matches;
	// meta description buf
	int32_t m_dbufSize;
	char m_dbuf[1024];
	SafeBuf m_htb;
	Title m_title;
	Summary m_summary;
	char m_isErrorPage;		// May be -1

	// stuff
	int64_t m_lastTimeStart;
	const char *m_statusMsg;
	Msg4  m_msg4;

	bool  m_deleteFromIndex;

	// ptrs to stuff
	SafeBuf m_titleRecBuf;
	key96_t   m_titleRecKey;

	// for isDupOfUs()
	char *m_dupTrPtr;
	int32_t  m_dupTrSize;

	key96_t     m_doledbKey;
	SpiderRequest m_sreq;
	SpiderReply   m_srep;//newsr;

	// bool flags for what procedures we have done
	bool m_checkedUrlFilters;
	
	bool m_listAdded                ;
	bool m_check1                   ;
	bool m_check2                   ;
	bool m_prepared                 ;
	bool m_copied1                  ;
	bool m_updatingSiteLinkInfoTags ;

	bool m_didDelay                 ;
	bool m_didDelayUnregister       ;
	bool m_calledMsg22e             ;
	bool m_calledMsg22f             ;
	bool m_calledMsg25              ;
	bool m_calledSections           ;
	bool m_loaded                   ;

	bool m_doingConsistencyCheck ;

	int32_t m_dist;

	// use to store a \0 list of "titles" of the root page so we can
	// see which if any are the venue name, and thus match that to
	// addresses of the venue on the site, and we can use those addresses
	// as default venue addresses when no venues are listed on a page
	// on that site.
	char   m_rootTitleBuf[ROOT_TITLE_BUF_MAX];
	int32_t   m_rootTitleBufSize;

	// . this is filtered
	// . certain punct is replaced with \0
	char   m_filteredRootTitleBuf[ROOT_TITLE_BUF_MAX];
	int32_t   m_filteredRootTitleBufSize;

	// like m_rootTitleBuf but for the current page
	char   m_titleBuf[ROOT_TITLE_BUF_MAX];
	int32_t   m_titleBufSize;

	bool m_setTr                    ;

	void (* m_masterLoop) ( void *state );
	void  * m_masterState;

	void (* m_callback1) ( void *state );	
	bool (* m_callback2) ( void *state );	
	void  *m_state;

	// the spider priority
	int32_t m_priority;

	// the download error, like ETIMEDOUT, ENOROUTE, etc.
	int32_t m_downloadStatus;

	// . when the download was completed. will be zero if no download done
	// . used to set SpiderReply::m_downloadEndTime because we need
	//   high resolution for that so we can dole out the next spiderrequest
	//   from that IP quickly if the sameipwait is like 500ms.
	int64_t m_downloadEndTime;

	int32_t  m_metaListAllocSize;
	char *m_p;
	char *m_pend;

	int32_t  m_maxCacheAge;

	bool          m_hashedTitle;
	bool          m_hashedMetas;

	int32_t          m_niceness;

	bool m_usePosdb     ;
	bool m_useClusterdb ;
	bool m_useLinkdb    ;
	bool m_useSpiderdb  ;
	bool m_useTitledb   ;
	bool m_useTagdb     ;
	bool m_useSecondaryRdbs ;

	SafeBuf *m_pbuf;

	// store termlist into here if non-null
	bool     m_storeTermListInfo;
	char     m_sortTermListBy;

	// store the terms that we hash into this table so that PageParser.cpp
	// can print what was hashed and with what score and what description
	class HashTableX *m_wts;
	HashTableX m_wtsTable;
	SafeBuf m_wbuf;

	// Msg25.cpp stores its pageparser.cpp output into this one
	SafeBuf m_pageLinkBuf;
	SafeBuf m_siteLinkBuf;

	// which set() function was called above to set us?
	bool          m_setFromTitleRec;
	bool          m_setFromSpiderRec;
	bool          m_setFromUrl;
	bool          m_setFromDocId;
	bool          m_freeLinkInfo1;
	bool          m_contentInjected;

	bool          m_recycleContent;

	char *m_rawUtf8Content;
	int32_t  m_rawUtf8ContentSize;
	int32_t  m_rawUtf8ContentAllocSize; // we overallocate sometimes
	char *m_expandedUtf8Content;
	int32_t  m_expandedUtf8ContentSize;
	char *m_savedp;
	char *m_oldp;
	bool  m_didExpansion;
	SafeBuf m_esbuf;

	// used by msg13
	class Msg13Request *m_r;

	bool m_freed;

	bool m_indexedDoc; //indexDoc() perfomrned completely

	bool m_msg4Waiting;
	bool m_msg4Launched;

	bool m_blockedDoc;
	bool m_checkedUrlBlockList;
	bool m_checkedDnsBlockList;

	bool m_parsedRobotsMetaTag;
	bool m_robotsNoIndex;
	bool m_robotsNoFollow;
	bool m_robotsNoArchive;
	bool m_robotsNoSnippet;


	std::vector<std::string> m_hostNameServers;

	// word spam detection
	char *getWordSpamVec ( );
	bool setSpam ( const int32_t *profile, int32_t plen , int32_t numWords ,
		       unsigned char *spam );
	int32_t  getProbSpam  ( const int32_t *profile, int32_t plen , int32_t step );
	bool m_isRepeatSpammer;
	int32_t m_numRepeatSpam;

	// frag vector (repeated fragments). 0 means repeated, 1 means not.
	// vector is 1-1 with words in the document body.
	char *getFragVec ( );

	bool injectDoc ( const char *url ,
			 class CollectionRec *cr ,
			 char *content ,
			 bool contentHasMime ,
			 int32_t hopCount,
			 int32_t charset,
			 int32_t langId,
			 bool deleteUrl,
			 const char *contentTypeStr, // text/html, text/xml etc.
			 bool spiderLinks ,
			 char newOnly, // index iff new
			 bool skipContentHashCheck,
			 void *state,
			 void (*callback)(void *state) ,

			 uint32_t firstIndexedTime = 0,
			 uint32_t lastSpideredDate = 0 ,
			 int32_t  injectDocIp = 0 );

	int64_t logQueryTimingStart();
	void logQueryTimingEnd(const char* function, int64_t startTime);

	void callCallback();
};

// . PageParser.cpp uses this class for printing hashed terms out by calling
//   XmlDoc::print()
// . we store TermInfos into XmlDoc::m_wtsTable, a HashTableX
// . one for each term hashed
// . the key is the termId. dups are allowed
// . the term itself is stored into a separate buffer, m_wbuf, a SafeBuf, so
//   that TermInfo::m_term will reference that and it won't disappear on us
class TermDebugInfo {
 public:
	int32_t      m_termOff;
	int32_t      m_termLen;
	int32_t      m_descOff;   // the description offset
	int32_t      m_prefixOff; // the prefix offset, like "site" or "gbadid"
	int64_t m_termId;
	int32_t      m_date;
	bool      m_shardByTermId;

	char      m_langId;
	char      m_diversityRank;
	char      m_densityRank;
	char      m_wordSpamRank;
	char      m_hashGroup;
	int32_t      m_wordNum;
	int32_t      m_wordPos;
	posdbkey_t  m_key; // key144_t
	// 0 = not a syn, 1 = syn from presets,2=wikt,3=generated
	char      m_synSrc;
	int64_t  m_langBitVec64;
};

#endif // GB_XMLDOC_H
