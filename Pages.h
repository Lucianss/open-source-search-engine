// Copyright Matt Wells Sep 2004

// used for sending all dynamic html pages

#ifndef GB_PAGES_H
#define GB_PAGES_H

#include <inttypes.h>
#include <stddef.h>

class TcpSocket;
class HttpRequest;
class SafeBuf;
class CollectionRec;

bool printRedBox2 ( SafeBuf *sb , 
		    class TcpSocket *sock , 
		    class HttpRequest *hr );

bool printRedBox  ( SafeBuf *mb , 
		    class TcpSocket *sock , 
		    class HttpRequest *hr );

#define GOLD "f3c734"

#define LIGHTER_BLUE "e8e8ff"
#define LIGHT_BLUE "d0d0e0"
#define DARK_BLUE  "c0c0f0"
#define DARKER_BLUE  "a0a0f0"
#define DARKEST_BLUE  "8080f0"
#define TABLE_STYLE " style=\"border-radius:10px;border:#6060f0 2px solid;\" width=100% bgcolor=#a0a0f0 cellpadding=4 border=0 "

extern const char *g_msg;

// . declare all dynamic functions here
// . these are all defined in Page*.cpp files
// . these are called to send a dynamic page
bool sendPageWidgets ( TcpSocket *socket , HttpRequest *hr ) ;
bool sendPageBasicStatus     ( TcpSocket *s , HttpRequest *r );

bool printGigabotAdvice(SafeBuf *sb,
			int32_t page,
			const HttpRequest *hr,
			const char *gerrmsg);

bool sendPageRoot     ( TcpSocket *s , HttpRequest *r );
bool sendPageRoot     ( TcpSocket *s , HttpRequest *r, char *cookie );
bool sendPageResults  ( TcpSocket *s , HttpRequest *r );
bool sendPageAddUrl   ( TcpSocket *s , HttpRequest *r );
bool sendPageGet      ( TcpSocket *s , HttpRequest *r );
bool sendPageLogin    ( TcpSocket *s , HttpRequest *r );
bool sendPageStats    ( TcpSocket *s , HttpRequest *r );
bool sendPageHosts    ( TcpSocket *s , HttpRequest *r );
bool sendPageSockets  ( TcpSocket *s , HttpRequest *r );
bool sendPagePerf     ( TcpSocket *s , HttpRequest *r );
bool sendPageTitledb  ( TcpSocket *s , HttpRequest *r );
bool sendPageSpiderdbLookup(TcpSocket *s, HttpRequest *r);
bool sendPageParser   ( TcpSocket *s , HttpRequest *r );
bool sendPageAddColl  ( TcpSocket *s , HttpRequest *r );
bool sendPageDelColl  ( TcpSocket *s , HttpRequest *r );
bool sendPageCloneColl( TcpSocket *s , HttpRequest *r );
bool sendPageSpiderdb ( TcpSocket *s , HttpRequest *r );
bool sendPageDoledbIPTable(TcpSocket *s, HttpRequest *r);
bool sendPageReindex  ( TcpSocket *s , HttpRequest *r );
bool sendPageInject   ( TcpSocket *s , HttpRequest *r );
bool sendPageAddUrl2  ( TcpSocket *s , HttpRequest *r );
bool sendPageGeneric  ( TcpSocket *s , HttpRequest *r ); // in Parms.cpp
bool sendPageProfiler   ( TcpSocket *s , HttpRequest *r );
bool sendPageThreads    ( TcpSocket *s , HttpRequest *r );
bool sendPageAPI        ( TcpSocket *s , HttpRequest *r );
bool sendPageHelp       ( TcpSocket *s , HttpRequest *r );
bool sendPageHealthCheck ( TcpSocket *sock , HttpRequest *hr ) ;
bool sendPageDefaultCss(TcpSocket *s, HttpRequest *r);


enum class page_method_t {
	page_method_get = 1,		//plain http get
	page_method_post_url,		//http post, with url-encoded form values
	page_method_post_form		//http post, with form-data attachement
};

// values for WebPage::m_flags
#define PG_NOAPI 0x01
#define PG_STATUS 0x02
#define PG_COLLADMIN 0x04
#define PG_MASTERADMIN 0x08
#define PG_ACTIVE      0x10

