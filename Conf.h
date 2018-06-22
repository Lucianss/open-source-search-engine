// Copyright Matt Wells, Apr 2001

// . every host has a config record
// . like tagdb, record in 100% xml
// . allows remote configuration of hosts through Msg4 class
// . remote user sends some xml, we set our member vars using that xml
// . when we save to disk we convert our mem vars to xml
// . is global so everybody can see it
// . conf record can be changed by director OR with the host's priv key
// . use Conf remotely to get setup info about a specific host
// . get your local ip/port/groupMask/etc. from this class not HostMap

#ifndef GB_CONF_H
#define GB_CONF_H

#include "max_coll_len.h"
#include "max_url_len.h"
#include "SafeBuf.h"
#include "BaseScoringParameters.h"

#define USERAGENTMAXSIZE      128

#define MAX_DNSIPS            16
#define MAX_RNSIPS            13

//Publicly accessible and generallyy HA / reachable DNS servers. Use Google's servers - works reasonably well
#define PUBLICLY_AVAILABLE_DNS1 "8.8.8.8"
#define PUBLICLY_AVAILABLE_DNS2 "8.8.4.4"

class TcpSocket;
class HttpRequest;


mode_t getFileCreationFlags();
mode_t getDirCreationFlags ();

class Conf {

  public:

	Conf();

	bool isCollAdmin ( TcpSocket *socket , HttpRequest *hr );
	bool isCollAdminForColl (TcpSocket *sock, HttpRequest *hr, const char *coll );
	bool isCollAdmin2 (TcpSocket *socket , HttpRequest *hr,
			   class CollectionRec *cr);


	bool isMasterAdmin ( TcpSocket *socket , HttpRequest *hr );
	bool hasMasterPwd ( HttpRequest *hr );
	bool isMasterIp      ( uint32_t ip );
	bool isConnectIp    ( uint32_t ip );

	// loads conf parms from this file "{dir}/gb.conf"
	bool init ( char *dir );

	void setRootIps();

	// saves any changes to the conf file
	bool save ( );

	// reset all values to their defaults
	void reset();

	// defaults to default collection
	const char *getDefaultColl ( );

	// max amount of memory we can use
	size_t m_maxMem;
	bool m_mlockAllCurrent;
	bool m_mlockAllFuture;

	// if this is false, we do not save, used by dump routines
	// in main.cpp so they can change parms here and not worry about
	// a core dump saving them
	bool m_save;

	bool m_runAsDaemon;

	bool m_logToFile;

	char m_defaultColl[MAX_COLL_LEN + 1];

	// . dns parameters
	// . dnsDir should hold our saved cached (TODO: save the dns cache)
	int32_t  m_numDns;
	int32_t  m_dnsIps[MAX_DNSIPS];
	int16_t m_dnsPorts[MAX_DNSIPS];

	int64_t m_dnsCacheSize;
	int64_t m_dnsCacheMaxAge;

	int32_t  m_dnsMaxCacheMem;

	SafeBuf m_proxyIps;
	SafeBuf m_proxyAuth;

	// built-in dns parameters using name servers
	bool  m_askRootNameservers;
	int32_t  m_numRns;
	int32_t  m_rnsIps[MAX_RNSIPS];

	char m_queryLanguageServerName[64];
	int32_t m_queryLanguageServerPort;
	unsigned m_maxOutstandingQueryLanguage;
	unsigned m_queryLanguageTimeout;

	char m_siteMedianPageTemperatureServerName[64];
	int32_t m_siteMedianPageTemperatureServerPort;
	unsigned m_maxOutstandingSiteMedianPageTemperature;
	unsigned m_siteMedianPageTemperatureTimeout;

	char m_siteNumInlinksServerName[64];
	int32_t m_siteNumInlinksServerPort;
	unsigned m_maxOutstandingSiteNumInlinks;
	unsigned m_siteNumInlinksTimeout;

	char m_urlClassificationServerName[64];
	int32_t m_urlClassificationServerPort;
	unsigned m_maxOutstandingUrlClassifications;
	unsigned m_urlClassificationTimeout;
	
	// used to limit all rdb's to one merge per machine at a time
	int32_t  m_mergeBufSize;

	int32_t m_doledbNukeInterval;
	
	// rdb settings

	// posdb
	int32_t m_posdbMaxLostPositivesPercentage;
	int64_t m_posdbFileCacheSize;
	int32_t  m_posdbMaxTreeMem;

