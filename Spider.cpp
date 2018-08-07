// . TODO: do not cache if less than the 20k thing again.

// . TODO: nuke doledb every couple hours.
//   CollectionRec::m_doledbRefreshRateInSecs. but how would this work
//   for crawlbot jobs where we got 10,000 collections? i'd turn this off.
//   we could selectively update certain firstips in doledb that have
//   been in doledb for a long time.
//   i'd like to see how many collections are actually active
//   for diffbot first though.



// TODO: add m_downloadTimeTable to measure download speed of an IP
// TODO: consider a "latestpubdateage" in url filters for pages that are
//       adding new dates (not clocks) all the time

#include "Spider.h"
#include "SpiderLoop.h"
#include "SpiderColl.h"
#include "SpiderCache.h"
#include "Hostdb.h"
#include "RdbList.h"
#include "HashTableX.h"
#include "Msg5.h"      // local getList()
#include "Msg4Out.h"
#include "Doledb.h"
#include "Msg5.h"
#include "Collectiondb.h"
#include "Stats.h"
#include "SafeBuf.h"
#include "Repair.h"
#include "CountryCode.h"
#include "DailyMerge.h"
#include "Process.h"
#include "Conf.h"
#include "JobScheduler.h"
#include "XmlDoc.h"
#include "HttpServer.h"
#include "Pages.h"
#include "Parms.h"
#include "Rebalance.h"
#include "ip.h"
#include "Mem.h"
#include "UrlBlockCheck.h"
#include "Errno.h"
#include <list>


static void testWinnerTreeKey();

static int32_t getFakeIpForUrl2(const Url *url2);

/////////////////////////
/////////////////////////      SPIDEREC
/////////////////////////

void SpiderRequest::setKey (int32_t firstIp, int64_t parentDocId, int64_t uh48, bool isDel) {

	// sanity
	if ( firstIp == 0 || firstIp == -1 ) { g_process.shutdownAbort(true); }

	m_key = Spiderdb::makeKey ( firstIp, uh48, true, parentDocId, isDel );
	// set dataSize too!
	setDataSize();
}

void SpiderRequest::setDataSize ( ) {
	m_dataSize = (m_url - (char *)this) + strlen(m_url) + 1 
		// subtract m_key and m_dataSize
		- sizeof(key128_t) - 4 ;
}

int32_t SpiderRequest::print(SafeBuf *sbarg) const {
	SafeBuf tmp;
	SafeBuf *sb = sbarg ? sbarg : &tmp;

	sb->safePrintf("k=%s ", KEYSTR( this, getKeySizeFromRdbId( RDB_SPIDERDB_SQLITE ) ) );

	// indicate it's a request not a reply
	sb->safePrintf("REQ ");
	sb->safePrintf("ver=%d ", (int)m_version);

	sb->safePrintf("uh48=%" PRIx64" ",getUrlHash48());
	// if negtaive bail early now
	if ( (m_key.n0 & 0x01) == 0x00 ) {
		sb->safePrintf("[DELETE]");
		if ( ! sbarg ) printf("%s",sb->getBufStart() );
		return sb->length();
	}

	sb->safePrintf("recsize=%" PRId32" ",getRecSize());
	sb->safePrintf("parentDocId=%" PRIu64" ",getParentDocId());

	char ipbuf[16];
	sb->safePrintf("firstip=%s ",iptoa(m_firstIp,ipbuf) );
	sb->safePrintf("hostHash32=0x%" PRIx32" ",m_hostHash32 );
	sb->safePrintf("domHash32=0x%" PRIx32" ",m_domHash32 );
	sb->safePrintf("siteHash32=0x%" PRIx32" ",m_siteHash32 );
	sb->safePrintf("siteNumInlinks=%" PRId32" ",m_siteNumInlinks );

	// print time format: 7/23/1971 10:45:32
	struct tm *timeStruct ;
	char time[256];

	time_t ts = (time_t)m_addedTime;
	struct tm tm_buf;
	timeStruct = gmtime_r(&ts,&tm_buf);

	strftime ( time , 256 , "%Y%m%d-%H%M%S UTC", timeStruct );
	sb->safePrintf("addedTime=%s(%" PRIu32") ",time,(uint32_t)m_addedTime );
	sb->safePrintf("pageNumInlinks=%i ",(int)m_pageNumInlinks);
	sb->safePrintf("ufn=%" PRId32" ", (int32_t)m_ufn);
	// why was this unsigned?
	sb->safePrintf("priority=%" PRId32" ", (int32_t)m_priority);

	if ( m_isAddUrl ) sb->safePrintf("ISADDURL ");
	if ( m_isPageReindex ) sb->safePrintf("ISPAGEREINDEX ");
	if ( m_isPageParser ) sb->safePrintf("ISPAGEPARSER ");
	if ( m_urlIsDocId ) sb->safePrintf("URLISDOCID ");
	if ( m_isRSSExt ) sb->safePrintf("ISRSSEXT ");
	if ( m_isUrlPermalinkFormat ) sb->safePrintf("ISURLPERMALINKFORMAT ");
	if ( m_fakeFirstIp ) sb->safePrintf("ISFAKEFIRSTIP ");
	if ( m_isInjecting ) sb->safePrintf("ISINJECTING ");
	if ( m_forceDelete ) sb->safePrintf("FORCEDELETE ");

	if ( m_hasAuthorityInlink ) sb->safePrintf("HASAUTHORITYINLINK ");

	if ( m_avoidSpiderLinks ) sb->safePrintf("AVOIDSPIDERLINKS ");

	int32_t shardNum = g_hostdb.getShardNum( RDB_SPIDERDB_SQLITE, this );
	sb->safePrintf("shardnum=%" PRIu32" ",(uint32_t)shardNum);

	sb->safePrintf("url=%s",m_url);

	if ( ! sbarg ) {
		printf( "%s", sb->getBufStart() );
	}

	return sb->length();
}

void SpiderReply::setKey ( int32_t firstIp, int64_t parentDocId, int64_t uh48, bool isDel ) {
	m_key = Spiderdb::makeKey ( firstIp, uh48, false, parentDocId, isDel );
	// set dataSize too!
	m_dataSize = sizeof(SpiderReply) - sizeof(key128_t) - 4;
}

int32_t SpiderReply::print(SafeBuf *sbarg) const {

	SafeBuf *sb = sbarg;
	SafeBuf tmp;
	if ( ! sb ) sb = &tmp;

	sb->safePrintf("k=%s ",KEYSTR(this,sizeof(spiderdbkey_t)));

	// indicate it's a reply
	sb->safePrintf("REP ");
	sb->safePrintf("ver=%d ", (int)m_version);

	sb->safePrintf("uh48=%" PRIx64" ",getUrlHash48());
	sb->safePrintf("parentDocId=%" PRIu64" ",getParentDocId());

	// if negative bail early now
	if ( (m_key.n0 & 0x01) == 0x00 ) {
		sb->safePrintf("[DELETE]");
		if ( ! sbarg ) printf("%s",sb->getBufStart() );
		return sb->length();
	}

	char ipbuf[16];
	sb->safePrintf("firstip=%s ",iptoa(m_firstIp,ipbuf) );
	sb->safePrintf("percentChangedPerDay=%.02f%% ",m_percentChangedPerDay);

	// print time format: 7/23/1971 10:45:32
	struct tm *timeStruct ;
	char time[256];
	time_t ts = (time_t)m_spideredTime;
	struct tm tm_buf;
	timeStruct = gmtime_r(&ts,&tm_buf);
	time[0] = 0;
	if ( m_spideredTime ) {
		strftime(time, 256, "%Y%m%d-%H%M%S UTC", timeStruct);
	}
	sb->safePrintf("spideredTime=%s(%" PRIu32") ", time, (uint32_t)m_spideredTime);
	sb->safePrintf("siteNumInlinks=%" PRId32" ",m_siteNumInlinks );
	sb->safePrintf("ch32=%" PRIu32" ",(uint32_t)m_contentHash32);
	sb->safePrintf("crawldelayms=%" PRId32"ms ",m_crawlDelayMS );
	sb->safePrintf("httpStatus=%" PRId32" ",(int32_t)m_httpStatus );
	sb->safePrintf("langId=%s(%" PRId32") ", getLanguageString(m_langId),(int32_t)m_langId );

	if ( m_errCount )
		sb->safePrintf("errCount=%" PRId32" ",(int32_t)m_errCount);

	if ( m_sameErrCount )
		sb->safePrintf("sameErrCount=%" PRId32" ",(int32_t)m_sameErrCount);

	sb->safePrintf("errCode=%s(%" PRIu32") ",mstrerror(m_errCode),
		       (uint32_t)m_errCode );

	//if ( m_isSpam ) sb->safePrintf("ISSPAM ");
	if ( m_isRSS ) sb->safePrintf("ISRSS ");
	if ( m_isPermalink ) sb->safePrintf("ISPERMALINK ");
	//if ( m_deleted ) sb->safePrintf("DELETED ");
	if ( ! m_isIndexedINValid && m_isIndexed ) sb->safePrintf("ISINDEXED ");

	if ( ! sbarg ) 
		printf("%s",sb->getBufStart() );

	return sb->length();
}

/*
 * {
 *     "elapsedMS" : 0,
 *     "url": "http://example.com/",
 *     "status": "getting web page",
 *     "priority": 15,
 *     "ufn": 3,
 *     "firstIp": "127.0.0.1",
 *     "errCount": 0,
 *     "urlHash48": 123456789
 *     "siteInLinks": 0,
 *     "hops": 0,
 *     "addedTime: 14000000,
 *     "pageNumInLinks: 1,
 *     "parentDocId": 123456789,
 * }
 */