// . description of a dynamic page
// . we have a static array of these in Pages.cpp
class WebPage {
 public:
	char  m_pageNum;  // see enum array below for this
	const char *m_filename;
	int32_t  m_flen;
	const char *m_name;     // for printing the links to the pages in admin sect.
	bool  m_cast;     // broadcast input to all hosts?
	page_method_t  m_page_method;  // use a POST request/reply instead of GET?
	                  // used because GET's input is limited to a few k.
	//char  m_perm;     // permissions, see USER_* #define's below
	const char *m_desc; // page description
	bool (* m_function)(TcpSocket *s , HttpRequest *r);
	char  m_pgflags;
};



class Pages {

 public:

	const WebPage *getPage ( int32_t page ) ;

	// . associate each page number (like PAGE_SEARCH) with a callback
	//   to which we pass the HttpRequest and TcpSocket
	// . associate each page with a security level
	void init ( );

	// a request like "GET /sockets" should return PAGE_SOCKETS
	int32_t getDynamicPageNumber ( HttpRequest *r ) ;

	const char *getPath ( int32_t page ) ;

	// this passes control to the dynamic page generation routine based
	// on the path of the GET request
	bool sendDynamicReply ( TcpSocket *s , HttpRequest *r , int32_t page );

	//
	// HTML generation utility routines
	// . (always use the safebuf versions when possible)
	// . also, modify both if you modify either!

	bool printAdminTop 	       ( SafeBuf     *sb   ,
					 TcpSocket   *s    ,
					 HttpRequest *r    ,
					 const char  *qs = NULL,
					 const char* bodyJavascript = "" );

	bool  printAdminBottom         ( SafeBuf *sb);
	bool  printAdminBottom2        ( SafeBuf *sb);
	bool  printTail                ( SafeBuf* sb, bool isLocal );
	bool printSubmit ( SafeBuf *sb ) ;

	bool  printColors              ( SafeBuf *sb , const char* bodyJavascript = "" ) ;
	bool  printLogo                ( SafeBuf *sb, const char *coll ) ;
	bool  printHostLinks (SafeBuf *sb, int32_t  page, const char *coll, int32_t fromIp, const char *qs) ;

	bool  printAdminLinks          ( SafeBuf *sb, 
					 int32_t  page ,
					 const char *coll ,
					 bool isBasic );

	bool  printCollectionNavBar (SafeBuf *sb     ,
				      int32_t  page     ,
				      const char *coll     ,
				      const char *qs       ,
				      TcpSocket *sock ,
				      HttpRequest *hr );
};

extern class Pages g_pages;


// . each dynamic page has a number
// . some pages also have urls like /search to mean page=0
enum {

	// public pages
	PAGE_ROOT        =0,
	PAGE_RESULTS     ,
	PAGE_ADDURL      ,
	PAGE_GET         ,
	PAGE_LOGIN       ,

	// basic controls page /admin/basic
	PAGE_BASIC_SETTINGS ,
	PAGE_BASIC_STATUS ,
	PAGE_COLLPASSWORDS ,//BASIC_SECURITY ,
	PAGE_BASIC_SEARCH ,

	// master admin pages
	PAGE_HOSTS       ,
	PAGE_MASTER      ,
	PAGE_RDB    ,
	PAGE_SEARCH      ,
	PAGE_RANKING     ,
	PAGE_SPIDER      , 
	PAGE_SPIDERPROXIES ,
	PAGE_LOG         ,
	PAGE_COLLPASSWORDS2 ,//BASIC_SECURITY ,
	PAGE_MASTERPASSWORDS ,
#ifndef PRIVACORE_SAFE_VERSION
	PAGE_ADDCOLL     ,
	PAGE_DELCOLL     , 
	PAGE_CLONECOLL   ,
#endif
	PAGE_REPAIR      ,

	PAGE_FILTERS     ,
	PAGE_INJECT      , 
	PAGE_ADDURL2     ,
	PAGE_REINDEX     ,	

	PAGE_STATS       ,
	PAGE_PERF        ,
	PAGE_SOCKETS     ,

	PAGE_PROFILER    ,
	PAGE_THREADS     ,

	PAGE_API ,

	PAGE_TITLEDB     ,
	PAGE_SPIDERDBLOOKUP,
	PAGE_SPIDERDB    ,
	PAGE_DOLEIPTABLE ,
	PAGE_SEARCHBOX   ,
	PAGE_PARSER      ,
	PAGE_SITEDB      ,
	PAGE_HEALTHCHECK ,
	PAGE_NONE     	};
	

bool printApiForPage ( SafeBuf *sb , int32_t PAGENUM , CollectionRec *cr ) ;

#endif // GB_PAGES_H