	// tagdb
	int32_t m_tagdbMaxLostPositivesPercentage;
	int64_t m_tagdbFileCacheSize;
	int32_t  m_tagdbMaxTreeMem;

	char m_mergespaceLockDirectory[1024];
	int32_t m_mergespaceMinLockFiles;
	char m_mergespaceDirectory[1024];

	// clusterdb for site clustering, each rec is 16 bytes
	int32_t m_clusterdbMaxLostPositivesPercentage;
	int64_t m_clusterdbFileCacheSize;
	int32_t  m_clusterdbMaxTreeMem;
	int32_t  m_clusterdbMinFilesToMerge;

	// titledb
	int32_t m_titledbMaxLostPositivesPercentage;
	int64_t m_titledbFileCacheSize;
	int32_t  m_titledbMaxTreeMem;

	// spiderdb
	int32_t m_spiderdbMaxLostPositivesPercentage;
	int64_t m_spiderdbFileCacheSize;
	int32_t  m_spiderdbMaxTreeMem;

	// linkdb for storing linking relations
	int32_t m_linkdbMaxLostPositivesPercentage;
	int32_t  m_linkdbMaxTreeMem;
	int32_t  m_linkdbMinFilesToMerge;

	// are we doing a command line thing like 'gb 0 dump s ....' in
	// which case we do not want to log certain things
	bool m_doingCommandLine;

	int32_t  m_maxCoordinatorThreads;
	int32_t  m_maxCpuThreads;
	int32_t  m_maxSummaryThreads;
	int32_t  m_maxIOThreads;
	int32_t  m_maxExternalThreads;
	int32_t  m_maxFileMetaThreads;
	int32_t  m_maxMergeThreads;

	int32_t  m_maxJobCleanupTime;

	char    m_vagusClusterId[128];
	int32_t m_vagusPort;
	int32_t m_vagusKeepaliveSendInterval; //milliseconds
	int32_t m_vagusKeepaliveLifetime; //milliseconds
	int32_t m_vagusMaxDeadTime; //minutes
	
	int32_t m_maxDocsWanted;        //maximum number of results in one go. Puts a limit on SearchInput::m_docsWanted
	int32_t m_maxFirstResultNum;    //maximum document offset / result-page. Puts a limit on SearchInput::m_firstResultNum

	int32_t  min_docid_splits; //minimum number of DocId splits using Msg40
	int32_t  max_docid_splits; //maximum number of DocId splits using Msg40
	int64_t  m_msg40_msg39_timeout; //timeout for entire get-docid-list phase, in milliseconds.
	int64_t  m_msg3a_msg39_network_overhead; //additional latency/overhead of sending reqeust+response over network.

	bool	m_useHighFrequencyTermCache;

	bool  m_spideringEnabled;
	bool  m_injectionsEnabled;
	bool  m_queryingEnabled;
	bool  m_returnResultsAnyway;

	bool m_spiderIPUrl;
	bool m_spiderAdultContent;
	bool  m_addUrlEnabled; // TODO: use at http interface level
	bool  m_doStripeBalancing;

	// . true if the server is on the production cluster
	// . we enforce the 'elvtune -w 32 /dev/sd?' cmd on all drives because
	//   that yields higher performance when dumping/merging on disk
	bool  m_isLive;

	int32_t  m_maxTotalSpiders;

	int32_t m_spiderFilterableMaxWordCount;

	int32_t m_spiderDeadHostCheckInterval;

	int64_t m_spiderUrlCacheMaxAge;
	int64_t m_spiderUrlCacheSize;

	// indexdb has a max cached age for getting IndexLists (10 mins deflt)
	int32_t  m_indexdbMaxIndexListAge;

	int32_t m_udpMaxSockets;

	// TODO: parse these out!!!!
	int32_t  m_httpMaxSockets;
	int32_t  m_httpsMaxSockets;
	int32_t  m_httpMaxSendBufSize;

	// a search results cache (for Msg40)
	int64_t m_docSummaryWithDescriptionMaxCacheAge; //cache timeout for document summaries for documents with a meta-tag with description, in milliseconds

	// for Weights.cpp
	int32_t   m_sliderParm;

	float m_sameLangWeight;
	float m_unknownLangWeight;
	BaseScoringParameters m_baseScoringParameters;

	int32_t m_numFlagScoreMultipliers; //constant = 26
	int32_t m_numFlagRankAdjustments; //constant = 26
	
	int32_t m_maxCorruptLists;

	int32_t m_defaultQueryResultsValidityTime; //in seconds
	
