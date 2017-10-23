#include "gb-include.h"

#include "Titledb.h"
#include "Spider.h"
#include "SpiderLoop.h"
#include "Doledb.h"
#include "Tagdb.h"
#include "Clusterdb.h"
#include "Linkdb.h"
#include "Posdb.h"
#include "Dns.h"
#include "TcpServer.h"
#include "UdpServer.h"
#include "Msg51.h"
#include "Pages.h"
#include "Stats.h"
#include <sys/time.h>      // getrlimit()
#include <sys/resource.h>  // getrlimit()
#include "Proxy.h"
#include "Sections.h"
#include "Msg13.h"
#include "Msg3.h"
#include "Mem.h"


static bool printNumAbbr(SafeBuf &p, int64_t vvv) {
	float val = (float)vvv;
	const char *suffix = "K";
	val /= 1024;
	if ( val > 1000.0 ) { val /= 1024.0; suffix = "M"; }
	if ( almostEqualFloat(val, 0.0) ) 
		p.safePrintf("<td>0</td>");
	else if ( suffix[0] =='K' )
		p.safePrintf("<td>%.00f%s</td>",val,suffix);
	else
		p.safePrintf("<td><b>%.01f%s</b></td>",val,suffix);
	return true;
}

static bool printUptime(SafeBuf &sb) {
	int32_t uptime = time(NULL) - g_stats.m_uptimeStart ;
	// sanity check... wtf?
	if ( uptime < 0 ) { uptime = 0; }

	int32_t days  = uptime / 86400; uptime -= days  * 86400;
	int32_t hours = uptime /  3600; uptime -= hours * 3600;
	int32_t mins  = uptime /    60; uptime -= mins  * 60;
	int32_t secs  = uptime;

	// singular plural
	const char *ds = "day";
	if ( days != 1 ) ds = "days";
	const char *hs = "hour";
	if ( hours != 1 ) hs = "hours";
	const char *ms = "minute";
	if ( mins != 1 ) ms = "minutes";
	const char *ss = "seconds";
	if ( secs == 1 ) ss = "second";
	
	if ( days >= 1 )
		sb.safePrintf("%" PRId32" %s ",days,ds);

	if ( hours >= 1 )
		sb.safePrintf("%" PRId32" %s ", hours,hs);

	if ( mins >= 1 )
		sb.safePrintf("%" PRId32" %s ", mins,ms);

	if ( secs != 0 ) 
		sb.safePrintf(" %" PRId32" %s",secs,ss);
	return true;
}