int32_t SpiderRequest::printToJSON(SafeBuf *sb, const char *status, const XmlDoc *xd, int32_t row) const {
	sb->safePrintf("\t\t{\n");

	int64_t elapsedMS = 0;
	if (xd) {
		elapsedMS = gettimeofdayInMilliseconds() - xd->m_startTime;
	}

	sb->safePrintf("\t\t\t\"elapsedMS\": %" PRId64",\n", elapsedMS);
	sb->safePrintf("\t\t\t\"url\": \"%s\",\n", m_url);
	sb->safePrintf("\t\t\t\"status\": \"%s\",\n", status);
	sb->safePrintf("\t\t\t\"priority\": %hhd,\n", m_priority);
	sb->safePrintf("\t\t\t\"ufn\": %" PRId16",\n", m_ufn);

	char ipbuf[16];
	sb->safePrintf("\t\t\t\"firstIp\": \"%s\",\n", iptoa(m_firstIp,ipbuf));
	sb->safePrintf("\t\t\t\"urlHash48\": %" PRId64",\n", getUrlHash48());
	sb->safePrintf("\t\t\t\"siteInLinks\": %" PRId32",\n", m_siteNumInlinks);
	sb->safePrintf("\t\t\t\"addedTime\": %" PRIu32",\n", m_addedTime);
	sb->safePrintf("\t\t\t\"pageNumInLinks\": %" PRIu8",\n", m_pageNumInlinks);
	sb->safePrintf("\t\t\t\"parentDocId\": %" PRId64"\n", getParentDocId());

	/// @todo ALC add flags to json response
//	if ( m_isAddUrl ) sb->safePrintf("ISADDURL ");
//	if ( m_isPageReindex ) sb->safePrintf("ISPAGEREINDEX ");
//	if ( m_isPageParser ) sb->safePrintf("ISPAGEPARSER ");
//	if ( m_urlIsDocId ) sb->safePrintf("URLISDOCID ");
//	if ( m_isRSSExt ) sb->safePrintf("ISRSSEXT ");
//	if ( m_isUrlPermalinkFormat ) sb->safePrintf("ISURLPERMALINKFORMAT ");
//	if ( m_isInjecting ) sb->safePrintf("ISINJECTING ");
//	if ( m_forceDelete ) sb->safePrintf("FORCEDELETE ");
//	if ( m_hasAuthorityInlink ) sb->safePrintf("HASAUTHORITYINLINK ");

	sb->safePrintf("\t\t}\n");
	sb->safePrintf("\t\t,\n");

	return sb->length();
}

int32_t SpiderRequest::printToTable(SafeBuf *sb, const char *status, const XmlDoc *xd, int32_t row) const {
	// show elapsed time
	if (xd) {
		int64_t now = gettimeofdayInMilliseconds();
		int64_t elapsed = now - xd->m_startTime;
		sb->safePrintf(" <td>%" PRId32"</td>\n",row);
		sb->safePrintf(" <td>%" PRId64"ms</td>\n",elapsed);
		collnum_t collnum = xd->m_collnum;
		CollectionRec *cr = g_collectiondb.getRec(collnum);
		const char *cs = "";
		if ( cr ) cs = cr->m_coll;

		sb->safePrintf(" <td><a href=\"/search?c=%s&q=url%%3A%s\">%s</a>"
			       "</td>\n",cs,m_url,cs);
	}

	sb->safePrintf(" <td><a href=\"%s\"><nobr>",m_url);
	sb->safeTruncateEllipsis ( m_url , 64 );
	sb->safePrintf("</nobr></a></td>\n");
	sb->safePrintf(" <td><nobr>%s</nobr></td>\n",status );

	sb->safePrintf(" <td>%" PRId32"</td>\n",(int32_t)m_priority);
	sb->safePrintf(" <td>%" PRId32"</td>\n",(int32_t)m_ufn);

	char ipbuf[16];
	sb->safePrintf(" <td>%s</td>\n",iptoa(m_firstIp,ipbuf) );
	sb->safePrintf(" <td>%" PRIu64"</td>\n",getUrlHash48());
	sb->safePrintf(" <td>%" PRId32"</td>\n",m_siteNumInlinks );

	// print time format: 7/23/1971 10:45:32
	struct tm *timeStruct ;
	char time[256];

	time_t ts3 = (time_t)m_addedTime;
	struct tm tm_buf;
	timeStruct = gmtime_r(&ts3,&tm_buf);

	strftime(time, 256, "%Y%m%d-%H%M%S UTC", timeStruct );
	sb->safePrintf(" <td><nobr>%s(%" PRIu32")</nobr></td>\n",time,
		       (uint32_t)m_addedTime);

	sb->safePrintf(" <td>%i</td>\n",(int)m_pageNumInlinks);
	sb->safePrintf(" <td>%" PRIu64"</td>\n",getParentDocId() );

	sb->safePrintf(" <td><nobr>");

	if ( m_isAddUrl ) sb->safePrintf("ISADDURL ");
	if ( m_isPageReindex ) sb->safePrintf("ISPAGEREINDEX ");
	if ( m_isPageParser ) sb->safePrintf("ISPAGEPARSER ");
	if ( m_urlIsDocId ) sb->safePrintf("URLISDOCID ");
	if ( m_isRSSExt ) sb->safePrintf("ISRSSEXT ");
	if ( m_isUrlPermalinkFormat ) sb->safePrintf("ISURLPERMALINKFORMAT ");
	if ( m_isInjecting ) sb->safePrintf("ISINJECTING ");
	if ( m_forceDelete ) sb->safePrintf("FORCEDELETE ");

	if ( m_hasAuthorityInlink ) sb->safePrintf("HASAUTHORITYINLINK ");

	sb->safePrintf("</nobr></td>\n");

	sb->safePrintf("</tr>\n");

	return sb->length();
}

int32_t SpiderRequest::printTableHeader ( SafeBuf *sb , bool currentlySpidering) {

	sb->safePrintf("<tr class=\"level2\">\n");

	// how long its been being spidered
	if ( currentlySpidering ) {
		sb->safePrintf(" <th>#</th>\n");
		sb->safePrintf(" <th>elapsed</th>\n");
		sb->safePrintf(" <th>coll</th>\n");
	}

	sb->safePrintf(" <th>url</th>\n");
	sb->safePrintf(" <th>status</th>\n");

	sb->safePrintf(" <th>pri</th>\n");
	sb->safePrintf(" <th>ufn</th>\n");

	sb->safePrintf(" <th>firstIp</th>\n");
	sb->safePrintf(" <th>urlHash48</th>\n");
	sb->safePrintf(" <th>siteInlinks</th>\n");
	sb->safePrintf(" <th>addedTime</th>\n");
	sb->safePrintf(" <th>pageNumInLinks</th>\n");
	sb->safePrintf(" <th>parentDocId</th>\n");
	sb->safePrintf(" <th>flags</th>\n");
	sb->safePrintf("</tr>\n");

	return sb->length();
}


/////////////////////////
/////////////////////////      SPIDERDB
/////////////////////////


// a global class extern'd in .h file
Spiderdb g_spiderdb;
Spiderdb g_spiderdb2;

// reset rdb
void Spiderdb::reset() { m_rdb.reset(); }

// print the spider rec
int32_t Spiderdb::print(const char *srec, SafeBuf *sb) {
	// get if request or reply and print it
	if ( isSpiderRequest ( reinterpret_cast<const key128_t*>(srec) ) )
		reinterpret_cast<const SpiderRequest*>(srec)->print(sb);
	else
		reinterpret_cast<const SpiderReply*>(srec)->print(sb);
	return 0;
}

void Spiderdb::printKey(const char *k) {
	const key128_t *key = reinterpret_cast<const key128_t*>(k);

	SafeBuf sb;
	// get if request or reply and print it
	if ( isSpiderRequest (key ) ) {
		reinterpret_cast<const SpiderRequest*>(key)->print(&sb);
	} else {
		reinterpret_cast<const SpiderReply*>(key)->print(&sb);
	}

	logf(LOG_TRACE, "%s", sb.getBufStart());
}

bool Spiderdb::init ( ) {
	char      priority   = 12;
	int32_t      spiderTime = 0x3fe96610;
	int64_t urlHash48  = 0x1234567887654321LL & 0x0000ffffffffffffLL;

	// doledb key test
	key96_t dk = Doledb::makeKey(priority,spiderTime,urlHash48,false);
	if(Doledb::getPriority(&dk)!=priority){g_process.shutdownAbort(true);}
	if(Doledb::getSpiderTime(&dk)!=spiderTime){g_process.shutdownAbort(true);}
	if(Doledb::getUrlHash48(&dk)!=urlHash48){g_process.shutdownAbort(true);}
	if(Doledb::getIsDel(&dk)!= 0){g_process.shutdownAbort(true);}

	// spiderdb key test
	int64_t docId = 123456789;
	int32_t firstIp = 0x23991688;
	key128_t sk = Spiderdb::makeKey ( firstIp, urlHash48, 1, docId, false );
	if ( ! Spiderdb::isSpiderRequest (&sk) ) { g_process.shutdownAbort(true); }
	if ( Spiderdb::getUrlHash48(&sk) != urlHash48){g_process.shutdownAbort(true);}
	if ( Spiderdb::getFirstIp(&sk) != firstIp) {g_process.shutdownAbort(true);}

	testWinnerTreeKey();

	// . what's max # of tree nodes?
	// . assume avg spider rec size (url) is about 45
	// . 45 + 33 bytes overhead in tree is 78
	int32_t maxTreeNodes  = g_conf.m_spiderdbMaxTreeMem  / 78;

	// initialize our own internal rdb
	return m_rdb.init ( "spiderdb"   ,
			    -1      , // fixedDataSize
			    // now that we have MAX_WINNER_NODES allowed in doledb
			    // we don't have to keep spiderdb so tightly merged i guess..
			    // MDW: it seems to slow performance when not tightly merged
			    // so put this back to "2"...
			    -1,//g_conf.m_spiderdbMinFilesToMerge , mintomerge
			    g_conf.m_spiderdbMaxTreeMem ,
			    maxTreeNodes                ,
			    false                       , // half keys?
			    sizeof(key128_t),             //key size
			    false);                       //useIndexFile
}



// init the rebuild/secondary rdb, used by PageRepair.cpp
bool Spiderdb::init2 ( int32_t treeMem ) {
	// . what's max # of tree nodes?
	// . assume avg spider rec size (url) is about 45
	// . 45 + 33 bytes overhead in tree is 78
	int32_t maxTreeNodes  = treeMem  / 78;
	// initialize our own internal rdb
	return m_rdb.init ( "spiderdbRebuild"   ,
			    -1            , // fixedDataSize
			    200           , // g_conf.m_spiderdbMinFilesToMerge
			    treeMem       , // g_conf.m_spiderdbMaxTreeMem ,
			    maxTreeNodes  ,
			    false         , // half keys?
			    sizeof(key128_t), // key size
			    false);           //useIndexFile
}

key128_t Spiderdb::makeKey ( int32_t      firstIp     ,
			     int64_t urlHash48   , 
			     bool      isRequest   ,
			     // MDW: now we use timestamp instead of parentdocid
			     // for spider replies. so they do not dedup...
			     int64_t parentDocId ,
			     bool      isDel       ) {
	key128_t k;
	k.n1 = (uint32_t)firstIp;
	// push ip to top 32 bits
	k.n1 <<= 32;
	// . top 32 bits of url hash are in the lower 32 bits of k.n1
	// . often the urlhash48 has top bits set that shouldn't be so mask
	//   it to 48 bits
	k.n1 |= (urlHash48 >> 16) & 0xffffffff;
	// remaining 16 bits
	k.n0 = urlHash48 & 0xffff;
	// room for isRequest
	k.n0 <<= 1;
	if ( isRequest ) k.n0 |= 0x01;
	// parent docid
	k.n0 <<= 38;
	// if we are making a spider reply key just leave the parentdocid as 0
	// so we only store one reply per url. the last reply we got.
	// if ( isRequest ) k.n0 |= parentDocId & DOCID_MASK;
	k.n0 |= parentDocId & DOCID_MASK;
	// reserved (padding)
	k.n0 <<= 8;
	// del bit
	k.n0 <<= 1;
	if ( ! isDel ) k.n0 |= 0x01;
	return k;
}