	bool   m_useCollectionPasswords;

	// if in read-only mode we do no spidering and load no saved trees
	// so we can use all mem for caching index lists
	bool   m_readOnlyMode;

	// if this is true we use /etc/hosts for hostname lookup before dns
	bool   m_useEtcHosts;

	//verify integrity of tree/buckets after modification operations
	bool m_verifyTreeIntegrity;

	// just ensure lists being written are valid rdb records (titlerecs)
	// trying to isolate titlerec corruption
	bool m_verifyDumpedLists;

	// verify validity of index while merging
	bool m_verifyIndex;

	// calls fsync(fd) if true after each write
	bool   m_flushWrites; 
	bool   m_verifyWrites;
	int32_t   m_corruptRetries;
	int m_sqliteSynchronous;

	// verify tagrec while indexing
	bool m_verifyTagRec;

	bool m_spiderHostToQueryHostFallbackAllowed;
	bool m_queryHostToSpiderHostFallbackAllowed;

	int64_t m_docDeleteDelayMs;
	int64_t m_docRebuildDelayMs;
	int64_t m_docReindexDelayMs;

	int64_t m_docDeleteMaxPending;
	int64_t m_docRebuildMaxPending;
	int64_t m_docReindexMaxPending;

	// log unfreed memory on exit
	bool   m_detectMemLeaks;

	bool   m_forceIt;

	// if this is true we do not add indexdb keys that *should* already
	// be in indexdb. but if you recently upped the m_truncationLimit
	// then you can set this to false to add all indexdb keys.
	//bool   m_onlyAddUnchangedTermIds;
	bool   m_doIncrementalUpdating;

	int64_t m_stableSummaryCacheSize;
	int64_t m_stableSummaryCacheMaxAge;
	int64_t m_unstableSummaryCacheSize;
	int64_t m_unstableSummaryCacheMaxAge;

	bool   m_useShotgun;
	bool   m_testMem;
	bool   m_doConsistencyTesting;

	int32_t m_titleRecVersion;

	// defaults to "Gigabot/1.0"
	char m_spiderUserAgent[USERAGENTMAXSIZE];

	char m_spiderBotName[USERAGENTMAXSIZE];

	int32_t m_autoSaveFrequency;

	int32_t m_docCountAdjustment;

	bool m_profilingEnabled;

	//
	// See Log.h for an explanation of the switches below
	//

	// GET and POST requests.
	bool  m_logHttpRequests;
	bool  m_logAutobannedQueries;

	int32_t m_logLoopTimeThreshold;
	int32_t m_logRdbIndexAddListTimeThreshold;
	int32_t m_logRdbMapAddListTimeThreshold;

	// if query took this or more milliseconds, log its time
	int32_t  m_logQueryTimeThreshold;
	// if disk read took this or more milliseconds, log its time
	int32_t  m_logDiskReadTimeThreshold;

	int32_t m_logSqliteTransactionTimeThreshold;

	bool  m_logQueryReply;
	// log what gets into the index
	bool  m_logSpideredUrls;
	// log informational messages, they are not indicative of any error.
	bool  m_logInfo;
	// when out of udp slots
	bool  m_logNetCongestion;
	// doc quota limits, url truncation limits
	bool  m_logLimits;

	// log debug switches
	bool  m_logDebugAddurl;
	bool  m_logDebugAdmin;
	bool  m_logDebugBuild;
	bool  m_logDebugBuildTime;
	bool  m_logDebugDate;
	bool  m_logDebugDb;
	bool  m_logDebugDetailed;
	bool  m_logDebugDirty;
	bool  m_logDebugDisk;
	bool  m_logDebugDns;
	bool  m_logDebugDownloads;
	bool  m_logDebugHttp;
	bool  m_logDebugImage;
	bool  m_logDebugLang;
	bool  m_logDebugLinkInfo;
	bool  m_logDebugLoop;
	bool  m_logDebugMem;
	bool  m_logDebugMemUsage;
	bool  m_logDebugMerge;
	bool  m_logDebugMsg13;
	bool  m_logDebugMsg20;
	bool  m_logDebugMulticast;
	bool  m_logDebugNet;
	bool  m_logDebugProxies;
	bool  m_logDebugQuery;
	bool  m_logDebugRepair;
	bool  m_logDebugRobots;
	bool  m_logDebugSections;
	bool  m_logDebugSpcache; // SpiderCache.cpp debug
	bool  m_logDebugSpeller;
	bool  m_logDebugSpider;
	bool  m_logDebugReindex;
	bool  m_logDebugSEO;
	bool  m_logDebugStats;
	bool  m_logDebugSummary;
	bool  m_logDebugTagdb;
	bool  m_logDebugTcp;
	bool  m_logDebugTcpBuf;
	bool  m_logDebugTitle;
	bool  m_logDebugTopDocs;
	bool  m_logDebugUdp;
	bool  m_logDebugUnicode;
	bool  m_logDebugUrlAttempts;
	bool  m_logDebugVagus;