// . returns false if blocked, true otherwise
// . sets errno on error
// . make a web page displaying the config of this host
// . call g_httpServer.sendDynamicPage() to send it
bool sendPageStats ( TcpSocket *s , HttpRequest *r ) {
	// don't allow pages bigger than 128k in cache
	StackBuf<64*1024> p;
	char format = r->getReplyFormat();

	// print standard header
	// 	char *ss = p.getBuf();
	// 	char *ssend = p.getBufEnd();
	if ( format == FORMAT_HTML ) g_pages.printAdminTop ( &p , s , r );
	//      p.incrementLength(sss - ss);

	struct rusage ru;
	if ( getrusage ( RUSAGE_SELF , &ru ) )
		log("admin: getrusage: %s.",mstrerror(errno));

	if ( format == FORMAT_HTML ) {
		p.safePrintf(
			     "<style>"
			     ".poo { background-color:#%s;}\n"
			     "</style>\n" ,
			     LIGHT_BLUE );

	// memory in general table
		p.safePrintf (
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=2>"
			      "<center><b>Memory</b></td></tr>\n"
			      "<tr class=poo><td><b>memory allocated</b>"
			      "</td><td>%" PRId64"</td></tr>\n"
			      "<tr class=poo><td><b>max memory limit</b>"
			      "</td><td>%" PRId64"</td></tr>\n"
			      "<tr class=poo><td>max allocated</td>"
			      "<td>%" PRId64"</td></tr>\n",
			      TABLE_STYLE ,
			      g_mem.getUsedMem() ,
			      g_mem.getMaxMem() ,
			      g_mem.getMaxAllocated()
			      );
		p.safePrintf (
			      "<tr class=poo><td>max single alloc</td>"
			      "<td>%" PRId64"</td></tr>\n"
			      "<tr class=poo><td>max single alloc by</td>"
			      "<td>%s</td></tr>\n"
			      "<tr class=poo><td># out of memory errors</td>"
			      "<td>%" PRId32"</td></tr>\n"

			      "<tr class=poo><td>swaps</td>"
			      "<td>%" PRId64"</td></tr>\n"
			      ,
			      g_mem.getMaxAlloc(),
			      g_mem.getMaxAllocBy() ,

			      g_mem.getOOMCount(),

			      (int64_t)ru.ru_nswap
			      ); 
		p.safePrintf (
			      "<tr class=poo><td><b>current allocations</b>"
			      "</td>"
			      "<td>%" PRId32"</td></tr>\n"


			      "<tr class=poo><td><b>max allocations</b>"
			      "</td>"
			      "<td>%" PRId32"</td></tr>\n"


			      "<tr class=poo><td><b>total allocations</b></td>"
			      "<td>%" PRId64"</td></tr>\n" ,
			      g_mem.getNumAllocated() ,
			      g_mem.getMemTableSize(),
			      (int64_t)g_mem.getNumTotalAllocated() );

	}



	if ( format == FORMAT_XML ) 
		p.safePrintf("<response>\n"
			     "\t<statusCode>0</statusCode>\n"
			     "\t<statusMsg>Success</statusMsg>\n");

	if ( format == FORMAT_JSON ) 
		p.safePrintf("{\"response\":{\n"
			     "\t\"statusCode\":0,\n"
			     "\t\"statusMsg\":\"Success\",\n");


	if ( format == FORMAT_XML ) 
		p.safePrintf ("\t<memoryStats>\n"
			      "\t\t<allocated>%" PRId64"</allocated>\n"
			      "\t\t<max>%" PRId64"</max>\n"
			      "\t\t<maxAllocated>%" PRId64"</maxAllocated>\n"
			      "\t\t<maxSingleAlloc>%" PRId64"</maxSingleAlloc>\n"
			      "\t\t<maxSingleAllocBy>%s</maxSingleAllocBy>\n"
			      "\t\t<currentAllocations>%" PRId32
			      "</currentAllocations>\n"
			      "\t\t<totalAllocations>%" PRId64"</totalAllocations>\n"
			      "\t</memoryStats>\n"
			      , g_mem.getUsedMem()
			      , g_mem.getMaxMem() 
			      , g_mem.getMaxAllocated() 
			      , g_mem.getMaxAlloc()
			      , g_mem.getMaxAllocBy() 
			      , g_mem.getNumAllocated() 
			      , (int64_t)g_mem.getNumTotalAllocated() );

	if ( format == FORMAT_JSON ) 
		p.safePrintf ("\t\"memoryStats\":{\n"
			      "\t\t\"allocated\":%" PRId64",\n"
			      "\t\t\"max\":%" PRId64",\n"
			      "\t\t\"maxAllocated\":%" PRId64",\n"
			      "\t\t\"maxSingleAlloc\":%" PRId64",\n"
			      "\t\t\"maxSingleAllocBy\":\"%s\",\n"
			      "\t\t\"currentAllocations\":%" PRId32",\n"
			      "\t\t\"totalAllocations\":%" PRId64"\n"
			      "\t},\n"
			      , g_mem.getUsedMem()
			      , g_mem.getMaxMem() 
			      , g_mem.getMaxAllocated() 
			      , g_mem.getMaxAlloc()
			      , g_mem.getMaxAllocBy() 
			      , g_mem.getNumAllocated() 
			      , (int64_t)g_mem.getNumTotalAllocated() );


			     

	// end table
	if ( format == FORMAT_HTML ) p.safePrintf ( "</table><br>" );

	//Query performance stats

	if ( format == FORMAT_HTML )
		p.safePrintf ( 
			      "<br>"
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=2>"
			      "<center><b>Queries</b></td></tr>\n", TABLE_STYLE);

	int64_t total = 0;
	for ( int32_t i = 0 ; i <= CR_OK ; i++ )
		total += g_stats.m_filterStats[i];


	if ( format == FORMAT_HTML )
		p.safePrintf ( "<tr class=poo><td><b>Total DocIds Generated"
			       "</b></td><td>%" PRId64
			       "</td></tr>\n" , total );


	if ( format == FORMAT_XML )
		p.safePrintf ( "\t<totalDocIdsGenerated>%" PRId64
			       "</totalDocIdsGenerated>\n" , total );

	if ( format == FORMAT_JSON )
		p.safePrintf ( "\t\"totalDocIdsGenerated\":%" PRId64",\n",total);

	// print each filter stat
	for ( int32_t i = 0 ; i < CR_END ; i++ ) {
		if ( format == FORMAT_HTML )
			p.safePrintf("<tr class=poo><td>&nbsp;&nbsp;%s</td>"
				     "<td>%" PRId32"</td></tr>\n" ,
				     g_crStrings[i],g_stats.m_filterStats[i] );
		if ( format == FORMAT_XML )
			p.safePrintf("\t<queryStat>\n"
				     "\t\t<status><![CDATA[%s]]>"
				     "</status>\n"
				     "\t\t<count>%" PRId32"</count>\n"
				     "\t</queryStat>\n"
				     ,g_crStrings[i],g_stats.m_filterStats[i]);
		if ( format == FORMAT_JSON )
			p.safePrintf("\t\"queryStat\":{\n"
				     "\t\t\"status\":\"%s\",\n"
				     "\t\t\"count\":%" PRId32"\n"
				     "\t},\n"
				     ,g_crStrings[i],g_stats.m_filterStats[i]);
	}

	if ( format == FORMAT_HTML ) p.safePrintf("</table><br><br>\n");

	// stripe loads
	if ( g_hostdb.m_myHost->m_isProxy ) {
		p.safePrintf ( 
			      "<br>"
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=4>"
			      "<center><b>Stripe Loads</b></td></tr>\n" ,
			      TABLE_STYLE  );
		p.safePrintf("<tr class=poo><td><b>Stripe #</b></td>"
			     "<td><b>Queries Out</b></td>"
			     "<td><b>Query Terms Out</b></td>"
			     "<td><b>Last HostId Used</b></td>"
			     "</tr>\n" );
		// print out load of each stripe
		int32_t numStripes = g_hostdb.getNumStripes();
		for ( int32_t i = 0 ; i < numStripes ; i++ )
			p.safePrintf("<tr class=poo><td>%" PRId32"</td>"
				     "<td>%" PRId32"</td>"
				     "<td>%" PRId32"</td>"
				     "<td>%" PRId32"</td></tr>\n" ,
				     i , 
				     g_proxy.m_queriesOutOnStripe [i],
				     g_proxy.m_termsOutOnStripe   [i],
				     g_proxy.m_stripeLastHostId   [i]);
		// close table
		p.safePrintf("</table><br><br>\n");
	}

	//
	// print cache table
	// columns are the caches
	//

	int32_t numCaches = 0;
	const RdbCache *caches[20];
	caches[numCaches++] = Msg13::getHttpCacheRobots();
	caches[numCaches++] = Msg13::getHttpCacheOthers();
	caches[numCaches++] = g_dns.getCache();
	caches[numCaches++] = g_dns.getCacheLocal();
	auto const winnerlist_statistics = g_spiderLoop.m_winnerListCache.query_statistics();

	if ( format == FORMAT_HTML ) {
		p.safePrintf (
		  "<table %s>"
		  "<tr class=hdrow>"
		  "<td colspan=%" PRId32">"
		  "<center><b>Caches"
		  "</b></td></tr>\n",
		  TABLE_STYLE,
		  numCaches+2 );

		// 1st column is empty
		p.safePrintf ("<tr class=poo><td>&nbsp;</td>");  
	}

	for ( int32_t i = 0 ; format == FORMAT_XML && i < numCaches ; i++ ) {
		p.safePrintf("\t<cacheStats>\n");
		p.safePrintf("\t\t<name>%s</name>\n",caches[i]->getDbname());
		int64_t a = caches[i]->getNumHits();
		int64_t b = caches[i]->getNumMisses();
		double r = 100.0 * (double)a / (double)(a+b);
		p.safePrintf("\t\t<hitRatio>");
		if ( a+b > 0.0 ) p.safePrintf("%.1f%%",r);
		p.safePrintf("</hitRatio>\n");
		p.safePrintf("\t\t<numHits>%" PRId64"</numHits>\n",a);
		p.safePrintf("\t\t<numMisses>%" PRId64"</numMisses>\n",b);
		p.safePrintf("\t\t<numTries>%" PRId64"</numTries>\n",a+b);

		p.safePrintf("\t\t<numUsedSlots>%" PRId32"</numUsedSlots>\n",
			     caches[i]->getNumUsedNodes());
		p.safePrintf("\t\t<numTotalSlots>%" PRId32"</numTotalSlots>\n",
			     caches[i]->getNumTotalNodes());
		p.safePrintf("\t\t<bytesUsed>%" PRId32"</bytesUsed>\n",
			     caches[i]->getMemOccupied());
		p.safePrintf("\t\t<maxBytes>%" PRId32"</maxBytes>\n",
			     caches[i]->getMaxMem());
		p.safePrintf("\t\t<saveToDisk>%" PRId32"</saveToDisk>\n",
			     (int32_t)caches[i]->useDisk());
		p.safePrintf("\t</cacheStats>\n");
	}
	if(format==FORMAT_XML) {
		p.safePrintf("\t<cacheStats>\n");
		p.safePrintf("\t\t<name>%s</name>\n","winnercache");
		int64_t a = winnerlist_statistics.lookup_hits;
		int64_t b = winnerlist_statistics.lookup_misses;
		double r = 100.0 * (double)a / (double)(a+b);
		p.safePrintf("\t\t<hitRatio>");
		if(a+b > 0.0) p.safePrintf("%.1f%%",r);
		p.safePrintf("</hitRatio>\n");
		p.safePrintf("\t\t<numHits>%lu</numHits>\n", winnerlist_statistics.lookup_hits);
		p.safePrintf("\t\t<numMisses>%lu</numMisses>\n", winnerlist_statistics.lookup_misses);
		p.safePrintf("\t\t<numTries>%lu</numTries>\n", winnerlist_statistics.lookups);

		p.safePrintf("\t\t<numUsedSlots>%u</numUsedSlots>\n", winnerlist_statistics.items);
		p.safePrintf("\t\t<numTotalSlots>%u</numTotalSlots>\n", winnerlist_statistics.max_items);
		p.safePrintf("\t\t<bytesUsed>%zu</bytesUsed>\n", winnerlist_statistics.memory_used);
		p.safePrintf("\t\t<maxBytes>%zu</maxBytes>\n", winnerlist_statistics.max_memory);
		p.safePrintf("\t\t<saveToDisk>%d</saveToDisk>\n", 0);
		p.safePrintf("\t</cacheStats>\n");
	}

	for ( int32_t i = 0 ; format == FORMAT_JSON && i < numCaches ; i++ ) {
		p.safePrintf("\t\"cacheStats\":{\n");
		p.safePrintf("\t\t\"name\":\"%s\",\n",caches[i]->getDbname());
		int64_t a = caches[i]->getNumHits();
		int64_t b = caches[i]->getNumMisses();
		double r = 100.0 * (double)a / (double)(a+b);
		p.safePrintf("\t\t\"hitRatio\":\"");
		if ( a+b > 0.0 ) p.safePrintf("%.1f%%",r);
		p.safePrintf("\",\n");
		p.safePrintf("\t\t\"numHits\":%" PRId64",\n",a);
		p.safePrintf("\t\t\"numMisses\":%" PRId64",\n",b);
		p.safePrintf("\t\t\"numTries\":%" PRId64",\n",a+b);

		p.safePrintf("\t\t\"numUsedSlots\":%" PRId32",\n",
			     caches[i]->getNumUsedNodes());
		p.safePrintf("\t\t\"numTotalSlots\":%" PRId32",\n",
			     caches[i]->getNumTotalNodes());
		p.safePrintf("\t\t\"bytesUsed\":%" PRId32",\n",
			     caches[i]->getMemOccupied());
		p.safePrintf("\t\t\"maxBytes\":%" PRId32",\n",
			     caches[i]->getMaxMem());
		p.safePrintf("\t\t\"saveToDisk\":%" PRId32"\n",
			     (int32_t)caches[i]->useDisk());
		p.safePrintf("\t},\n");
	}
	if(format==FORMAT_JSON) {
		p.safePrintf("\t\"cacheStats\":{\n");
		p.safePrintf("\t\t\"name\":\"%s\",\n","winnercache");
		int64_t a = winnerlist_statistics.lookup_hits;
		int64_t b = winnerlist_statistics.lookup_misses;
		double r = 100.0 * (double)a / (double)(a+b);
		p.safePrintf("\t\t\"hitRatio\":\"");
		if ( a+b > 0.0 ) p.safePrintf("%.1f%%",r);
		p.safePrintf("\",\n");
		p.safePrintf("\t\t\"numHits\":%lu,\n", winnerlist_statistics.lookup_hits);
		p.safePrintf("\t\t\"numMisses\":%lu,\n", winnerlist_statistics.lookup_misses);
		p.safePrintf("\t\t\"numTries\":%lu,\n", winnerlist_statistics.lookups);

		p.safePrintf("\t\t\"numUsedSlots\":%u,\n", winnerlist_statistics.items);
		p.safePrintf("\t\t\"numTotalSlots\":%u,\n", winnerlist_statistics.max_items);
		p.safePrintf("\t\t\"bytesUsed\":%zu,\n", winnerlist_statistics.memory_used);
		p.safePrintf("\t\t\"maxBytes\":%zu,\n", winnerlist_statistics.max_memory);
		p.safePrintf("\t\t\"saveToDisk\":%d\n", 0);
		p.safePrintf("\t},\n");
	}


	// do not print any more if xml or json
	if ( format == FORMAT_XML || format == FORMAT_JSON )
		goto skip1;

	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		p.safePrintf("<td><b>%s</b></td>",caches[i]->getDbname() );
	}
	p.safePrintf("<td><b>%s</b></td>", "winnercache");
	//p.safePrintf ("<td><b><i>Total</i></b></td></tr>\n" );

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>hit ratio</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getNumHits();
		int64_t b = caches[i]->getNumMisses();
		double r = 100.0 * (double)a / (double)(a+b);
		if ( a+b > 0.0 ) 
			p.safePrintf("<td>%.1f%%</td>",r);
		else
			p.safePrintf("<td>--</td>");
	}
	p.safePrintf("<td>--</td>"); //todo

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>hits</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getNumHits();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>%lu</td>", winnerlist_statistics.lookup_hits);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>tries</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getNumHits();
		int64_t b = caches[i]->getNumMisses();
		p.safePrintf("<td>%" PRId64"</td>",a+b);
	}
	p.safePrintf("<td>%lu</td>", winnerlist_statistics.lookups);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>used slots</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getNumUsedNodes();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>%u</td>", winnerlist_statistics.items);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>max slots</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getNumTotalNodes();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>%u</td>", winnerlist_statistics.max_items);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>used bytes</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getMemOccupied();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>%zu</td>", winnerlist_statistics.memory_used);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>max bytes</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getMaxMem();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>%zu</td>", winnerlist_statistics.max_memory);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>dropped recs</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getNumDeletes();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>%lu</td>", winnerlist_statistics.removes);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>added recs</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->getNumAdds();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>%lu</td>", winnerlist_statistics.inserts);

	p.safePrintf ("</tr>\n<tr class=poo><td><b><nobr>save to disk</nobr></b></td>" );
	for ( int32_t i = 0 ; i < numCaches ; i++ ) {
		int64_t a = caches[i]->useDisk();
		p.safePrintf("<td>%" PRId64"</td>",a);
	}
	p.safePrintf("<td>0</td>");

	// end the table now
	p.safePrintf ( "</tr>\n</table><br><br>" );

 skip1:

	// 
	// General Info Table
	//
	FILE *ff = fopen ("/proc/version", "r" );
	const char *kv = "unknown";
	char kbuf[1024];
	//char kbuf2[1024];
	if ( ff ) {
		fgets ( kbuf , 1000 , ff );
		//sscanf ( kbuf , "%*s %*s %s %*s", kbuf2 );
		//kv = kbuf2;
		kv = kbuf;
		fclose(ff);
	}
        time_t now = getTimeLocal();
	struct tm tm_buf;
	char nowStr[64];
	char buf2[64];
	sprintf ( nowStr , "%s UTC", asctime_r(gmtime_r(&now,&tm_buf),buf2) );

	// replace \n in nowstr with space
	char *nn = strstr(nowStr,"\n");
	if ( nn ) *nn = ' ';


	//
	// get the uptime as a string
	//
	SafeBuf ubuf;
	printUptime ( ubuf );

	const int arch = __WORDSIZE;

	if ( format == FORMAT_HTML )
		p.safePrintf (
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=2>"
			      "<center><b>General Info</b></td></tr>\n"
			      "<tr class=poo><td><b>Uptime</b></td><td>%s</td></tr>\n"
			      "<tr class=poo><td><b>Process ID</b></td><td>%" PRIu32"</td></tr>\n"
			      "<tr class=poo><td><b>Corrupted Disk Reads</b></td><td>%" PRId32"</td></tr>\n"

			      //"<tr class=poo><td><b>read signals</b></td><td>%" PRId64"</td></tr>\n"
			      //"<tr class=poo><td><b>write signals</b></td><td>%" PRId64"</td></tr>\n"
			      "<tr class=poo><td><b>Kernel Version</b></td><td>%s</td></tr>\n"
			      "<tr class=poo><td><b>Gigablast Architecture</b></td><td>%i bit</td></tr>\n"
			      
			      //"<tr class=poo><td><b>Gigablast Version</b></td><td>%s %s</td></tr>\n"
			      "<tr class=poo><td><b>Parsing Inconsistencies</b></td><td>%" PRId32"</td>\n"

			      // overflows. when we have too many unindexed 
			      // spiderrequests for a particular firstip, we 
			      // start dropping so we don't spam spiderdb
			      "<tr class=poo><td><b>Dropped Spider Requests</b></td><td>%" PRId32"</td>\n"

			      "<tr class=poo><td><b>Index Shards</b></td><td>%" PRId32"</td>\n"
			      "<tr class=poo><td><b>Hosts per Shard</b></td><td>%" PRId32"</td>\n"
			      //"<tr class=poo><td><b>Fully Split</b></td><td>%" PRId32"</td>\n"
			      //"<tr class=poo><td><b>Tfndb Extension Bits</b></td><td>%" PRId32"</td>\n"
			      "</tr>\n"
			      "<tr class=poo><td><b>Spider Locks</b></td><td>%" PRId32"</td></tr>\n"
			      "<tr class=poo><td><b>Local Time</b></td><td>%s (%" PRId32")</td></tr>\n"
			      ,
			      TABLE_STYLE ,
			      ubuf.getBufStart(),
			      (uint32_t)getpid(),
			      g_numCorrupt,

			      //g_stats.m_readSignals,
			      //g_stats.m_writeSignals,
			      kv , 
			      arch,
			      //GBPROJECTNAME,
			      //GBVersion ,
			      g_stats.m_parsingInconsistencies ,
			      g_stats.m_totalOverflows,
			      (int32_t)g_hostdb.getNumShards(),//g_hostdb.m_indexSplits,
			      (int32_t)g_hostdb.getNumHostsPerShard(),
			      g_spiderLoop.getLockCount(),
			      //(int32_t)g_conf.m_fullSplit,
			      //(int32_t)g_conf.m_tfndbExtBits,
			      nowStr,
			      (int32_t)now);//ctime(&time));

	if ( format == FORMAT_XML ) 
		p.safePrintf (
			      "\t<generalStats>\n"
			      "\t\t<uptime>%s</uptime>\n"

			      "\t\t<corruptedDiskReads>%" PRId32
			      "</corruptedDiskReads>\n"

			      "\t\t<kernelVersion><![CDATA[%s]]>"
			      "</kernelVersion>\n"

			      "\t\t<gigablastArchitecture><![CDATA[%i bit]]>"
			      "</gigablastArchitecture>\n"

			      "\t\t<parsingInconsistencies>%" PRId32
			      "</parsingInconsistencies>\n"

			      "\t\t<numShards>%" PRId32"</numShards>\n"

			      "\t\t<hostsPerShard>%" PRId32"</hostsPerShard>\n"

			      "\t\t<spiderLocks>%" PRId32"</spiderLocks>\n"

			      "\t\t<localTimeStr>%s</localTimeStr>\n"
			      "\t\t<localTime>%" PRId32"</localTime>\n"
			      ,
			      ubuf.getBufStart(),
			      g_numCorrupt,
			      kv , 
			      arch,
			      g_stats.m_parsingInconsistencies ,
			      (int32_t)g_hostdb.getNumShards(),
			      (int32_t)g_hostdb.getNumHostsPerShard(),
			      g_spiderLoop.getLockCount(),
			      nowStr,
			      (int32_t)now);

	if ( format == FORMAT_JSON ) {
		p.safePrintf (
			      "\t\"generalStats\":{\n"
			      "\t\t\"uptime\":\"%s\",\n"

			      "\t\t\"corruptedDiskReads\":%" PRId32",\n"

			      "\t\t\"kernelVersion\":\""
			      ,
			      ubuf.getBufStart(),
			      g_numCorrupt);
		// this has quotes in it
		p.jsonEncode(kv);
		p.safePrintf( "\",\n"

			      "\t\t\"gigablastArchitecture\":\"%i bit\",\n"
			      "\t\t\"parsingInconsistencies\":%" PRId32",\n"

			      "\t\t\"numShards\":%" PRId32",\n"

			      "\t\t\"hostsPerShard\":%" PRId32",\n"

			      "\t\t\"spiderLocks\":%" PRId32",\n"

			      "\t\t\"localTimeStr\":\"%s\",\n"
			      "\t\t\"localTime\":%" PRId32",\n"
			      ,
			      arch,
			      g_stats.m_parsingInconsistencies ,
			      (int32_t)g_hostdb.getNumShards(),
			      (int32_t)g_hostdb.getNumHostsPerShard(),
			      g_spiderLoop.getLockCount(),
			      nowStr,
			      (int32_t)now);
	}

	// end table
	time_t nowg = getTimeGlobal();
	{
		struct tm tm_buf;
		char buf[64];
		sprintf(nowStr, "%s UTC", asctime_r(gmtime_r(&nowg, &tm_buf), buf));
	}

	// replace \n in nowstr with space
	char *sn = strstr(nowStr,"\n");
	if ( sn ) *sn = ' ';


	if ( format == FORMAT_HTML )
		p.safePrintf ( "<tr class=poo><td><b>Global Time</b></td>"
			       "<td>%s (%" PRId32")</td></tr>\n"
			       "</table><br><br>", nowStr,
			       (int32_t)nowg);//ctime(&time))

	if ( format == FORMAT_XML )
		p.safePrintf ( "\t\t<globalTimeStr>%s</globalTimeStr>\n"
			       "\t\t<globalTime>%" PRId32"</globalTime>\n"
			       "\t</generalStats>\n"
			       ,nowStr,(int32_t)nowg);

	if ( format == FORMAT_JSON )
		p.safePrintf ( "\t\t\"globalTimeStr\":\"%s\",\n"
			       "\t\t\"globalTime\":%" PRId32"\n"
			       "\t},\n"
			       ,nowStr,(int32_t)nowg);


	//
	// print network stats
	//
	if ( format == FORMAT_HTML )
		p.safePrintf ( 
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=2 class=hdrow>"
			      "<center><b>Network</b></td></tr>\n"

			      "<tr class=poo><td><b>http server "
			      "bytes downloaded</b>"
			      "</td><td>%" PRIu64"</td></tr>\n"

			      "<tr class=poo><td><b>http server "
			      "bytes downloaded (uncompressed)</b>"
			      "</td><td>%" PRIu64"</td></tr>\n"

			      "<tr class=poo><td><b>http server "
			      "compression ratio</b>"
			      "</td><td>%.02f</td></tr>\n"
			      

			      "<tr class=poo><td><b>ip1 bytes/packets in</b>"
			      "</td><td>%" PRIu64" / %" PRIu64"</td></tr>\n"

			      "<tr class=poo><td><b>ip1 bytes/packets out</b>"
			      "</td><td>%" PRIu64" / %" PRIu64"</td></tr>\n"

			      "<tr class=poo><td><b>ip2 bytes/packets in</b>"
			      "</td><td>%" PRIu64" / %" PRIu64"</td></tr>\n"

			      "<tr class=poo><td><b>ip2 bytes/packets out</b>"
			      "</td><td>%" PRIu64" / %" PRIu64"</td></tr>\n"

			      "<tr class=poo><td><b>cancel acks sent</b>"
			      "</td><td>%" PRId32"</td></tr>\n"
			      "<tr class=poo><td><b>cancel acks read</b>"
			      "</td><td>%" PRId32"</td></tr>\n"
			      "<tr class=poo><td><b>dropped dgrams</b>"
			      "</td><td>%" PRId32"</td></tr>\n"
			      ,
			      TABLE_STYLE,

			      g_httpServer.m_bytesDownloaded,
			      g_httpServer.m_uncompressedBytes,
			      g_httpServer.getCompressionRatio(),

			      g_udpServer.m_eth0BytesIn.load(),
			      g_udpServer.m_eth0PacketsIn.load(),

			      g_udpServer.m_eth0BytesOut.load(),
			      g_udpServer.m_eth0PacketsOut.load(),

			      g_udpServer.m_eth1BytesIn.load(),
			      g_udpServer.m_eth1PacketsIn.load(),

			      g_udpServer.m_eth1BytesOut.load(),
			      g_udpServer.m_eth1PacketsOut.load(),

			      g_cancelAcksSent,
			      g_cancelAcksRead,
			      g_dropped
			       );


	if ( format == FORMAT_XML )
		p.safePrintf ( 
			      "\t<networkStats>\n"

			      "\t\t<httpServerBytesDownloaded>%" PRIu64
			      "</httpServerBytesDownloaded>\n"

			      "\t\t<httpServerBytesDownloadedUncompressed>%" PRIu64
			      "</httpServerBytesDownloadedUncompressed>\n"

			      "\t\t<httpServerCompressionRatio>%.02f"
			      "</httpServerCompressionRatio>\n"

			      "\t\t<ip1BytesIn>%" PRIu64"</ip1BytesIn>\n"
			      "\t\t<ip1PacketsIn>%" PRIu64"</ip1PacketsIn>\n"

			      "\t\t<ip1BytesOut>%" PRIu64"</ip1BytesOut>\n"
			      "\t\t<ip1PacketsOut>%" PRIu64"</ip1PacketsOut>\n"

			      "\t\t<ip2BytesIn>%" PRIu64"</ip2BytesIn>\n"
			      "\t\t<ip2PacketsIn>%" PRIu64"</ip2PacketsIn>\n"

			      "\t\t<ip2BytesOut>%" PRIu64"</ip2BytesOut>\n"
			      "\t\t<ip2PacketsOut>%" PRIu64"</ip2PacketsOut>\n"

			      "\t\t<cancelAcksSent>%" PRId32"</cancelAcksSent>\n"

			      "\t\t<cancelAcksRead>%" PRId32"</cancelAcksRead>\n"

			      "\t\t<droppedDgrams>%" PRId32"</droppedDgrams>\n"

			      "\t</networkStats>\n"

			      ,

			      g_httpServer.m_bytesDownloaded,
			      g_httpServer.m_uncompressedBytes,
			      g_httpServer.getCompressionRatio(),


			      g_udpServer.m_eth0BytesIn.load(),
			      g_udpServer.m_eth0PacketsIn.load(),

			      g_udpServer.m_eth0BytesOut.load(),
			      g_udpServer.m_eth0PacketsOut.load(),

			      g_udpServer.m_eth1BytesIn.load(),
			      g_udpServer.m_eth1PacketsIn.load(),

			      g_udpServer.m_eth1BytesOut.load(),
			      g_udpServer.m_eth1PacketsOut.load(),

			      g_cancelAcksSent,
			      g_cancelAcksRead,
			      g_dropped
			       );

	if ( format == FORMAT_JSON )
		p.safePrintf ( 
			      "\t\"networkStats\":{\n"


			      "\t\t\"httpServerBytesDownloaded\":%" PRIu64",\n"
			      "\t\t\"httpServerBytesDownloadedUncompressed\""
			      ":%" PRIu64",\n"
			      "\t\t\"httpServerCompressionRatio\":%.02f,\n"

			      "\t\t\"ip1BytesIn\":%" PRIu64",\n"
			      "\t\t\"ip1PacketsIn\":%" PRIu64",\n"

			      "\t\t\"ip1BytesOut\":%" PRIu64",\n"
			      "\t\t\"ip1PacketsOut\":%" PRIu64",\n"

			      "\t\t\"ip2BytesIn\":%" PRIu64",\n"
			      "\t\t\"ip2PacketsIn\":%" PRIu64",\n"

			      "\t\t\"ip2BytesOut\":%" PRIu64",\n"
			      "\t\t\"ip2PacketsOut\":%" PRIu64",\n"

			      "\t\t\"cancelAcksSent\":%" PRId32",\n"

			      "\t\t\"cancelAcksRead\":%" PRId32",\n"

			      "\t\t\"droppedDgrams\":%" PRId32",\n"

			      "\t},\n"

			      ,

			      g_httpServer.m_bytesDownloaded,
			      g_httpServer.m_uncompressedBytes,
			      g_httpServer.getCompressionRatio(),


			      g_udpServer.m_eth0BytesIn.load(),
			      g_udpServer.m_eth0PacketsIn.load(),

			      g_udpServer.m_eth0BytesOut.load(),
			      g_udpServer.m_eth0PacketsOut.load(),

			      g_udpServer.m_eth1BytesIn.load(),
			      g_udpServer.m_eth1PacketsIn.load(),

			      g_udpServer.m_eth1BytesOut.load(),
			      g_udpServer.m_eth1PacketsOut.load(),

			      g_cancelAcksSent,
			      g_cancelAcksRead,
			      g_dropped
			       );

	if ( format == FORMAT_HTML ) 
		p.safePrintf ( "</table><br><br>\n" );


	if ( g_hostdb.m_myHost->m_isProxy ) {

	p.safePrintf ( 
		       "<table %s>"
		       "<tr class=hdrow>"
		       "<td colspan=50>"
		       "<center><b>Spider Compression Proxy Stats</b> "

		       " &nbsp; [<a href=\"/admin/stats?reset=2\">"
		       "reset</a>]</td></tr>\n"

		       "<tr class=poo>"
		       "<td><b>type</b></td>\n"
		       "<td><b>#docs</b></td>\n"
		       "<td><b>bytesIn / bytesOut (ratio)</b></td>\n"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"

		       "<tr class=poo>"
		       "<td>%s</td>"
		       "<td>%" PRIu64"</td>"
		       "<td>%" PRIu64" / %" PRIu64" (%.02f)</td>"
		       "</tr>"
		       ,
		       TABLE_STYLE,

		       "All",
		       g_stats.m_compressAllDocs,
		       g_stats.m_compressAllBytesIn,
		       g_stats.m_compressAllBytesOut,
		       (float)g_stats.m_compressAllBytesIn/
		       (float)g_stats.m_compressAllBytesOut,

		       "MimeError",
		       g_stats.m_compressMimeErrorDocs,
		       g_stats.m_compressMimeErrorBytesIn,
		       g_stats.m_compressMimeErrorBytesOut,
		       (float)g_stats.m_compressMimeErrorBytesIn/
		       (float)g_stats.m_compressMimeErrorBytesOut,

		       "Unchanged",
		       g_stats.m_compressUnchangedDocs,
		       g_stats.m_compressUnchangedBytesIn,
		       g_stats.m_compressUnchangedBytesOut,
		       (float)g_stats.m_compressUnchangedBytesIn/
		       (float)g_stats.m_compressUnchangedBytesOut,

		       "BadContent",
		       g_stats.m_compressBadContentDocs,
		       g_stats.m_compressBadContentBytesIn,
		       g_stats.m_compressBadContentBytesOut,
		       (float)g_stats.m_compressBadContentBytesIn/
		       (float)g_stats.m_compressBadContentBytesOut,


		       "BadCharset",
		       g_stats.m_compressBadCharsetDocs,
		       g_stats.m_compressBadCharsetBytesIn,
		       g_stats.m_compressBadCharsetBytesOut,
		       (float)g_stats.m_compressBadCharsetBytesIn/
		       (float)g_stats.m_compressBadCharsetBytesOut,

		       "BadContentType",
		       g_stats.m_compressBadCTypeDocs,
		       g_stats.m_compressBadCTypeBytesIn,
		       g_stats.m_compressBadCTypeBytesOut,
		       (float)g_stats.m_compressBadCTypeBytesIn/
		       (float)g_stats.m_compressBadCTypeBytesOut,

		       "BadLang",
		       g_stats.m_compressBadLangDocs,
		       g_stats.m_compressBadLangBytesIn,
		       g_stats.m_compressBadLangBytesOut,
		       (float)g_stats.m_compressBadLangBytesIn/
		       (float)g_stats.m_compressBadLangBytesOut,
		       
		       //"HasIframe",
		       //g_stats.m_compressHasIframeDocs,
		       //g_stats.m_compressHasIframeBytesIn,
		       //g_stats.m_compressHasIframeBytesOut,
		       //(float)g_stats.m_compressHasIframeBytesIn/
		       //(float)g_stats.m_compressHasIframeBytesOut,
		       

		       "FullPageRequested",
		       g_stats.m_compressFullPageDocs,
		       g_stats.m_compressFullPageBytesIn,
		       g_stats.m_compressFullPageBytesOut,
		       (float)g_stats.m_compressFullPageBytesIn/
		       (float)g_stats.m_compressFullPageBytesOut,

		       "PlainLink",
		       g_stats.m_compressPlainLinkDocs,
		       g_stats.m_compressPlainLinkBytesIn,
		       g_stats.m_compressPlainLinkBytesOut,
		       (float)g_stats.m_compressPlainLinkBytesIn/
		       (float)g_stats.m_compressPlainLinkBytesOut,
		       
		       "EmptyLink",
		       g_stats.m_compressEmptyLinkDocs,
		       g_stats.m_compressEmptyLinkBytesIn,
		       g_stats.m_compressEmptyLinkBytesOut,
		       (float)g_stats.m_compressEmptyLinkBytesIn/
		       (float)g_stats.m_compressEmptyLinkBytesOut,

		       "HasDateAndAddress",
		       g_stats.m_compressHasDateDocs,
		       g_stats.m_compressHasDateBytesIn,
		       g_stats.m_compressHasDateBytesOut,
		       (float)g_stats.m_compressHasDateBytesIn/
		       (float)g_stats.m_compressHasDateBytesOut,

		       "RobotsTxt",
		       g_stats.m_compressRobotsTxtDocs,
		       g_stats.m_compressRobotsTxtBytesIn,
		       g_stats.m_compressRobotsTxtBytesOut,
		       (float)g_stats.m_compressRobotsTxtBytesIn/
		       (float)g_stats.m_compressRobotsTxtBytesOut,

		       "UnknownType",
		       // "dates: pools overflowed" etc.
		       //"DateXmlSetError",
		       g_stats.m_compressUnknownTypeDocs,
		       g_stats.m_compressUnknownTypeBytesIn,
		       g_stats.m_compressUnknownTypeBytesOut,
		       (float)g_stats.m_compressUnknownTypeBytesIn/
		       (float)g_stats.m_compressUnknownTypeBytesOut );

	p.safePrintf ( "</table><br><br>\n" );

	}


	if ( r->getLong("reset",0) == 1 ) 
		g_stats.clearMsgStats();

	//
	// print msg re-routes
	//
	if ( format == FORMAT_HTML ) {
		p.safePrintf ( 
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=50>"
			      "<center><b>Message Stats</b> "

			      " &nbsp; [<a href=\"/admin/stats?reset=1\">"
			      "reset</a>]</td></tr>\n"

			      "<tr class=poo>"
			      "<td><b>niceness</td>\n"
			      "<td><b>msgtype</td>\n"

			      "<td><b>packets in</td>\n"
			      "<td><b>packets out</td>\n"

			      "<td><b>acks in</td>\n"
			      "<td><b>acks out</td>\n"

			      "<td><b>reroutes</td>\n"
			      "<td><b>dropped</td>\n"
			      "<td><b>cancels read</td>\n"
			      "<td><b>errors</td>\n"
			      "<td><b>timeouts</td>\n"
			      "<td><b>no mem</td>\n"
			      ,
			      TABLE_STYLE);
		p.safePrintf("</tr>\n");
	}

	// if ( format == FORMAT_XML )
	// 	p.safePrintf("\t<messageStats>\n");

	// if ( format == FORMAT_JSON )
	// 	p.safePrintf("\t\"messageStats\":{\n");

	// loop over niceness
	for ( int32_t i3 = 0 ; i3 < 2 ; i3++ ) {
		// print each msg stat
		for ( int32_t i1 = 0 ; i1 < MAX_MSG_TYPES ; i1++ ) {
			// skip it if has no handler
			if ( ! g_udpServer.hasHandler(i1) ) continue;
			if ( ! g_stats.m_reroutes   [i1][i3] &&
			     ! g_stats.m_packetsIn  [i1][i3] &&
			     ! g_stats.m_packetsOut [i1][i3] &&
			     ! g_stats.m_errors     [i1][i3] &&
			     ! g_stats.m_timeouts   [i1][i3] &&
			     ! g_stats.m_nomem      [i1][i3] &&
			     ! g_stats.m_dropped    [i1][i3] &&
			     ! g_stats.m_cancelRead [i1][i3]  )
				continue;
			// print it all out.
			if ( format == FORMAT_HTML )
				p.safePrintf( 
					     "<tr class=poo>"
					     "<td>%" PRId32"</td>"    // niceness, 0 or 1
					     "<td>0x%02x</td>" // msgType
					     //"<td>%" PRId32"</td>"    // request?
					     "<td>%" PRId32"</td>" // packets in
					     "<td>%" PRId32"</td>" // packets out
					     "<td>%" PRId32"</td>" // acks in
					     "<td>%" PRId32"</td>" // acks out
					     "<td>%" PRId32"</td>" // reroutes
					     "<td>%" PRId32"</td>" // dropped
					     "<td>%" PRId32"</td>" // cancel read
					     "<td>%" PRId32"</td>" // errors
					     "<td>%" PRId32"</td>" // timeouts
					     "<td>%" PRId32"</td>" // nomem
					     ,
					     i3, // niceness
					     (unsigned char)i1, // msgType
					     //i2, // request?
					     g_stats.m_packetsIn [i1][i3],
					     g_stats.m_packetsOut[i1][i3],
					     g_stats.m_acksIn [i1][i3],
					     g_stats.m_acksOut[i1][i3],
					     g_stats.m_reroutes[i1][i3],
					     g_stats.m_dropped[i1][i3],
					     g_stats.m_cancelRead[i1][i3],
					     g_stats.m_errors[i1][i3],
					     g_stats.m_timeouts[i1][i3],
					     g_stats.m_nomem[i1][i3]
					      );
			if ( format == FORMAT_XML )
				p.safePrintf(
					     "\t<messageStat>\n"
					     "\t\t<niceness>%" PRId32"</niceness>\n"
					     "\t\t<msgType>0x%02x</msgType>\n"
					     "\t\t<packetsIn>%" PRId32"</packetsIn>\n"
					     "\t\t<packetsOut>%" PRId32"</packetsOut>\n"
					     "\t\t<acksIn>%" PRId32"</acksIn>\n"
					     "\t\t<acksOut>%" PRId32"</acksOut>\n"
					     "\t\t<reroutes>%" PRId32"</reroutes>\n"
					     "\t\t<dropped>%" PRId32"</dropped>\n"
					     "\t\t<cancelsRead>%" PRId32"</cancelsRead>\n"
					     "\t\t<errors>%" PRId32"</errors>\n"
					     "\t\t<timeouts>%" PRId32"</timeouts>\n"
					     "\t\t<noMem>%" PRId32"</noMem>\n"
					     "\t</messageStat>\n"
					     ,i3, // niceness
					     (unsigned char)i1, // msgType
					     g_stats.m_packetsIn [i1][i3],
					     g_stats.m_packetsOut[i1][i3],
					     g_stats.m_acksIn [i1][i3],
					     g_stats.m_acksOut[i1][i3],
					     g_stats.m_reroutes[i1][i3],
					     g_stats.m_dropped[i1][i3],
					     g_stats.m_cancelRead[i1][i3],
					     g_stats.m_errors[i1][i3],
					     g_stats.m_timeouts[i1][i3],
					     g_stats.m_nomem[i1][i3]
					      );
			if ( format == FORMAT_JSON )
				p.safePrintf(
					     "\t\"messageStat\":{\n"
					     "\t\t\"niceness\":%" PRId32",\n"
					     "\t\t\"msgType\":\"0x%02x\",\n"
					     "\t\t\"packetsIn\":%" PRId32",\n"
					     "\t\t\"packetsOut\":%" PRId32",\n"
					     "\t\t\"acksIn\":%" PRId32",\n"
					     "\t\t\"acksOut\":%" PRId32",\n"
					     "\t\t\"reroutes\":%" PRId32",\n"
					     "\t\t\"dropped\":%" PRId32",\n"
					     "\t\t\"cancelsRead\":%" PRId32",\n"
					     "\t\t\"errors\":%" PRId32",\n"
					     "\t\t\"timeouts\":%" PRId32",\n"
					     "\t\t\"noMem\":%" PRId32"\n"
					     "\t},\n"
					     ,i3, // niceness
					     (unsigned char)i1, // msgType
					     g_stats.m_packetsIn [i1][i3],
					     g_stats.m_packetsOut[i1][i3],
					     g_stats.m_acksIn [i1][i3],
					     g_stats.m_acksOut[i1][i3],
					     g_stats.m_reroutes[i1][i3],
					     g_stats.m_dropped[i1][i3],
					     g_stats.m_cancelRead[i1][i3],
					     g_stats.m_errors[i1][i3],
					     g_stats.m_timeouts[i1][i3],
					     g_stats.m_nomem[i1][i3]
					      );
		}
	}


	//if ( format == FORMAT_XML )
	//	p.safePrintf("\t</messageStats>\n");

	if ( format == FORMAT_XML ) {
		p.safePrintf("</response>\n");
		return g_httpServer.sendDynamicPage(s,p.getBufStart(),
						    p.length(),
						    0,false,"text/xml");
	}

	if ( format == FORMAT_JSON ) {
		// remove last ,\n
		p.m_length -= 2;
		p.safePrintf("\n}\n}\n");
		return g_httpServer.sendDynamicPage(s,p.getBufStart(),
						    p.length(),
						   0,false,"application/json");
	}

	if ( format == FORMAT_HTML ) {
		p.safePrintf ( "</table><br><br>\n" );

		//
		// print msg send times
		//
		p.safePrintf ( 
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=50>"
			      "<center><b>Message Send Times</b></td></tr>\n"

			      "<tr class=poo>"
			      "<td><b>niceness</td>\n"
			      "<td><b>request?</td>\n"
			      "<td><b>msgtype</td>\n"
			      "<td><b>total sent</td>\n"
			      "<td><b>avg send time</td>\n" ,
			      TABLE_STYLE);

		// print bucket headers
		for ( int32_t i = 0 ; i < MAX_BUCKETS ; i++ ) 
			p.safePrintf("<td>%i+</td>\n",(1<<i)-1);
		p.safePrintf("</tr>\n");
	}

	// loop over niceness
	for ( int32_t i3 = 0 ; i3 < 2 ; i3++ ) {
		// loop over isRequest?
		for ( int32_t i2 = 0 ; i2 < 2 ; i2++ ) {
			// print each msg stat
			for ( int32_t i1 = 0 ; i1 < MAX_MSG_TYPES ; i1++ ) {
				// skip it if has no handler
				if ( ! g_udpServer.hasHandler(i1) ) continue;
				// skip if xml
				if ( format != FORMAT_HTML ) continue;
					// print it all out.
					// careful, second index is the nicenss, and third is
					// the isReply. our loops are reversed.
					int64_t total = g_stats.m_msgTotalOfSendTimes[i1][i3][i2];
					int64_t nt    = g_stats.m_msgTotalSent       [i1][i3][i2];
					// skip if no stat
					if ( nt == 0 ) continue;
					int32_t      avg   = 0;
					if ( nt > 0 ) avg = total / nt;
					p.safePrintf( 
						     "<tr class=poo>"
						      "<td>%" PRId32"</td>"    // niceness, 0 or 1
						      "<td>%" PRId32"</td>"    // request?
						      "<td>0x%02x</td>" // msgType
						      "<td>%" PRId64"</td>" // total sent
						      "<td>%" PRId32"ms</td>" ,// avg send time in ms
						      i3, // niceness
						      i2, // request?
						      (unsigned char)i1, // msgType
						      nt,
						      avg );
					// print buckets
					for ( int32_t i4 = 0 ; i4 < MAX_BUCKETS ; i4++ ) {
						int64_t count ;
						count = g_stats.m_msgTotalSentByTime[i1][i3][i2][i4];
						p.safePrintf("<td>%" PRId64"</td>",count);
					}
					p.safePrintf("</tr>\n");
			}
		}
	}

	if ( format == FORMAT_HTML ) {
		p.safePrintf ( "</table><br><br>\n" );

		//
		// print msg queued times
		//
		p.safePrintf ( 
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=50>"
			      "<center><b>Message Queued Times</b></td></tr>\n"

			      "<tr class=poo>"
			      //"<td>request?</td>\n"
			      "<td><b>niceness</td>\n"
			      "<td><b>msgtype</td>\n"
			      "<td><b>total queued</td>\n"
			      "<td><b>avg queued time</td>\n" ,
			      TABLE_STYLE);
		// print bucket headers
		for ( int32_t i = 0 ; i < MAX_BUCKETS ; i++ ) 
			p.safePrintf("<td>%i+</td>\n",(1<<i)-1);
		p.safePrintf("</tr>\n");
	}

	// loop over niceness
	for ( int32_t i3 = 0 ; i3 < 2 ; i3++ ) {
		// print each msg stat
		for ( int32_t i1 = 0 ; i1 < MAX_MSG_TYPES ; i1++ ) {
			// only html
			if ( format != FORMAT_HTML ) break;
			// skip it if has no handler
			if ( ! g_udpServer.hasHandler(i1) ) continue;
			// print it all out
			int64_t total = g_stats.m_msgTotalOfQueuedTimes[i1][i3];
			int64_t nt    = g_stats.m_msgTotalQueued       [i1][i3];
			// skip if no stat
			if ( nt == 0 ) continue;
			int32_t      avg   = 0;
			if ( nt > 0 ) avg = total / nt;
			p.safePrintf( 
				     "<tr class=poo>"
				      "<td>%" PRId32"</td>"    // niceness, 0 or 1
				     "<td>0x%02x</td>" // msgType
				      //"<td>%" PRId32"</td>"    // request?
				      "<td>%" PRId64"</td>" // total done
				      "<td>%" PRId32"ms</td>" ,// avg handler time in ms
				      i3, // niceness
				      (unsigned char)i1, // msgType
				      //i2, // request?
				      nt ,
				      avg );
			// print buckets
			for ( int32_t i4 = 0 ; i4 < MAX_BUCKETS ; i4++ ) {
				int64_t count ;
				count = g_stats.m_msgTotalQueuedByTime[i1][i3][i4];
				p.safePrintf("<td>%" PRId64"</td>",count);
			}
			p.safePrintf("</tr>\n");
		}
	}

	if ( format == FORMAT_HTML ) {
		p.safePrintf ( "</table><br><br>\n" );

		//
		// print msg handler times
		//
		p.safePrintf ( 
			      "<table %s>"
			      "<tr class=hdrow>"
			      "<td colspan=50>"
			      "<center><b>Message Reply Generation Times</b>"
			      "</td></tr>\n"

			      "<tr class=poo>"
			      //"<td>request?</td>\n"
			      "<td><b>niceness</td>\n"
			      "<td><b>msgtype</td>\n"
			      "<td><b>total replies</td>\n"
			      "<td><b>avg gen time</td>\n" ,
			      TABLE_STYLE);
		// print bucket headers
		for ( int32_t i = 0 ; i < MAX_BUCKETS ; i++ ) 
			p.safePrintf("<td>%i+</td>\n",(1<<i)-1);
		p.safePrintf("</tr>\n");
	}

	// loop over niceness
	for ( int32_t i3 = 0 ; i3 < 2 ; i3++ ) {
		// print each msg stat
		for ( int32_t i1 = 0 ; i1 < MAX_MSG_TYPES ; i1++ ) {
			// only hyml
			if ( format != FORMAT_HTML ) {
				break;
			}

			// skip it if has no handler
			if ( ! g_udpServer.hasHandler(i1) ) {
				continue;
			}

			// print it all out
			int64_t total = g_stats.m_msgTotalOfHandlerTimes[i1][i3];
			int64_t nt    = g_stats.m_msgTotalHandlersCalled[i1][i3];
			// skip if no stat
			if ( nt == 0 ) continue;
			int32_t      avg   = 0;
			if ( nt > 0 ) avg = total / nt;
			p.safePrintf(
				     "<tr class=poo>"
				      "<td>%" PRId32"</td>"    // niceness, 0 or 1
				     "<td>0x%02x</td>" // msgType
				      //"<td>%" PRId32"</td>"    // request?
				      "<td>%" PRId64"</td>" // total called
				      "<td>%" PRId32"ms</td>" ,// avg handler time in ms
				      i3, // niceness
				      (unsigned char)i1, // msgType
				      //i2, // request?
				      nt ,
				      avg );
			// print buckets
			for ( int32_t i4 = 0 ; i4 < MAX_BUCKETS ; i4++ ) {
				int64_t count ;
				count = g_stats.m_msgTotalHandlersByTime[i1][i3][i4];
				p.safePrintf("<td>%" PRId64"</td>",count);
			}
			p.safePrintf("</tr>\n");
		}
	}


	if ( format == FORMAT_HTML )
		p.safePrintf ( "</table><br><br>\n" );

	// print out whos is using the most mem
