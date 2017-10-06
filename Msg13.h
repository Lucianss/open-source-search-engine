// Matt Wells, copyright Oct 2001

// . ask another host to download a url for you
// . the remote host will also use a cache if m_maxCacheAge > 0
// . used for downloading and caching robots.txt
// . if m_compressReply then the host compressed the http reply before
//   sending it back to you via udp

#ifndef GB_MSG13_H
#define GB_MSG13_H

#include "SpiderProxy.h" // MAXUSERNAMEPWD
#include "collnum_t.h"
#include <stddef.h>

// max crawl delay form proxy backoff of 1 minute (60 seconds)
#define MAX_PROXYCRAWLDELAYMS 60000

class RdbCache;

void resetMsg13Caches ( ) ;
bool printHammerQueueTable ( SafeBuf *sb ) ;

class Msg13Request {
public:

	// the top portion of Msg13Request is sent to handleRequest54()
	// in SpiderProxy.cpp to get and return proxies, as well as to
	// ban proxies.
	int32_t getProxyRequestSize() const { return offsetof(Msg13Request,m_lastHack); }
	int32_t  m_urlIp;
	int32_t  m_lbId; // loadbucket id
	// the http proxy to use to download
	int32_t  m_proxyIp;
	uint16_t m_proxyPort;
	int32_t  m_banProxyIp;
	uint16_t m_banProxyPort;
	char  m_opCode;
	char  m_lastHack;

	collnum_t m_collnum;

	// not part of the proxy request, but set from ProxyReply:
	int32_t  m_numBannedProxies;
	// . if using proxies, how many proxies have we tried to download 
	//   this url through
	// . used internally in Msg13.cpp
	int32_t m_proxyTries;
	// if using proxies, did host #0 tell us there were more to try if
	// this one did not work out?
	bool m_hasMoreProxiesToTry;

	// we call this function after the imposed crawl-delay is over
	void (*m_hammerCallback)(class Msg13Request *r);


	int64_t m_urlHash48;
	int32_t  m_firstIp;

	// when it was stored in the hammer queue
	int64_t m_stored;

	// a tmp hack var referencing into m_url[] below
	char *m_proxiedUrl;
	int32_t  m_proxiedUrlLen;

	int64_t m_downloadStartTimeMS;

	char  m_niceness;
	int32_t  m_ifModifiedSince;
	int32_t  m_maxCacheAge;
	int32_t  m_maxTextDocLen;
	int32_t  m_maxOtherDocLen;
	// in milliseconds. use -1 if none or unknown.
	int32_t  m_crawlDelayMS;
	// for linked list, this is the hammer queue
	class Msg13Request *m_nextLink;

	char m_proxyUsernamePwdAuth[MAXUSERNAMEPWD];

	// if doing spider compression, compute contentHash32 of document
	// downloaded, and if it matches this then send back EDOCUNCHANGED
	int32_t  m_contentHash32;

	unsigned  m_compressReply:1;
	unsigned  m_useCompressionProxy:1;

	// does url end in /robots.txt ?
	unsigned  m_isRobotsTxt:1;
	unsigned  m_skipHammerCheck:1;
	unsigned  m_attemptedIframeExpansion:1;
	unsigned  m_crawlDelayFromEnd:1;

	// does m_url represent a FULL http request mime and NOT just a url?
	// this happens when gigablast is being used like a squid proxy.
	unsigned  m_isSquidProxiedUrl:1;

	unsigned  m_forceUseFloaters:1;

	unsigned  m_wasInTableBeforeStarting:1;

	// if we just end up calling HttpServer::getDoc() via calling
	// downloadDoc() then we set this for callback purposes
	class Msg13 *m_parent;

	// on the other hand, if we are called indirectly by handleRequest13()
	// then we set m_udpSlot.
	class UdpSlot *m_udpSlot;

	// used for addTestDoc() and caching. msg13 sets this
	int64_t m_urlHash64;	
	int32_t      m_spideredTime;
	// used for caching (and for request table, wait in line table)
	int64_t m_cacheKey;

	char *ptr_url;
	char *ptr_cookie;

	int32_t  size_url;
	int32_t  size_cookie;

	// variable data starts here

	int32_t getSize() const {
		return offsetof(Msg13Request,ptr_url) + size_url + size_cookie;
	}

	// zero it all out
	void reset() {
		//memset (this,0,(char *)m_url - (char *)this + 1); 
		memset (this,0,sizeof(Msg13Request));
		m_maxTextDocLen  = -1; // no limit
		m_maxOtherDocLen = -1; // no limit
		m_crawlDelayMS   = -1; // unknown or none
		m_collnum = (collnum_t)-1;
	}
};

class Msg13 {

 public:

	Msg13() ;
	~Msg13();
	void reset() ;

	// register our request handler with g_udpServer (called by main.cpp)
	static bool registerHandler();

	static RdbCache *getHttpCacheRobots();
	static RdbCache *getHttpCacheOthers();

	bool getDoc ( Msg13Request *r ,
		      void   *state             ,
		      void  (*callback)(void *state) );

	bool forwardRequest();

	bool gotForwardedReply ( class UdpSlot *slot );
	bool gotFinalReply ( char *reply, int32_t replySize, int32_t replyAllocSize);

	// keep public so wrappers can access
	void *m_state;
	void  (* m_callback) (void *state );

	// we now store the uncompressed http reply in here
	char *m_replyBuf;
	int32_t  m_replyBufSize;
	int32_t  m_replyBufAllocSize;

	// point to it
	Msg13Request *m_request;
};

extern RdbCache s_hammerCache;

#endif // GB_MSG13_H