////////
//
// winner tree key. holds the top/best spider requests for a firstIp
// for spidering purposes.
//
////////

// key bitmap (192 bits):
//
// ffffffff ffffffff ffffffff ffffffff  f=firstIp
// pppppppp pppppppp 00000000 00000000  p=255-priority
// tttttttt tttttttt tttttttt tttttttt  t=spiderTimeMS
// tttttttt tttttttt tttttttt tttttttt  h=urlHash48
// hhhhhhhh hhhhhhhh hhhhhhhh hhhhhhhh 
// hhhhhhhh hhhhhhhh 00000000 00000000

key192_t makeWinnerTreeKey ( int32_t firstIp ,
			     int32_t priority ,
			     int64_t spiderTimeMS ,
			     int64_t uh48 ) {
	key192_t k;
	k.n2 = firstIp;
	k.n2 <<= 16;
	k.n2 |= (255-priority);
	k.n2 <<= 16;

	k.n1 = spiderTimeMS;

	k.n0 = uh48;
	k.n0 <<= 16;

	return k;
}

void parseWinnerTreeKey ( const key192_t  *k ,
			  int32_t      *firstIp ,
			  int32_t      *priority ,
			  int64_t  *spiderTimeMS ,
			  int64_t *uh48 ) {
	*firstIp = (k->n2) >> 32;
	*priority = 255 - ((k->n2 >> 16) & 0xffff);

	*spiderTimeMS = k->n1;

	*uh48 = (k->n0 >> 16);
}

static void testWinnerTreeKey() {
	int32_t firstIp = 1234567;
	int32_t priority = 123;
	int64_t spiderTimeMS = 456789123LL;
	int64_t uh48 = 987654321888LL;
	key192_t k = makeWinnerTreeKey (firstIp,priority,spiderTimeMS,uh48);
	int32_t firstIp2;
	int32_t priority2;
	int64_t spiderTimeMS2;
	int64_t uh482;
	parseWinnerTreeKey(&k,&firstIp2,&priority2,&spiderTimeMS2,&uh482);
	if ( firstIp != firstIp2 ) { g_process.shutdownAbort(true); }
	if ( priority != priority2 ) { g_process.shutdownAbort(true); }
	if ( spiderTimeMS != spiderTimeMS2 ) { g_process.shutdownAbort(true); }
	if ( uh48 != uh482 ) { g_process.shutdownAbort(true); }
}

/////////////////////////
/////////////////////////      UTILITY FUNCTIONS
/////////////////////////

// does this belong in our spider cache?
bool isAssignedToUs ( int32_t firstIp ) {
	if( !g_hostdb.getMyHost()->m_spiderEnabled ) return false;
	
	// get our group
	const Host *shard = g_hostdb.getMyShard();
	// pick a host in our group

	// and number of hosts in the group
	int32_t hpg = g_hostdb.getNumHostsPerShard();
	// let's mix it up since spider shard was selected using this
	// same mod on the firstIp method!!
	uint64_t h64 = firstIp;
	unsigned char c = firstIp & 0xff;
	h64 ^= g_hashtab[c][0];

	// hash to a host
	int32_t i = ((uint32_t)h64) % hpg;
	const Host *h = &shard[i];
	// return that if alive
	if ( ! g_hostdb.isDead(h) && h->m_spiderEnabled) {
		return (h->m_hostId == g_hostdb.m_myHostId);
	}
	// . select another otherwise
	// . put all alive in an array now
	const Host *alive[64];
	int32_t upc = 0;

	for ( int32_t j = 0 ; j < hpg ; j++ ) {
		const Host *h = &shard[j];
		if ( g_hostdb.isDead(h) ) continue;
		if( ! h->m_spiderEnabled ) continue;
		alive[upc++] = h;
	}
	// if none, that is bad! return the first one that we wanted to
	if ( upc == 0 ) {
		char ipbuf[16];
		log("spider: no hosts can handle spider request for ip=%s", iptoa(firstIp,ipbuf));
		return false;
	}
	// select from the good ones now
	i  = ((uint32_t)firstIp) % upc;
	// get that
	h = alive[i]; //&shard[i];
	// guaranteed to be alive... kinda
	return (h->m_hostId == g_hostdb.m_myHostId);
}


///////////////////////////////////
//
// URLFILTERS
//
///////////////////////////////////

#define SIGN_EQ 1
#define SIGN_NE 2
#define SIGN_GT 3
#define SIGN_LT 4
#define SIGN_GE 5
#define SIGN_LE 6


class PatternData {
public:
	// hash of the subdomain or domain for this line in sitelist
	int32_t m_thingHash32;
	// ptr to the line in CollectionRec::m_siteListBuf
	int32_t m_patternStrOff;
	// offset of the url path in the pattern, 0 means none
	int16_t m_pathOff;
	int16_t m_pathLen;
	// offset into buffer. for 'tag:shallow site:walmart.com' type stuff
	int32_t  m_tagOff;
	int16_t m_tagLen;
};

static void doneAddingSeedsWrapper(void *state) {
	SafeBuf *sb = reinterpret_cast<SafeBuf*>(state);
	// note it
	log("basic: done adding seeds using msg4");
	delete sb;
}