	bool m_logTraceBigFile;
	bool m_logTraceMatchList;
	bool m_logTraceContentTypeBlockList;
	bool m_logTraceDocid2FlagsAndSiteMap;
	bool m_logTraceDocProcess;
	bool m_logTraceDns;
	bool m_logTraceDnsBlockList;
	bool m_logTraceDnsCache;
	bool m_logTraceFile;
	bool m_logTraceHttpMime;
	bool m_logTraceIpBlockList;
	bool m_logTraceLanguageResultOverride;
	bool m_logTraceMem;
	bool m_logTraceMsg0;
	bool m_logTraceMsg4In;
	bool m_logTraceMsg4Out;
	bool m_logTraceMsg4OutData;
	bool m_logTraceMsg25;
	bool m_logTracePageLinkdbLookup;
	bool m_logTracePageSpiderdbLookup;
	bool m_logTracePos;
	bool m_logTracePosdb;
	bool m_logTraceQuery;
	bool m_logTraceQueryLanguage;
	bool m_logTraceRdb;
	bool m_logTraceRdbBase;
	bool m_logTraceRdbBuckets;
	bool m_logTraceRdbDump;
	bool m_logTraceRdbIndex;
	bool m_logTraceRdbList;
	bool m_logTraceRdbMap;
	bool m_logTraceRdbMerge;
	bool m_logTraceRdbTree;

	bool m_logTraceRepairs;
	bool m_logTraceRobots;
	bool m_logTraceRobotsCheckList;
	bool m_logTraceSiteMedianPageTemperature;
	bool m_logTraceSiteNumInlinks;
	bool m_logTraceSpider;
	bool m_logTraceSpiderUrlCache;
	bool m_logTraceReindex;
	bool m_logTraceSpiderdbRdbSqliteBridge;
	bool m_logTraceSummary;
	bool m_logTraceTitledb;
	bool m_logTraceXmlDoc;
	bool m_logTracePhrases;
	bool m_logTraceTokenIndexing;
	bool m_logTraceUrlMatchList;
	bool m_logTraceUrlResultOverride;
	bool m_logTraceWordSpam;
	bool m_logTraceUrlClassification;
	bool m_logTraceTopTree;
	bool m_logTraceTermCheckList;

	// expensive timing messages
	bool m_logTimingAddurl;
	bool m_logTimingAdmin;
	bool m_logTimingBuild;
	bool m_logTimingDb;
	bool m_logTimingNet;
	bool m_logTimingQuery;
	bool m_logTimingLinkInfo;
	bool m_logTimingRobots;

	// programmer reminders.
	bool m_logReminders;

	SafeBuf m_masterPwds;

	// these are the new master ips
	SafeBuf m_connectIps;

	char m_redirect[MAX_URL_LEN];
	bool m_useCompressionProxy;
	bool m_gzipDownloads;

	// used by proxy to make proxy point to the temp cluster while
	// the original cluster is updated
	bool m_useTmpCluster;

	// allow scaling up of hosts by removing recs not in the correct
	// group. otherwise a sanity check will happen.
	bool  m_allowScale;
	bool  m_bypassValidation;

	int32_t  m_maxCallbackDelay;

	// used by Repair.cpp
	bool  m_repairingEnabled;
	int32_t  m_maxRepairinjections;
	int64_t  m_repairMem;
	SafeBuf m_collsToRepair;
	bool  m_fullRebuild;
	bool  m_rebuildAddOutlinks;
	bool  m_rebuildRecycleLinkInfo;
	bool  m_rebuildUseTitleRecTagRec;
	bool  m_rebuildTitledb;
	bool  m_rebuildPosdb;
	bool  m_rebuildClusterdb;
	bool  m_rebuildSpiderdb;
	bool  m_rebuildSpiderdbSmall;
	bool  m_rebuildLinkdb;
	bool  m_rebuildRoots;
	bool  m_rebuildNonRoots;
};

extern class Conf g_conf;

#endif // GB_CONF_H