// 	ss = p.getBuf();
// 	ssend = p.getBufEnd();
	g_mem.printMemBreakdownTable(&p);
	//p.incrementLength(sss - ss);

	p.safePrintf ( "<br><br>\n" );


	// print db table
	// columns are the dbs
	p.safePrintf (
		  "<table %s>"
		  "<tr class=hdrow>"
		  "<td colspan=50>"
		  "<center><b>Databases"
		  "</b></td>"
		  "</tr>\n" ,
		  TABLE_STYLE );

	// make the rdbs
	const Rdb *rdbs[] = {
		g_posdb.getRdb(),
		g_titledb.getRdb(),
		g_doledb.getRdb() ,
		g_tagdb.getRdb(),
		g_clusterdb.getRdb(),
		g_linkdb.getRdb(),
	};
	int32_t nr = sizeof(rdbs) / sizeof(Rdb *);
	//TODO: sqlite: show statistics for sqlite database(s)

	// print dbname
	p.safePrintf("<tr class=poo><td>&nbsp;</td>");
	for ( int32_t i = 0 ; i < nr ; i++ ) 
		p.safePrintf("<td><b>%s</b></td>",rdbs[i]->getDbname());
	p.safePrintf("<td><i><b>Total</b></i></tr>\n");

	//int64_t total ;
	//float totalf ;

	// print # big files
	p.safePrintf("<tr class=poo><td><b># big files</b>*</td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumFiles();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);


	// print # small files
	p.safePrintf("<tr class=poo><td><b># small files</b>*</td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumSmallFiles();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);


	// print disk space used
	p.safePrintf("<tr class=poo><td><b>disk space (MB)</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getDiskSpaceUsed()/1000000;
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);


	// print # recs total
	p.safePrintf("<tr class=poo><td><b># recs</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumTotalRecs();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);


	// print # recs in mem
	p.safePrintf("<tr class=poo><td><b># recs in mem</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumUsedNodes();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);

	// print # negative recs in mem
	p.safePrintf("<tr class=poo><td><b># negative in mem</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumNegativeKeys();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);

	// print mem occupied
	p.safePrintf("<tr class=poo><td><b>mem occupied</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getTreeMemOccupied();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);

	// print mem allocated
	p.safePrintf("<tr class=poo><td><b>mem allocated</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getTreeMemAllocated();
		total += val;
		printNumAbbr ( p , val );
		//p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);

	// print mem max
	p.safePrintf("<tr class=poo><td><b>mem max</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getMaxTreeMem();
		total += val;
		printNumAbbr ( p , val );
		//p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);

	// print rdb mem used
	p.safePrintf("<tr class=poo><td><b>rdb mem used</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getUsedMem();
		total += val;
		//p.safePrintf("<td>%" PRIu64"</td>",val);
		printNumAbbr ( p , val );
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);

	// print rdb mem avail
	p.safePrintf("<tr class=poo><td><b><nobr>rdb mem available</nobr></b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getAvailMem();
		total += val;
		//p.safePrintf("<td>%" PRIu64"</td>",val);
		printNumAbbr ( p , val );
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);


	// print map mem
	p.safePrintf("<tr class=poo><td><b>map mem</b></td>");
	total = 0LL;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getMapMemAllocated();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRId64"</td></tr>\n",total);

	/*
	// print rec cache hits %
	p.safePrintf("<tr class=poo><td><b>rec cache hits %%</b></td>");
	totalf = 0.0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t hits   = rdbs[i]->m_cache.getNumHits();
		int64_t misses = rdbs[i]->m_cache.getNumHits();
		int64_t sum    = hits + misses;
		float val = 0.0;
		if ( sum > 0.0 ) val = ((float)hits * 100.0) / (float)sum;
		totalf += val;
		p.safePrintf("<td>%.1f</td>",val);
	}
	p.safePrintf("<td>%.1f</td></tr>\n",totalf);


	// print rec cache hits
	p.safePrintf("<tr class=poo><td><b>rec cache hits</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->m_cache.getNumHits();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	// print rec cache misses
	p.safePrintf("<tr class=poo><td><b>rec cache misses</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->m_cache.getNumMisses();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);

	// print rec cache tries
	p.safePrintf("<tr class=poo><td><b>rec cache tries</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t hits   = rdbs[i]->m_cache.getNumHits();
		int64_t misses = rdbs[i]->m_cache.getNumHits();
		int64_t val    = hits + misses;
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);

	p.safePrintf("<tr class=poo><td><b>rec cache used slots</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->m_cache.getNumUsedNodes();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>rec cache max slots</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->m_cache.getNumTotalNodes();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>rec cache used bytes</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->m_cache.getMemOccupied();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>rec cache max bytes</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->m_cache.getMaxMem();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);
	*/


	p.safePrintf("<tr class=poo><td><b>file cache hits %%</b></td>");
	//totalf = 0.0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		int64_t hits   = rpc->getNumHits();
		int64_t misses = rpc->getNumMisses();
		int64_t sum    = hits + misses;
		float val = 0.0;
		if ( sum > 0.0 ) val = ((float)hits * 100.0) / (float)sum;
		//totalf += val;
		p.safePrintf("<td>%.1f%%</td>",val);
	}
	p.safePrintf("<td>--</td></tr>\n");





	p.safePrintf("<tr class=poo><td><b>file cache hits</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		int64_t val = rpc->getNumHits();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>file cache misses</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		int64_t val = rpc->getNumMisses();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>file cache tries</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		int64_t hits   = rpc->getNumHits();
		int64_t misses = rpc->getNumMisses();
		int64_t val    = hits + misses;
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>file cache adds</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		p.safePrintf("<td>%" PRIu64"</td>",rpc->getNumAdds());
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>file cache drops</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		p.safePrintf("<td>%" PRIu64"</td>",rpc->getNumDeletes());
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b>file cache used</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		int64_t val = rpc->getMemOccupied();
		total += val;
		printNumAbbr ( p , val );
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b><nobr>file cache allocated</nobr></b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		const Rdb *rdb = rdbs[i];
		const RdbCache *rpc = getDiskPageCache ( rdb->getRdbId() );
		if ( ! rpc ) {
			p.safePrintf("<td>--</td>");
			continue;
		}
		int64_t val = rpc->getMemAllocated();
		total += val;
		printNumAbbr ( p , val );
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);




	p.safePrintf("<tr class=poo><td><b># disk seeks</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumSeeks();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># disk re-seeks</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumReSeeks();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># bytes read</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumRead();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># get requests read</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumRequestsGet();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># get requests bytes</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNetReadGet();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># get replies sent</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumRepliesGet();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># get reply bytes</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNetSentGet();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);



	p.safePrintf("<tr class=poo><td><b># add requests read</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumRequestsAdd();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># add requests bytes</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNetReadAdd();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);

	p.safePrintf("<tr class=poo><td><b># add replies sent</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNumRepliesAdd();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);


	p.safePrintf("<tr class=poo><td><b># add reply bytes</b></td>");
	total = 0;
	for ( int32_t i = 0 ; i < nr ; i++ ) {
		int64_t val = rdbs[i]->getNetSentAdd();
		total += val;
		p.safePrintf("<td>%" PRIu64"</td>",val);
	}
	p.safePrintf("<td>%" PRIu64"</td></tr>\n",total);






	// end the table now
	p.safePrintf ( "</table>\n" );
	// end the table now

	p.safePrintf (
		  "</center>"
		  "<br><i>*: # of files from all collection sub dirs.</i>\n");
	p.safePrintf (
		  "</center>"
		  "<br><i>Note: # recs for spiderdb or titledb may be lower "
		  "than the actual count because of unassociated negative "
		  "recs</i>\n");
	p.safePrintf (
		  "<br><i>Note: # recs for titledb may be higher "
		  "than the actual count because of duplicate positives "
		  "recs</i>\n");
	p.safePrintf (
		  "<br><i>Note: requests/replies does not include packet "
		  "header and ACK overhead</i>\n");
	p.safePrintf (
		  "<br><i>Note: twins may differ in rec counts but still have "
		  "the same data because they dump at different times which "
		  "leads to different reactions. To see if truly equal, "
		  "do a 'gb ddump' then when that finishes, a, 'gb pmerge'"
		  "for posdb or a 'gb tmerge' for titledb.\n");

	// print the final tail
	//p += g_httpServer.printTail ( p , pend - p );

	// calculate buffer length
	//int32_t bufLen = p - buf;
	int32_t bufLen = p.length();

	if ( format == FORMAT_XML ) {
		p.safePrintf("</response>\n");
		return g_httpServer.sendDynamicPage(s,p.getBufStart(),bufLen,
						    0,false,"text/xml");
	}



	// . send this page
	// . encapsulates in html header and tail
	// . make a Mime
	//return g_httpServer.sendDynamicPage ( s , buf , bufLen );
	return g_httpServer.sendDynamicPage ( s, p.getBufStart(), bufLen );
}