// . Collectiondb.cpp calls this when any parm flagged with
//   PF_REBUILDURLFILTERS is updated
// . it only adds sites via msg4 that are in "siteListArg" but NOT in the
//   current CollectionRec::m_siteListBuf
// . updates SpiderColl::m_siteListDomTable to see what doms we can spider
// . updates SpiderColl::m_negSubstringBuf and m_posSubStringBuf to
//   see what substrings in urls are disallowed/allowable for spidering
// . this returns false if it blocks
// . returns true and sets g_errno on error
// . uses msg4 to add seeds to spiderdb if necessary if "siteListArg"
//   has new urls that are not currently in cr->m_siteListBuf
// . only adds seeds for the shard we are on iff we are responsible for
//   the fake firstip!!! that way only one shard does the add.
bool updateSiteListBuf ( collnum_t collnum ,
                         bool addSeeds ,
                         const char *siteListArg ) {

	const CollectionRec *cr = g_collectiondb.getRec(collnum);
	if ( ! cr ) return true;

	// tell spiderloop to update the active list in case this
	// collection suddenly becomes active
	g_spiderLoop.invalidateActiveList();

	// this might make a new spidercoll...
	SpiderColl *sc = g_spiderCache.getSpiderColl ( cr->m_collnum );

	// sanity. if in use we should not even be here
	if ( sc->m_msg4x.isInUse() ) {
		log( LOG_WARN, "basic: trying to update site list while previous update still outstanding.");
		g_errno = EBADENGINEER;
		return true;
	}

	// hash current sitelist entries, each line so we don't add
	// dup requests into spiderdb i guess...
	HashTableX dedup;
	if ( ! dedup.set ( 4,0,1024,NULL,0,false,"sldt") ) {
		return true;
	}

	// this is a safebuf PARM in Parms.cpp now HOWEVER, not really
	// because we set it here from a call to CommandUpdateSiteList()
	// because it requires all this computational crap.
	const char *op = cr->m_siteListBuf.getBufStart();

	// scan and hash each line in it
	for ( ; ; ) {
		// done?
		if ( ! *op ) break;
		// skip spaces
		if ( is_wspace_a(*op) ) op++;
		// done?
		if ( ! *op ) break;
		// get end
		const char *s = op;
		// skip to end of line marker
		for ( ; *op && *op != '\n' ; op++ ) ;
		// keep it simple
		int32_t h32 = hash32 ( s , op - s );
		// for deduping
		if ( ! dedup.addKey ( &h32 ) ) {
			return true;
		}
	}

	// get the old sitelist Domain Hash to PatternData mapping table
	// which tells us what domains, subdomains or paths we can or
	// can not spider...
	HashTableX *dt = &sc->m_siteListDomTable;

	// reset it
	if (!dt->set(4, sizeof(PatternData), 1024, NULL, 0, true, "sldt")) {
		return true;
	}

	// clear old shit
	sc->m_posSubstringBuf.purge();
	sc->m_negSubstringBuf.purge();

	// we can now free the old site list methinks
	//cr->m_siteListBuf.purge();

	// reset flags
	sc->m_siteListIsEmpty = true;

	sc->m_siteListIsEmptyValid = true;

	// use this so it will be free automatically when msg4 completes!
	SafeBuf *spiderReqBuf = new SafeBuf();

	// scan the list
	const char *pn = siteListArg;

	// completely empty?
	if ( ! pn ) return true;

	int32_t lineNum = 1;

	int32_t added = 0;

	Url u;

	for ( ; *pn ; lineNum++ ) {

		// get end
		const char *s = pn;
		// skip to end of line marker
		for ( ; *pn && *pn != '\n' ; pn++ ) ;

		// point to the pattern (skips over "tag:xxx " if there)
		const char *patternStart = s;

		// back p up over spaces in case ended in spaces
		const char *pe = pn;
		for ( ; pe > s && is_wspace_a(pe[-1]) ; pe-- );

		// skip over the \n so pn points to next line for next time
		if ( *pn == '\n' ) pn++;

		// make hash of the line
		int32_t h32 = hash32 ( s , pe - s );

		bool seedMe = true;
		bool isUrl = true;
		bool isNeg = false;
		bool isFilter = true;

		// skip spaces at start of line
		for ( ; *s && *s == ' ' ; s++ );

		// comment?
		if ( *s == '#' ) continue;

		// empty line?
		if ( s[0] == '\r' && s[1] == '\n' ) { s++; continue; }

		// empty line?
		if ( *s == '\n' ) continue;

		const char *tag = NULL;
		int32_t tagLen = 0;

		innerLoop:

		// skip spaces
		for ( ; *s && *s == ' ' ; s++ );

		// these will be manual adds and should pass url filters
		// because they have the "ismanual" directive override
		if ( strncmp(s,"seed:",5) == 0 ) {
			s += 5;
			isFilter = false;
			goto innerLoop;
		}


		// does it start with "tag:xxxxx "?
		if ( *s == 't' &&
		     s[1] == 'a' &&
		     s[2] == 'g' &&
		     s[3] == ':' ) {
			tag = s+4;
			for ( ; *s && ! is_wspace_a(*s) ; s++ );
			tagLen = s - tag;
			// skip over white space after tag:xxxx so "s"
			// point to the url or contains: or whatever
			for ( ; *s && is_wspace_a(*s) ; s++ );
			// set pattern start to AFTER the tag stuff
			patternStart = s;
		}

		if ( *s == '-' ) {
			isNeg = true;
			s++;
		}

		if ( strncmp(s,"site:",5) == 0 ) {
			s += 5;
			seedMe = false;
			goto innerLoop;
		}

		if ( strncmp(s,"contains:",9) == 0 ) {
			s += 9;
			seedMe = false;
			isUrl = false;
			goto innerLoop;
		}

		int32_t slen = pe - s;

		// empty line?
		if ( slen <= 0 )
			continue;

		// add to string buffers
		if ( ! isUrl && isNeg ) {
			if ( !sc->m_negSubstringBuf.safeMemcpy(s,slen))
				return true;
			if ( !sc->m_negSubstringBuf.pushChar('\0') )
				return true;
			if ( ! tagLen ) continue;
			// append tag
			if ( !sc->m_negSubstringBuf.safeMemcpy("tag:",4))
				return true;
			if ( !sc->m_negSubstringBuf.safeMemcpy(tag,tagLen) )
				return true;
			if ( !sc->m_negSubstringBuf.pushChar('\0') )
				return true;
		}
		if ( ! isUrl ) {
			// add to string buffers
			if ( ! sc->m_posSubstringBuf.safeMemcpy(s,slen) )
				return true;
			if ( ! sc->m_posSubstringBuf.pushChar('\0') )
				return true;
			if ( ! tagLen ) continue;
			// append tag
			if ( !sc->m_posSubstringBuf.safeMemcpy("tag:",4))
				return true;
			if ( !sc->m_posSubstringBuf.safeMemcpy(tag,tagLen) )
				return true;
			if ( !sc->m_posSubstringBuf.pushChar('\0') )
				return true;
			continue;
		}


		u.set( s, slen );

		// error? skip it then...
		if ( u.getHostLen() <= 0 ) {
			log("basic: error on line #%" PRId32" in sitelist",lineNum);
			continue;
		}

		// is fake ip assigned to us?
		int32_t firstIp = getFakeIpForUrl2 ( &u );

		if ( ! isAssignedToUs( firstIp ) ) continue;

		// see if in existing table for existing site list
		if ( addSeeds &&
		     // a "site:" directive mean no seeding
		     // a "contains:" directive mean no seeding
		     seedMe &&
		     // do not seed stuff after tag:xxx directives
		     // no, we need to seed it to avoid confusion. if
		     // they don't want it seeded they can use site: after
		     // the tag:
		     //! tag &&
		     ! dedup.isInTable ( &h32 ) ) {
			// make spider request
			SpiderRequest sreq;
			sreq.setFromAddUrl ( u.getUrl() );
			if (
				// . add this url to spiderdb as a spiderrequest
				// . calling msg4 will be the last thing we do
					!spiderReqBuf->safeMemcpy(&sreq,sreq.getRecSize()))
				return true;
			// count it
			added++;

		}

		// if it is a "seed: xyz.com" thing it is seed only
		// do not use it for a filter rule
		if ( ! isFilter ) continue;


		// make the data node used for filtering urls during spidering
		PatternData pd;
		// hash of the subdomain or domain for this line in sitelist
		pd.m_thingHash32 = u.getHostHash32();
		// . ptr to the line in CollectionRec::m_siteListBuf.
		// . includes pointing to "exact:" too i guess and tag: later.
		// . store offset since CommandUpdateSiteList() passes us
		//   a temp buf that will be freed before copying the buf
		//   over to its permanent place at cr->m_siteListBuf
		pd.m_patternStrOff = patternStart - siteListArg;
		// offset of the url path in the pattern, 0 means none
		pd.m_pathOff = 0;
		// did we have a tag?
		if ( tag ) {
			pd.m_tagOff = tag - siteListArg;
			pd.m_tagLen = tagLen;
		}
		else {
			pd.m_tagOff = -1;
			pd.m_tagLen = 0;
		}
		// scan url pattern, it should start at "s"
		const char *x = s;
		// go all the way to the end
		for ( ; *x && x < pe ; x++ ) {
			// skip ://
			if ( x[0] == ':' && x[1] =='/' && x[2] == '/' ) {
				x += 2;
				continue;
			}
			// stop if we hit another /, that is path start
			if ( x[0] != '/' ) continue;
			x++;
			// empty path besides the /?
			if (  x >= pe   ) break;
			// ok, we got something here i think
			// no, might be like http://xyz.com/?poo
			//if ( u.getPathLen() <= 1 ) { g_process.shutdownAbort(true); }
			// calc length from "start" of line so we can
			// jump to the path quickly for compares. inc "/"
			pd.m_pathOff = (x-1) - patternStart;
			pd.m_pathLen = pe - (x-1);
			break;
		}

		// add to new dt
		int32_t domHash32 = u.getDomainHash32();
		if ( ! dt->addKey ( &domHash32 , &pd ) )
			return true;

		// we have some patterns in there
		sc->m_siteListIsEmpty = false;
	}

	if ( ! addSeeds ) return true;

	log( "spider: adding %" PRId32" seed urls", added );

	// use spidercoll to contain this msg4 but if in use it
	// won't be able to be deleted until it comes back..
	if(!sc->m_msg4x.addMetaList(spiderReqBuf, sc->m_collnum, spiderReqBuf, doneAddingSeedsWrapper, RDB_SPIDERDB_DEPRECATED))
		return false;
	else {
		delete spiderReqBuf;
		return true;
	}
}

// . Spider.cpp calls this to see if a url it wants to spider is
//   in our "site list"
// . we should return the row of the FIRST match really
// . the url patterns all contain a domain now, so this can use the domain
//   hash to speed things up
// . return ptr to the start of the line in case it has "tag:" i guess
static const char *getMatchingUrlPattern(const SpiderColl *sc, const SpiderRequest *sreq, const char *tagArg) { // tagArg can be NULL
	logTrace( g_conf.m_logTraceSpider, "BEGIN" );

	// if it is just a bunch of comments or blank lines, it is empty
	if ( sc->m_siteListIsEmptyValid && sc->m_siteListIsEmpty ) {
		logTrace( g_conf.m_logTraceSpider, "END. Empty. Returning NULL" );
		return NULL;
	}

	// if we had a list of contains: or regex: directives in the sitelist
	// we have to linear scan those
	const char *nb = sc->m_negSubstringBuf.getBufStart();
	const char *nbend = nb + sc->m_negSubstringBuf.length();
	for ( ; nb && nb < nbend ; ) {
		// return NULL if matches a negative substring
		if ( strstr ( sreq->m_url , nb ) ) {
			logTrace( g_conf.m_logTraceSpider, "END. Matches negative substring. Returning NULL" );
			return NULL;
		}
		// skip it
		nb += strlen(nb) + 1;
	}


	const char *myPath = NULL;

	// check domain specific tables
	const HashTableX *dt = &sc->m_siteListDomTable;

	// get this
	const CollectionRec *cr = sc->getCollectionRec();

	// need to build dom table for pattern matching?
	if ( dt->getNumUsedSlots() == 0 && cr ) {
		// do not add seeds, just make siteListDomTable, etc.
		updateSiteListBuf ( sc->m_collnum ,
		                    false , // add seeds?
		                    cr->m_siteListBuf.getBufStart() );
	}

	if ( dt->getNumUsedSlots() == 0 ) {
		// empty site list -- no matches
		logTrace( g_conf.m_logTraceSpider, "END. No slots. Returning NULL" );
		return NULL;
		//g_process.shutdownAbort(true); }
	}

	// this table maps a 32-bit domain hash of a domain to a
	// patternData class. only for those urls that have firstIps that
	// we handle.
	int32_t slot = dt->getSlot ( &sreq->m_domHash32 );

	const char *buf = cr->m_siteListBuf.getBufStart();

	// loop over all the patterns that contain this domain and see
	// the first one we match, and if we match a negative one.
	for ( ; slot >= 0 ; slot = dt->getNextSlot(slot,&sreq->m_domHash32)) {
		// get pattern
		const PatternData *pd = (const PatternData *)dt->getValueFromSlot ( slot );
		// point to string
		const char *patternStr = buf + pd->m_patternStrOff;
		// is it negative? return NULL if so so url will be ignored
		//if ( patternStr[0] == '-' )
		//	return NULL;
		// otherwise, it has a path. skip if we don't match path ptrn
		if ( pd->m_pathOff ) {
			if ( ! myPath ) myPath = sreq->getUrlPath();
			if ( strncmp (myPath, patternStr + pd->m_pathOff, pd->m_pathLen ) != 0 ) {
				continue;
			}
		}

		// for entries like http://domain.com/ we have to match
		// protocol and url can NOT be like www.domain.com to match.
		// this is really like a regex like ^http://xyz.com/poo/boo/
		if ( (patternStr[0]=='h' ||
		      patternStr[0]=='H') &&
		     ( patternStr[1]=='t' ||
		       patternStr[1]=='T' ) &&
		     ( patternStr[2]=='t' ||
		       patternStr[2]=='T' ) &&
		     ( patternStr[3]=='p' ||
		       patternStr[3]=='P' ) ) {
			const char *x = patternStr+4;
			// is it https:// ?
			if ( *x == 's' || *x == 'S' ) x++;
			// watch out for subdomains like http.foo.com
			if ( *x != ':' ) {
				goto nomatch;
			}
			// ok, we have to substring match exactly. like
			// ^http://xyssds.com/foobar/
			const char *a = patternStr;
			const char *b = sreq->m_url;
			for ( ; ; a++, b++ ) {
				// stop matching when pattern is exhausted
				if ( is_wspace_a(*a) || ! *a ) {
					logTrace( g_conf.m_logTraceSpider, "END. Pattern is exhausted. Returning '%s'", patternStr );
					return patternStr;
				}
				if ( *a != *b ) {
					break;
				}
			}
			// we failed to match "pd" so try next line
			continue;
		}

		nomatch:
		// if caller also gave a tag we'll want to see if this
		// "pd" has an entry for this domain that has that tag
		if ( tagArg ) {
			// skip if entry has no tag
			if ( pd->m_tagLen <= 0 ) {
				continue;
			}

			// skip if does not match domain or host
			if ( pd->m_thingHash32 != sreq->m_domHash32 &&
			     pd->m_thingHash32 != sreq->m_hostHash32 ) {
				continue;
			}

			// compare tags
			const char *pdtag = pd->m_tagOff + buf;
			if ( strncmp(tagArg,pdtag,pd->m_tagLen) != 0 ) {
				continue;
			}

			// must be nothing after
			if ( is_alnum_a(tagArg[pd->m_tagLen]) ) {
				continue;
			}

			// that's a match
			logTrace( g_conf.m_logTraceSpider, "END. Match tag. Returning '%s'", patternStr );
			return patternStr;
		}

		// was the line just a domain and not a subdomain?
		if ( pd->m_thingHash32 == sreq->m_domHash32 ) {
			// this will be false if negative pattern i guess
			logTrace( g_conf.m_logTraceSpider, "END. Match domain. Returning '%s'", patternStr );
			return patternStr;
		}

		// was it just a subdomain?
		if ( pd->m_thingHash32 == sreq->m_hostHash32 ) {
			// this will be false if negative pattern i guess
			logTrace( g_conf.m_logTraceSpider, "END. Match subdomain. Returning '%s'", patternStr );
			return patternStr;
		}
	}


	// if we had a list of contains: or regex: directives in the sitelist
	// we have to linear scan those
	const char *pb = sc->m_posSubstringBuf.getBufStart();
	const char *pend = pb + sc->m_posSubstringBuf.length();
	for ( ; pb && pb < pend ; ) {
		// return NULL if matches a negative substring
		if ( strstr ( sreq->m_url , pb ) ) {
			logTrace( g_conf.m_logTraceSpider, "END. Match. Returning '%s'", pb );
			return pb;
		}
		// skip it
		pb += strlen(pb) + 1;
	}

	return NULL;
}

// . this is called by SpiderCache.cpp for every url it scans in spiderdb
// . we must skip certain rules in getUrlFilterNum() when doing to for Msg20
//   because things like "parentIsRSS" can be both true or false since a url
//   can have multiple spider recs associated with it!
int32_t getUrlFilterNum(const SpiderRequest *sreq,
			const SpiderReply   *srep,
			int32_t		nowGlobal,
			bool		isForMsg20,
			const CollectionRec	*cr,
			bool		isOutlink,
			int32_t		langIdArg ) {
	logTrace( g_conf.m_logTraceSpider, "BEGIN" );
		
	if ( ! sreq ) {
		logError("spider: sreq is NULL!");
		return -1;
	}

	int32_t langId = langIdArg;
	if ( srep ) langId = srep->m_langId;

	// convert lang to string
	const char *lang    = NULL;
	int32_t  langLen = 0;
	if ( langId >= 0 ) { // if ( srep ) {
		// this is NULL on corruption
		lang = getLanguageAbbr ( langId );//srep->m_langId );	
		if (lang) langLen = strlen(lang);
	}

	const char *tld = (char *)-1;
	int32_t  tldLen;

	int32_t  urlLen = sreq->getUrlLen();
	const char *url = sreq->m_url;

	const char *row = NULL;
	bool checkedRow = false;
	SpiderColl *sc = g_spiderCache.getSpiderColl(cr->m_collnum);

	// CONSIDER COMPILING FOR SPEED:
	// 1) each command can be combined into a bitmask on the spiderRequest
	//    bits, or an access to m_siteNumInlinks, or a substring match
	// 2) put all the strings we got into the list of Needles
	// 3) then generate the list of needles the SpiderRequest/url matches
	// 4) then reduce each line to a list of needles to have, a
	//    min/max/equal siteNumInlinks
	//    and a bitMask to match the bit flags in the SpiderRequest

	// stop at first regular expression it matches
	for ( int32_t i = 0 ; i < cr->m_numRegExs ; i++ ) {
		// get the ith rule
		const SafeBuf *sb = &cr->m_regExs[i];
		//char *p = cr->m_regExs[i];
		const char *p = sb->getBufStart();

checkNextRule:
		// skip leading whitespace
		while ( *p && isspace(*p) ) p++;

		// do we have a leading '!'
		bool val = 0;
		if ( *p == '!' ) { val = 1; p++; }
		// skip whitespace after the '!'
		while ( *p && isspace(*p) ) p++;

		if ( *p=='h' && strncmp(p,"hasauthorityinlink",18) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) continue;
			// skip if not valid (pageaddurl? injection?)
			if ( ! sreq->m_hasAuthorityInlinkValid ) continue;
			// if no match continue
			if ( (bool)sreq->m_hasAuthorityInlink==val)continue;
			// skip
			p += 18;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		if ( *p=='h' && strncmp(p,"hasreply",8) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// skip for msg20
			if ( isForMsg20 ) continue;
			// if we got a reply, we are not new!!
			//if ( (bool)srep == (bool)val ) continue;
			if ( (bool)(sreq->m_hadReply) == (bool)val ) continue;
			// skip it for speed
			p += 8;
			// check for &&
			p = strstr(p, "&&");
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// hastmperror, if while spidering, the last reply was
		// like EDNSTIMEDOUT or ETCPTIMEDOUT or some kind of
		// usually temporary condition that warrants a retry
		if ( *p=='h' && strncmp(p,"hastmperror",11) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// skip for msg20
			if ( isForMsg20 ) continue;
			// reply based
			if ( ! srep ) continue;

			// get our error code
			int32_t errCode = srep->m_errCode;

			// . make it zero if not tmp error
			// . now have EDOCUNCHANGED and EDOCNOGOODDATE from
			//   Msg13.cpp, so don't count those here...
			if (!isSpiderTempError(errCode)) {
				errCode = 0;
			}

			// if no match continue
			if ( (bool)errCode == val ) continue;

			// skip
			p += 11;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		if ( *p != 'i' ) goto skipi;

		if ( strncmp(p,"isinjected",10) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) continue;
			// if no match continue
			if ( (bool)sreq->m_isInjecting==val ) continue;
			// skip
			p += 10;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		if ( strncmp(p,"isreindex",9) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) continue;
			// if no match continue
			if ( (bool)sreq->m_isPageReindex==val ) continue;
			// skip
			p += 9;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		// is it in the big list of sites?
		if ( strncmp(p,"insitelist",10) == 0 ) {
			// rebuild site list
			if ( !sc->m_siteListIsEmptyValid ) {
				updateSiteListBuf( sc->m_collnum, false, cr->m_siteListBuf.getBufStart() );
			}

			// if there is no domain or url explicitly listed
			// then assume user is spidering the whole internet
			// and we basically ignore "insitelist"
			if ( sc->m_siteListIsEmptyValid && sc->m_siteListIsEmpty ) {
				// use a dummy row match
				row = (char *)1;
			} else if ( ! checkedRow ) {
				// only do once for speed
				checkedRow = true;
				// this function is in PageBasic.cpp
				row = getMatchingUrlPattern ( sc, sreq ,NULL);
			}

			// if we are not submitted from the add url api, skip
			if ( (bool)row == val ) {
				continue;
			}
			// skip
			p += 10;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		// . was it submitted from PageAddUrl.cpp?
		// . replaces the "add url priority" parm
		if ( strncmp(p,"isaddurl",8) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) continue;
			// if we are not submitted from the add url api, skip
			if ( (bool)sreq->m_isAddUrl == val ) continue;
			// skip
			p += 8;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		if ( p[0]=='i' && strncmp(p,"ismanualadd",11) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) continue;
			// . if we are not submitted from the add url api, skip
			// . if we have '!' then val is 1
			if ( sreq->m_isAddUrl    || 
			     sreq->m_isInjecting ||
			     sreq->m_isPageReindex ||
			     sreq->m_isPageParser ) {
				if ( val ) continue;
			}
			else {
				if ( ! val ) continue;
			}
			// skip
			p += 11;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		// does it have an rss inlink? we want to expedite indexing
		// of such pages. i.e. that we gather from an rss feed that
		// we got from a pingserver...
		if ( strncmp(p,"isroot",6) == 0 ) {
			// skip for msg20
			//if ( isForMsg20 ) continue;
			// this is a docid only url, no actual url, so skip
			if ( sreq->m_isPageReindex ) continue;
			// a fast check
			const char *u = sreq->m_url;
			// skip http
			u += 4;
			// then optional s for https
			if ( *u == 's' ) u++;
			// then ://
			u += 3;
			// scan until \0 or /
			for ( ; *u && *u !='/' ; u++ );
			// if \0 we are root
			bool isRoot = true;
			if ( *u == '/' ) {
				u++;
				if ( *u ) isRoot = false;
			}
			// if we are not root
			if ( isRoot == val ) continue;
			// skip
			p += 6;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			
			p += 2;
			goto checkNextRule;
		}

		// we can now handle this guy since we have the latest
		// SpiderReply, pretty much guaranteed
		if ( strncmp(p,"isindexed",9) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			
			// skip for msg20
			if ( isForMsg20 ) continue;
			// skip if reply does not KNOW because of an error
			// since XmDoc::indexDoc() called
			// XmlDoc::getNewSpiderReply() and did not have this
			// info...
			if ( srep && (bool)srep->m_isIndexedINValid ) continue;
			// if no match continue
			if ( srep && (bool)srep->m_isIndexed==val ) continue;
			// allow "!isindexed" if no SpiderReply at all
			if ( ! srep && val == 0 ) continue;
			// skip
			p += 9;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		if ( strncmp ( p , "isfakeip",8 ) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) continue;
			// if no match continue
			if ( (bool)sreq->m_fakeFirstIp == val ) continue;
			p += 8;
			p = strstr(p, "&&");
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}

		// check for "isrss" aka "rss"
		if ( strncmp(p,"isrss",5) == 0 ) {
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// must have a reply
			if ( ! srep ) continue;
			// if we are not rss, we do not match this rule
			if ( (bool)srep->m_isRSS == val ) continue; 
			// skip it
			p += 5;
			// check for &&
			p = strstr(p, "&&");
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// check for "isrss" aka "rss"
		if ( strncmp(p,"isrssext",8) == 0 ) {
			// if we are not rss, we do not match this rule
			if ( (bool)sreq->m_isRSSExt == val ) continue; 
			// skip it
			p += 8;
			// check for &&
			p = strstr(p, "&&");
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// check for permalinks. for new outlinks we *guess* if its
		// a permalink by calling isPermalink() function.
		if (!strncmp(p,"ispermalink",11) ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// must have a reply
			if ( ! srep ) continue;
			// if we are not rss, we do not match this rule
			if ( (bool)srep->m_isPermalink == val ) continue; 
			// skip it
			p += 11;
			// check for &&
			p = strstr(p, "&&");
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}

			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// supports LF_ISPERMALINK bit for outlinks that *seem* to
		// be permalinks but might not
		if (!strncmp(p,"ispermalinkformat",17) ) {
			// if we are not rss, we do not match this rule
			if ( (bool)sreq->m_isUrlPermalinkFormat == val ) {
				continue;
			}

			// check for &&
			p = strstr(p, "&&");

			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// check for this
		if ( strncmp(p,"isnewrequest",12) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// skip for msg20
			if ( isForMsg20 ) continue;
			// skip if we are a new request and val is 1 (has '!')
			if ( ! srep && val ) continue;
			// skip if we are a new request and val is 1 (has '!')
			if(srep&&sreq->m_addedTime>srep->m_spideredTime &&val)
				continue;
			// skip if we are old and val is 0 (does not have '!')
			if(srep&&sreq->m_addedTime<=srep->m_spideredTime&&!val)
				continue;
			// skip it for speed
			p += 12;
			// check for &&
			p = strstr(p, "&&");
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// kinda like isnewrequest, but has no reply. use hasreply?
		if ( strncmp(p,"isnew",5) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// skip for msg20
			if ( isForMsg20 ) continue;
			// if we got a reply, we are not new!!
			if ( (bool)sreq->m_hadReply != (bool)val ) continue;
			// skip it for speed
			p += 5;
			// check for &&
			p = strstr(p, "&&");
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}
		// iswww, means url is like www.xyz.com/...
		if ( strncmp(p,"iswww", 5) == 0 ) {
			// skip "iswww"
			p += 5;
			// skip over http:// or https://
			const char *u = sreq->m_url;
			if ( u[4] == ':' ) u += 7;
			if ( u[5] == ':' ) u += 8;
			// url MUST be a www url
			char isWWW = 0;
			if( u[0] == 'w' &&
			    u[1] == 'w' &&
			    u[2] == 'w' ) isWWW = 1;
			// skip if no match
			if ( isWWW == val ) continue;
			// TODO: fix www.knightstown.skepter.com
			// maybe just have a bit in the spider request
			// another rule?
			p = strstr(p,"&&");
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&'
			p += 2;
			goto checkNextRule;
		}

		// non-boolen junk
 skipi:

		// . we always match the "default" reg ex
		// . this line must ALWAYS exist!
		if ( *p=='d' && ! strcmp(p,"default" ) ) {
			logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
			return i;
		}

		// is it in the big list of sites?
		if ( *p == 't' && strncmp(p,"tag:",4) == 0 ) {
			// skip for msg20
			//if ( isForMsg20 ) continue;
			// if only seeds in the sitelist and no

			// if there is no domain or url explicitly listed
			// then assume user is spidering the whole internet
			// and we basically ignore "insitelist"
			if ( sc->m_siteListIsEmpty && sc->m_siteListIsEmptyValid ) {
				row = NULL;// no row
			} else if ( ! checkedRow ) {
				// only do once for speed
				checkedRow = true;
				// this function is in PageBasic.cpp
				// . it also has to match "tag" at (p+4)
				row = getMatchingUrlPattern ( sc, sreq ,p+4);
			}
			// if we are not submitted from the add url api, skip
			if ( (bool)row == val ) continue;
			// skip tag:
			p += 4;
			// skip to next constraint
			p = strstr(p, "&&");
			// all done?
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			p += 2;
			goto checkNextRule;
		}
		



		// set the sign
		const char *s = p;
		// skip s to after
		while ( *s && is_alpha_a(*s) ) s++;

		// skip white space before the operator
		//char *saved = s;
		while ( *s && is_wspace_a(*s) ) s++;

		char sign = 0;
		if ( *s == '=' ) {
			s++;
			if ( *s == '=' ) s++;
			sign = SIGN_EQ;
		}
		else if ( *s == '!' && s[1] == '=' ) {
			s += 2;
			sign = SIGN_NE;
		}
		else if ( *s == '<' ) {
			s++;
			if ( *s == '=' ) { sign = SIGN_LE; s++; }
			else               sign = SIGN_LT; 
		} 
		else if ( *s == '>' ) {
			s++;
			if ( *s == '=' ) { sign = SIGN_GE; s++; }
			else               sign = SIGN_GT; 
		} 

		// skip whitespace after the operator
		while ( *s && is_wspace_a(*s) ) s++;


		// new quotas. 'sitepages' = pages from site.
		// 'sitepages > 20 && seedcount <= 1 --> FILTERED'
		if ( *p == 's' &&
		     p[1] == 'i' &&
		     p[2] == 't' &&
		     p[3] == 'e' &&
		     p[4] == 'p' &&
		     p[5] == 'a' &&
		     p[6] == 'g' &&
		     p[7] == 'e' &&
		     p[8] == 's' ) {
			int32_t *valPtr ;
			valPtr=(int32_t*)sc->m_siteIndexedDocumentCount.getValue(&sreq->m_siteHash32);
			// if no count in table, that is strange, i guess
			// skip for now???
			int32_t a;
			if ( ! valPtr ) a = 0;//{ g_process.shutdownAbort(true); }
			else a = *valPtr;
			//log("sitepgs=%" PRId32" for %s",a,sreq->m_url);
			// what is the provided value in the url filter rule?
			int32_t b = atoi(s);
			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// tld:cn 
		if ( *p=='t' && strncmp(p,"tld",3)==0){
			// set it on demand
			if ( tld == (char *)-1 )
				tld = getTLDFast ( sreq->m_url , &tldLen );
			// no match if we have no tld. might be an IP only url,
			// or not in our list in Domains.cpp::isTLD()
			if ( ! tld || tldLen == 0 ) continue;
			// set these up
			//char *a    = tld;
			//int32_t  alen = tldLen;
			const char *b    = s;
			// loop for the comma-separated list of tlds
			// like tld:us,uk,fr,it,de
		subloop1:
			// get length of it in the regular expression box
			const char *start = b;
			while ( *b && !is_wspace_a(*b) && *b!=',' ) b++;
			int32_t  blen = b - start;
			//char sm;
			// if we had tld==com,org,...
			if ( sign == SIGN_EQ &&
			     blen == tldLen && 
			     strncasecmp(start,tld,tldLen)==0 ) 
				// if we matched any, that's great
				goto matched1;
			// if its tld!=com,org,...
			// and we equal the string, then we do not matcht his
			// particular rule!!!
			if ( sign == SIGN_NE &&
			     blen == tldLen && 
			     strncasecmp(start,tld,tldLen)==0 ) 
				// we do not match this rule if we matched
				// and of the tlds in the != list
				continue;
			// might have another tld in a comma-separated list
			if ( *b != ',' ) {
				// if that was the end of the list and the
				// sign was == then skip this rule
				if ( sign == SIGN_EQ ) continue;
				// otherwise, if the sign was != then we win!
				if ( sign == SIGN_NE ) goto matched1;
				// otherwise, bad sign?
				continue;
			}
			// advance to next tld if there was a comma after us
			b++;
			// and try again
			goto subloop1;
			// otherwise
			// do we match, if not, try next regex
			//sm = strncasecmp(a,b,blen);
			//if ( sm != 0 && sign == SIGN_EQ ) goto miss1;
			//if ( sm == 0 && sign == SIGN_NE ) goto miss1;
			// come here on a match
		matched1:
			// we matched, now look for &&
			p = strstr ( b , "&&" );
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
			// come here if we did not match the tld
		}


		// lang:en,zh_cn
		if ( *p=='l' && strncmp(p,"lang",4)==0){
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			
			// must have a reply
			if ( langId == -1 ) continue;
			// skip if unknown? no, we support "xx" as unknown now
			//if ( srep->m_langId == 0 ) continue;
			// set these up
			const char *b = s;
			// loop for the comma-separated list of langids
			// like lang==en,es,...
		subloop2:
			// get length of it in the regular expression box
			const char *start = b;
			while ( *b && !is_wspace_a(*b) && *b!=',' ) b++;
			int32_t  blen = b - start;
			//char sm;
			// if we had lang==en,es,...
			if ( sign == SIGN_EQ &&
			     blen == langLen && 
			     lang &&
			     strncasecmp(start,lang,langLen)==0 ) 
				// if we matched any, that's great
				goto matched2;
			// if its lang!=en,es,...
			// and we equal the string, then we do not match this
			// particular rule!!!
			if ( sign == SIGN_NE &&
			     blen == langLen && 
			     lang && 
			     strncasecmp(start,lang,langLen)==0 ) 
				// we do not match this rule if we matched
				// and of the langs in the != list
				continue;
			// might have another in the comma-separated list
			if ( *b != ',' ) {
				// if that was the end of the list and the
				// sign was == then skip this rule
				if ( sign == SIGN_EQ ) continue;
				// otherwise, if the sign was != then we win!
				if ( sign == SIGN_NE ) goto matched2;
				// otherwise, bad sign?
				continue;
			}
			// advance to next list item if was a comma after us
			b++;
			// and try again
			goto subloop2;
			// come here on a match
		matched2:
			// we matched, now look for &&
			p = strstr ( b , "&&" );
			// if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			// skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
			// come here if we did not match the tld
		}

		// selector using the first time it was added to the Spiderdb
		// added by Sam, May 5th 2015
		if ( *p=='u' && strncmp(p,"urlage",6) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) {
				//log("was for message 20");
				continue;

			}
			// get the age of the spider_request. 
			// (substraction of uint with int, hope
			// every thing goes well there)
			int32_t sreq_age = 0;

			// if m_discoveryTime is available, we use it. Otherwise we use m_addedTime
			if ( sreq && sreq->m_discoveryTime!=0) sreq_age = nowGlobal-sreq->m_discoveryTime;
			if ( sreq && sreq->m_discoveryTime==0) sreq_age = nowGlobal-sreq->m_addedTime;
			//log("spiderage=%d",sreq_age);
			// the argument entered by user
			int32_t argument_age=atoi(s) ;
			if ( sign == SIGN_EQ && sreq_age != argument_age ) continue;
			if ( sign == SIGN_NE && sreq_age == argument_age ) continue;
			if ( sign == SIGN_GT && sreq_age <= argument_age ) continue;
			if ( sign == SIGN_LT && sreq_age >= argument_age ) continue;
			if ( sign == SIGN_GE && sreq_age <  argument_age ) continue;
			if ( sign == SIGN_LE && sreq_age >  argument_age ) continue;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}


		if ( *p=='e' && strncmp(p,"errorcount",10) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// skip for msg20
			if ( isForMsg20 ) continue;
			// reply based
			if ( ! srep ) continue;
			// shortcut
			int32_t a = srep->m_errCount;
			// make it point to the retry count
			int32_t b = atoi(s);
			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			// skip fast
			//p += 10;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		if ( *p=='s' && strncmp(p,"sameerrorcount",14) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// skip for msg20
			if ( isForMsg20 ) continue;
			// reply based
			if ( ! srep ) continue;
			// shortcut
			int32_t a = srep->m_sameErrCount;
			// make it point to the retry count
			int32_t b = atoi(s);
			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			// skip fast
			//p += 14;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}


		// EBADURL malformed url is ... 32880
		if ( *p=='e' && strncmp(p,"errorcode",9) == 0 ) {
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// skip for msg20
			if ( isForMsg20 ) continue;
			// reply based
			if ( ! srep ) continue;
			// shortcut
			int32_t a = srep->m_errCode;
			// make it point to the retry count
			int32_t b = atoi(s);
			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			// skip fast
			//p += 9;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		if ( *p == 'n' && strncmp(p,"numinlinks",10) == 0 ) {
			// skip for msg20
			if ( isForMsg20 ) continue;
			// these are -1 if they are NOT valid
			int32_t a = sreq->m_pageNumInlinks;
			// make it point to the priority
			int32_t b = atoi(s);
			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			// skip fast
			//p += 10;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// siteNumInlinks >= 300 [&&]
		if ( *p=='s' && strncmp(p, "sitenuminlinks", 14) == 0){
			// these are -1 if they are NOT valid
			int32_t a1 = sreq->m_siteNumInlinks;

			// only assign if valid
			int32_t a2 = -1; 
			if ( srep ) a2 = srep->m_siteNumInlinks;

			// assume a1 is the best
			int32_t a = -1;

			// assign to the first valid one
			if      ( a1 != -1 ) a = a1;
			else if ( a2 != -1 ) a = a2;

			// swap if both are valid, but srep is more recent
			if ( a1 != -1 && a2 != -1 && srep->m_spideredTime > sreq->m_addedTime )
				a = a2;

			// skip if nothing valid
			if ( a == -1 ) continue;

			// make it point to the priority
			int32_t b = atoi(s);

			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			// skip fast
			//p += 14;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// how many days have passed since it was last attempted
		// to be spidered? used in conjunction with percentchanged
		// to assign when to re-spider it next
		if ( *p=='s' && strncmp(p, "spiderwaited", 12) == 0){
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1");
				return -1;
			}

			// must have a reply
			if ( ! srep ) continue;

			// skip for msg20
			if ( isForMsg20 ) continue;

			// shortcut
			int32_t a = nowGlobal - srep->m_spideredTime;

			// make it point to the priority
			int32_t b = atoi(s);
			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// percentchanged >= 50 [&&] ...
		if ( *p=='p' && strncmp(p, "percentchangedperday", 20) == 0){
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// must have a reply
			if ( ! srep ) continue;
			// skip for msg20
			if ( isForMsg20 ) continue;
			// shortcut
			float a = srep->m_percentChangedPerDay;
			// make it point to the priority
			float b = atof(s);
			// compare
			if ( sign == SIGN_EQ && !almostEqualFloat(a, b) ) continue;
			if ( sign == SIGN_NE && almostEqualFloat(a, b) ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// httpStatus == 400
		if ( *p=='h' && strncmp(p, "httpstatus", 10) == 0){
			// if we do not have enough info for outlink, all done
			if ( isOutlink ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning -1" );
				return -1;
			}
			// must have a reply
			if ( ! srep ) continue;
			// shortcut (errCode doubles as g_errno)
			int32_t a = srep->m_httpStatus;
			// make it point to the priority
			int32_t b = atoi(s);
			// compare
			if ( sign == SIGN_EQ && a != b ) continue;
			if ( sign == SIGN_NE && a == b ) continue;
			if ( sign == SIGN_GT && a <= b ) continue;
			if ( sign == SIGN_LT && a >= b ) continue;
			if ( sign == SIGN_GE && a <  b ) continue;
			if ( sign == SIGN_LE && a >  b ) continue;
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

		// our own regex thing (match front of url)
		if ( *p=='^' ) {
			// advance over caret
			p++;
			// now pstart pts to the string we will match
			const char *pstart = p;
			// make "p" point to one past the last char in string
			while ( *p && ! is_wspace_a(*p) ) p++;
			// how long is the string to match?
			int32_t plen = p - pstart;
			// empty? that's kinda an error
			if ( plen == 0 ) 
				continue;
			int32_t m = 1;
			// check to see if we matched if url was long enough
			if ( urlLen >= plen )
				m = strncmp(pstart,url,plen);
			if ( ( m == 0 && val == 0 ) ||
			     // if they used the '!' operator and we
			     // did not match the string, that's a 
			     // row match
			     ( m && val == 1 ) ) {
				// another expression follows?
				p = strstr(s, "&&");
				//if nothing, else then it is a match
				if ( ! p ) {
					logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
					return i;
				}
				//skip the '&&' and go to next rule
				p += 2;
				goto checkNextRule;
			}
			// no match
			continue;
		}

		// our own regex thing (match end of url)
		if ( *p=='$' ) {
			// advance over dollar sign
			p++;
			// a hack for $\.css, skip over the backslash too
			if ( *p=='\\' && *(p+1)=='.' ) p++;
			// now pstart pts to the string we will match
			const char *pstart = p;
			// make "p" point to one past the last char in string
			while ( *p && ! is_wspace_a(*p) ) p++;
			// how long is the string to match?
			int32_t plen = p - pstart;
			// empty? that's kinda an error
			if ( plen == 0 ) 
				continue;
			// . do we match it?
			// . url has to be at least as big
			// . match our tail
			int32_t m = 1;
			// check to see if we matched if url was long enough
			if ( urlLen >= plen )
				m = strncmp(pstart,url+urlLen-plen,plen);
			if ( ( m == 0 && val == 0 ) ||
			     // if they used the '!' operator and we
			     // did not match the string, that's a 
			     // row match
			     ( m && val == 1 ) ) {
				// another expression follows?
				p = strstr(s, "&&");
				//if nothing, else then it is a match
				if ( ! p ) {
					logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
					return i;
				}

				//skip the '&&' and go to next rule
				p += 2;
				goto checkNextRule;
			}
			// no match
			continue;
		}

		// . by default a substring match
		// . action=edit
		// . action=history

		// now pstart pts to the string we will match
		const char *pstart = p;
		// make "p" point to one past the last char in string
		while ( *p && ! is_wspace_a(*p) ) p++;
		// how long is the string to match?
		int32_t plen = p - pstart;
		// need something...
		if ( plen <= 0 ) continue;
		// does url contain it? haystack=url needle=pstart..p
		const char *found = strnstrn(url, urlLen, pstart, plen);

		// support "!company" meaning if it does NOT match
		// then do this ...
		if ( ( found && val == 0 ) ||
		     // if they used the '!' operator and we
		     // did not match the string, that's a 
		     // row match
		     ( ! found && val == 1 ) ) {
			// another expression follows?
			p = strstr(s, "&&");
			//if nothing, else then it is a match
			if ( ! p ) {
				logTrace( g_conf.m_logTraceSpider, "END, returning i (%" PRId32")", i );
				return i;
			}
			//skip the '&&' and go to next rule
			p += 2;
			goto checkNextRule;
		}

	}

	// return -1 if no match, caller should use a default
	logTrace( g_conf.m_logTraceSpider, "END, returning -1" );

	return -1;
}

// . dedup for spiderdb
// . TODO: we can still have spider request dups in this if they are
//   sandwiched together just right because we only compare to the previous
//   SpiderRequest we added when looking for dups. just need to hash the
//   relevant input bits and use that for deduping.
// . TODO: we can store ufn/priority/spiderTime in the SpiderRequest along
//   with the date now, so if url filters do not change then 
//   gotSpiderdbList() can assume those to be valid and save time. BUT it does
//   have siteNumInlinks...
void dedupSpiderdbList ( RdbList *list ) {
	char *newList = list->getList();

	char *dst          = newList;
	char *restorePoint = newList;

	int64_t reqUh48  = 0LL;
	int64_t repUh48  = 0LL;

	SpiderReply   *oldRep = NULL;
	char *lastKey     = NULL;

	int32_t oldSize = list->getListSize();
	int32_t corrupt = 0;

	int32_t numToFilter = 0;

	// keep track of spider requests with the same url hash (uh48)
	std::list<std::pair<uint32_t, SpiderRequest*>> spiderRequests;

	// reset it
	list->resetListPtr();

	for ( ; ! list->isExhausted() ; ) {
		// get rec
		char *rec = list->getCurrentRec();

		// pre skip it
		list->skipCurrentRecord();

		// skip if negative, just copy over
		if (KEYNEG(rec)) {
			// otherwise, keep it
			lastKey = dst;
			memmove(dst, rec, sizeof(key128_t));
			dst += sizeof(key128_t);
			continue;
		}

		// is it a reply?
		if (Spiderdb::isSpiderReply((key128_t *)rec)) {
			// cast it
			SpiderReply *srep = (SpiderReply *)rec;

			// shortcut
			int64_t uh48 = srep->getUrlHash48();

			// crazy?
			if (!uh48) {
				//uh48 = hash64b ( srep->m_url );
				uh48 = 12345678;
				log("spider: got uh48 of zero for spider req. computing now.");
			}

			// does match last reply?
			if (repUh48 == uh48) {
				// if he's a later date than us, skip us!
				if (oldRep->m_spideredTime >= srep->m_spideredTime) {
					// skip us!
					continue;
				}
				// otherwise, erase him
				dst = restorePoint;
			}

			// save in case we get erased
			restorePoint = dst;

			// get our size
			int32_t recSize = srep->getRecSize();

			// and add us
			lastKey = dst;
			memmove(dst, rec, recSize);

			// advance
			dst += recSize;
			// update this crap for comparing to next reply
			repUh48 = uh48;
			oldRep = srep;

			// get next spiderdb record
			continue;
		}

		// shortcut
		SpiderRequest *sreq = (SpiderRequest *)rec;

		// might as well filter out corruption
		if (sreq->isCorrupt()) {
			corrupt += sreq->getRecSize();
			continue;
		}

		/// @note if we need to clean out existing spiderdb records, add it here

		// recalculate uh48 to make sure it's the same as stored url
		{
			int64_t uh48 = (hash64b(sreq->m_url) & 0x0000ffffffffffffLL);
			if (sreq->getUrlHash48() != uh48) {
				logError("Recalculated uh48=%" PRId64" != stored uh48=%" PRId64" for url='%s'", uh48, sreq->getUrlHash48(), sreq->m_url);
				continue;
			}
		}

		if (!sreq->m_urlIsDocId) {
			Url url;
			// we don't need to strip parameter here, speed up
			url.set(sreq->m_url, strlen(sreq->m_url), false, false, 122);

			if (isUrlUnwanted(url)) {
				logDebug(g_conf.m_logDebugSpider, "Url is unwanted [%s]", sreq->m_url);
				continue;
			}
		}

		// shortcut
		int64_t uh48 = sreq->getUrlHash48();

		// update request with SpiderReply if newer, because ultimately
		// ::getUrlFilterNum() will just look at SpiderRequest's 
		// version of these bits!
		if (oldRep && repUh48 == uh48 && oldRep->m_spideredTime > sreq->m_addedTime) {
			// if request was a page reindex docid based request and url has since been spidered, nuke it!

			// same if indexcode was EFAKEFIRSTIP which XmlDoc.cpp
			// re-adds to spiderdb with the right firstip. once
			// those guys have a reply we can ignore them.

			if (sreq->m_isPageReindex || sreq->m_fakeFirstIp) {
				continue;
			}

			sreq->m_hasAuthorityInlink = oldRep->m_hasAuthorityInlink;
		}

		// if we are not the same url as last request, then
		// we will not need to dedup, but should add ourselves to
		// the linked list, which we also reset here.
		if ( uh48 != reqUh48 ) {
			spiderRequests.clear();

			// we are the new banner carrier
			reqUh48 = uh48;
		}

		// why does sitehash32 matter really?
		uint32_t srh = sreq->m_siteHash32;
		if ( sreq->m_isInjecting   ) srh ^= 0x42538909;
		if ( sreq->m_isAddUrl      ) srh ^= 0x587c5a0b;
		if ( sreq->m_isPageReindex ) srh ^= 0x70fb3911;
		if ( sreq->m_forceDelete   ) srh ^= 0x4e6e9aee;

		if ( sreq->m_urlIsDocId         ) srh ^= 0xee015b07;
		if ( sreq->m_fakeFirstIp        ) srh ^= 0x95b8d376;

		// if he's essentially different input parms but for the
		// same url, we want to keep him because he might map the
		// url to a different url priority!
		bool skipUs = false;

		// now we keep a list of requests with same uh48
		for (auto it = spiderRequests.begin(); it != spiderRequests.end(); ++it) {
			if (srh != it->first) {
				continue;
			}

			SpiderRequest *prevReq = it->second;

			// skip us if previous guy is better

			// . if we are not the most recent, just do not add us
			if (sreq->m_addedTime >= prevReq->m_addedTime) {
				skipUs = true;
				break;
			}

			// TODO: for pro, base on parentSiteNumInlinks here,
			// we can also have two hashes,
			// m_srh and m_srh2 in the Link class, and if your
			// new secondary hash is unique we can let you in
			// if your parentpageinlinks is the highest of all.


			// otherwise, replace him

			// mark for removal. xttp://
			prevReq->m_url[0] = 'x';

			// no issue with erasing list here as we break out of loop immediately
			spiderRequests.erase(it);

			// make a note of this so we physically remove these
			// entries after we are done with this scan.
			numToFilter++;
			break;
		}

		// if we were not as good as someone that was basically the same SpiderRequest before us, keep going
		if (skipUs) {
			continue;
		}

		// add to linked list
		spiderRequests.emplace_front(srh, (SpiderRequest *)dst);

		// get our size
		int32_t recSize = sreq->getRecSize();

		// and add us
		lastKey = dst;
		memmove(dst, rec, recSize);

		// advance
		dst += recSize;
	}

	// sanity check
	if (dst < list->getList() || dst > list->getListEnd()) {
		g_process.shutdownAbort(true);
	}


	/////////
	//
	// now remove xttp:// urls if we had some
	//
	/////////
	if (numToFilter > 0) {
		// update list so for-loop below works
		list->setListSize(dst - newList);
		list->setListEnd(list->getList() + list->getListSize());
		list->setListPtr(newList);
		list->setListPtrHi(NULL);
		// and we'll re-write everything back into itself at "dst"
		dst = newList;
	}

	for (; !list->isExhausted();) {
		// get rec
		char *rec = list->getCurrentRec();

		// pre skip it (necessary because we manipulate the raw list below)
		list->skipCurrentRecord();

		// skip if negative, just copy over
		if (KEYNEG(rec)) {
			lastKey = dst;
			memmove(dst, rec, sizeof(key128_t));
			dst += sizeof(key128_t);
			continue;
		}

		// is it a reply?
		if (Spiderdb::isSpiderReply((key128_t *)rec)) {
			SpiderReply *srep = (SpiderReply *)rec;
			int32_t recSize = srep->getRecSize();
			lastKey = dst;
			memmove(dst, rec, recSize);
			dst += recSize;
			continue;
		}

		SpiderRequest *sreq = (SpiderRequest *)rec;
		// skip if filtered out
		if (sreq->m_url[0] == 'x') {
			continue;
		}

		int32_t recSize = sreq->getRecSize();
		lastKey = dst;
		memmove(dst, rec, recSize);
		dst += recSize;
	}


	// and stick our newly filtered list in there
	list->setListSize(dst - newList);
	// set to end i guess
	list->setListEnd(list->getList() + list->getListSize());
	list->setListPtr(dst);
	list->setListPtrHi(NULL);

	// log("spiderdb: remove ME!!!");
	// check it
	// list->checkList_r(false,false,RDB_SPIDERDB);
	// list->resetListPtr();

	int32_t delta = oldSize - list->getListSize();
	log( LOG_DEBUG, "spider: deduped %i bytes (of which %i were corrupted) out of %i",
	     (int)delta,(int)corrupt,(int)oldSize);

	if( !lastKey ) {
		logError("lastKey is null. Should not happen?");
	} else {
		list->setLastKey(lastKey);
	}
}




void getSpiderStatusMsg(const CollectionRec *cx, const char **msg, spider_status_t *status) {
	if ( ! g_conf.m_spideringEnabled ) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "Spidering disabled in master controls. You can turn it back on there.";
		return;
	}

	if ( g_conf.m_readOnlyMode ) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "In read-only mode. Spidering off.";
		return;
	}

	if ( g_dailyMerge.m_mergeMode ) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "Daily merge engaged, spidering paused.";
		return;
	}

	if ( g_repairMode ) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "In repair mode, spidering paused.";
		return;
	}

	// do not spider until collections/parms in sync with host #0
	if ( ! g_parms.inSyncWithHost0() ) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "Parms not in sync with host #0, spidering paused";
		return;
	}

	// don't spider if not all hosts are up, or they do not all
	// have the same hosts.conf.
	if ( g_hostdb.hostsConfInDisagreement() ) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "Hosts.conf discrepancy, spidering paused.";
		return;
	}

	// out CollectionRec::m_globalCrawlInfo counts do not have a dead
	// host's counts tallied into it, which could make a difference on
	// whether we have exceed a maxtocrawl limit or some such, so wait...
	if (g_hostdb.hasDeadHost()) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "All crawling temporarily paused because a shard is down.";
		return;
	}

	if ( ! cx->m_spideringEnabled ) {
		*status = spider_status_t::SP_PAUSED;
		*msg = "Spidering disabled in spider controls.";
		return;
	}

	if ( cx->m_spiderStatus == spider_status_t::SP_INITIALIZING ) {
		*status = spider_status_t::SP_INITIALIZING;
		*msg = "Job is initializing.";
		return;
	}

	if ( ! g_conf.m_spideringEnabled ) {
		*status = spider_status_t::SP_ADMIN_PAUSED;
		*msg = "All crawling temporarily paused by root administrator for maintenance.";
		return;
	}

	// otherwise in progress?
	*status = spider_status_t::SP_INPROGRESS;
	*msg = "Spider is in progress.";
}



static int32_t getFakeIpForUrl2(const Url *url2) {
	// make the probable docid
	int64_t probDocId = Titledb::getProbableDocId ( url2 );
	// make one up, like we do in PageReindex.cpp
	int32_t firstIp = (probDocId & 0xffffffff);
	return firstIp;
}



// returns false and sets g_errno on error
bool SpiderRequest::setFromAddUrl(const char *url) {
	logTrace( g_conf.m_logTraceSpider, "BEGIN. url [%s]", url );
		
	// reset it
	reset();
	// make the probable docid
	int64_t probDocId = Titledb::getProbableDocId ( url );

	// make one up, like we do in PageReindex.cpp
	int32_t firstIp = (probDocId & 0xffffffff);

	// ensure not crazy
	if ( firstIp == -1 || firstIp == 0 ) firstIp = 1;

	// . now fill it up
	// . TODO: calculate the other values... lazy!!! (m_isRSSExt, 
	//         m_siteNumInlinks,...)
	m_isAddUrl     = 1;
	m_addedTime    = (uint32_t)getTime();
	m_fakeFirstIp   = 1;
	//m_probDocId     = probDocId;
	m_firstIp       = firstIp;

	// too big?
	if ( strlen(url) > MAX_URL_LEN ) {
		g_errno = EURLTOOLONG;
		logTrace( g_conf.m_logTraceSpider, "END, EURLTOOLONG" );
		return false;
	}
	// the url! includes \0
	strcpy ( m_url , url );
	// call this to set m_dataSize now
	setDataSize();
	// make the key dude -- after setting url
	setKey ( firstIp , 0LL, false );

	// how to set m_firstIp? i guess addurl can be throttled independently
	// of the other urls???  use the hash of the domain for it!
	int32_t  dlen;
	const char *dom = getDomFast ( url , &dlen );

	// sanity
	if ( ! dom ) {
		g_errno = EBADURL;
		logTrace( g_conf.m_logTraceSpider, "END, EBADURL" );
		return false;
		//return sendReply ( st1 , true );
	}

	m_domHash32 = hash32 ( dom , dlen );

	int32_t hlen = 0;
	const char *host = getHostFast(url, &hlen);
	m_hostHash32 = hash32(host, hlen);

	SiteGetter sg;
	sg.getSite(url, nullptr, 0, 0, 0);
	m_siteHash32 = hash32(sg.getSite(), sg.getSiteLen());

	logTrace( g_conf.m_logTraceSpider, "END, done" );
	return true;
}



bool SpiderRequest::setFromInject(const char *url) {
	// just like add url
	if ( ! setFromAddUrl ( url ) ) return false;
	// but fix this
	m_isAddUrl = 0;
	m_isInjecting = 1;
	return true;
}



bool SpiderRequest::isCorrupt() const {
	// more corruption detection
	if ( m_dataSize > (int32_t)sizeof(SpiderRequest) ) {
		log(LOG_WARN, "spider: got corrupt oversize spiderrequest %i", (int)m_dataSize);
		return true;
	}

	if ( m_dataSize <= 0 ) {
		log(LOG_WARN, "spider: got corrupt undersize spiderrequest %i", (int)m_dataSize);
 		return true;
 	}

	// sanity check. check for http(s)://
	if (m_url[0] == 'h' && m_url[1] == 't' && m_url[2] == 't' && m_url[3] == 'p') {
		return false;
	}

	// to be a docid as url must have this set
	if (!m_isPageReindex && !m_urlIsDocId) {
		log(LOG_WARN, "spider: got corrupt 3 spiderRequest");
		return true;
	}

	// might be a docid from a pagereindex.cpp
	if (!is_digit(m_url[0])) {
		log(LOG_WARN, "spider: got corrupt 1 spiderRequest");
		return true;
	}

	// if it is a digit\0 it is ok, not corrupt
	if (!m_url[1]) {
		return false;
	}

	// if it is not a digit after the first digit, that is bad
	if (!is_digit(m_url[1])) {
		log(LOG_WARN, "spider: got corrupt 2 spiderRequest");
		return true;
	}

	const char *p    = m_url + 2;
	const char *pend = m_url + getUrlLen();
	for (; p < pend && *p; p++) {
		// the whole url must be digits, a docid
		if (!is_digit(*p)) {
			log(LOG_WARN, "spider: got corrupt 13 spiderRequest");
			return true;
		}
	}

	return false;
}

