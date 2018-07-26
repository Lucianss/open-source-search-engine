#include "Msg5.h"
#include "HttpRequest.h"
#include "RdbList.h"
#include "SafeBuf.h"
#include "HttpServer.h"
#include "Collectiondb.h"
#include "Doledb.h"
#include "Spider.h"
#include "SpiderLoop.h"
#include "SpiderColl.h"
#include "SpiderCache.h"
#include "XmlDoc.h"
#include "Pages.h"
#include "PageInject.h"
#include "ScopedLock.h"
#include "Process.h"
#include "ip.h"
#include "Mem.h"
#include "Errno.h"

namespace {

	class State11 {
	public:
		int32_t          m_numRecs;
		Msg5          m_msg5;
		RdbList       m_list;
		TcpSocket    *m_socket;
		HttpRequest   m_r;
		collnum_t     m_collnum;
		const char   *m_coll;
		int32_t          m_count;
		key96_t         m_startKey;
		key96_t         m_endKey;
		int32_t          m_minRecSizes;
		bool          m_done;
		SafeBuf       m_safeBuf;
		int32_t          m_priority;
	};

} //namespace

static bool loadLoop ( class State11 *st ) ;

// . returns false if blocked, true otherwise
// . sets g_errno on error
// . make a web page displaying the urls we got in doledb
// . doledb is sorted by priority complement then spider time
// . do not show urls in doledb whose spider time has not yet been reached,
//   so only show the urls spiderable now
// . call g_httpServer.sendDynamicPage() to send it
bool sendPageSpiderdb ( TcpSocket *s , HttpRequest *r ) {
	// set up a msg5 and RdbLists to get the urls from spider queue
	State11 *st ;
	try { st = new (State11); }
	catch(std::bad_alloc&) {
		g_errno = ENOMEM;
		log("PageSpiderdb: new(%i): %s",
		    (int)sizeof(State11),mstrerror(g_errno));
		log(LOG_ERROR,"%s:%s:%d: call sendErrorReply.", __FILE__, __func__, __LINE__);
		return g_httpServer.sendErrorReply(s,500,mstrerror(g_errno));}
	mnew ( st , sizeof(State11) , "PageSpiderdb" );
	// get the priority/#ofRecs from the cgi vars
	st->m_numRecs  = r->getLong ("n", 20  );
	st->m_r.copy ( r );
	// get collection name
	const char *coll = st->m_r.getString ( "c" , NULL , NULL );
	// get the collection record to see if they have permission
	//CollectionRec *cr = g_collectiondb.getRec ( coll );

	// the socket read buffer will remain until the socket is destroyed
	// and "coll" points into that
	st->m_coll = coll;
	CollectionRec *cr = g_collectiondb.getRec(coll);
	if ( cr ) st->m_collnum = cr->m_collnum;
	else      st->m_collnum = -1;
	// set socket for replying in case we block
	st->m_socket = s;
	st->m_count = 0;
	st->m_priority = MAX_SPIDER_PRIORITIES - 1;
	// get startKeys/endKeys/minRecSizes
	st->m_startKey    = Doledb::makeFirstKey2 (st->m_priority);
	st->m_endKey      = Doledb::makeLastKey2  (st->m_priority);
	st->m_minRecSizes = 20000;
	st->m_done        = false;
	// returns false if blocked, true otherwise
	return loadLoop ( st ) ;
}

static void gotListWrapper3 ( void *state , RdbList *list , Msg5 *msg5 ) ;
static bool sendPage        ( State11 *st );
static bool printList       ( State11 *st );

static bool loadLoop ( State11 *st ) {
	for(;;) {
		// let's get the local list for THIS machine (use msg5)
		if(! st->m_msg5.getList(RDB_DOLEDB,
					st->m_collnum,
					&st->m_list,
					&st->m_startKey,
					&st->m_endKey,
					st->m_minRecSizes,
					true                , // include tree
					0                   , // start file #
					-1                  , // # files
					st                  , // callback state
					gotListWrapper3,
					0                   , // niceness
					true,                 // do err correction
					-1,                   // maxRetries
					false))                // isRealMerge
			return false;
		// print it. returns false on error
		if(!printList(st))
			st->m_done = true;
		// check if done
		if(st->m_done) {
			// send the page back
			sendPage(st);
			// bail
			return true;
		}
	}
}

static void gotListWrapper3 ( void *state , RdbList *list , Msg5 *msg5 ) {
	// cast it
	State11 *st = (State11 *)state;
	// print it. returns false on error
	if ( ! printList ( st ) ) st->m_done = true;
	// check if done
	if ( st->m_done ) {
		// send the page back
		sendPage ( st );
		// bail
		return;
	}
	// otherwise, load more
	loadLoop( (State11 *)state );
}


// . make a web page from results stored in msg40
// . send it on TcpSocket "s" when done
// . returns false if blocked, true otherwise
// . sets g_errno on error
static bool printList ( State11 *st ) {
	// useful
	time_t nowGlobal = getTime();

	// print the spider recs we got
	SafeBuf *sbTable = &st->m_safeBuf;
	// shorcuts
	RdbList *list = &st->m_list;
	// row count
	int32_t j = 0;

	char format = st->m_r.getReplyFormat();

	// put it in there
	for ( ; ! list->isExhausted() ; list->skipCurrentRecord() ) {
		// stop if we got enough
		if ( st->m_count >= st->m_numRecs )  break;
		// get the doledb key
		key96_t dk = list->getCurrentKey();
		// update to that
		st->m_startKey = dk;
		// inc by one
		st->m_startKey++;
		// get spider time from that
		int32_t spiderTime = Doledb::getSpiderTime ( &dk );
		// skip if in future
		if ( spiderTime > nowGlobal ) continue;
		// point to the spider request *RECORD*
		char *rec = list->getCurrentData();
		// skip negatives
		if ( (dk.n0 & 0x01) == 0 ) continue;
		// count it
		st->m_count++;
		// what is this?
		if ( list->getCurrentRecSize() <= 16 ) { g_process.shutdownAbort(true);}
		// sanity check. requests ONLY in doledb
		if ( ! Spiderdb::isSpiderRequest ( (key128_t *)rec )) {
			log("spider: not printing spiderreply");
			continue;
			//g_process.shutdownAbort(true);
		}
		// get the spider rec, encapsed in the data of the doledb rec
		SpiderRequest *sreq = (SpiderRequest *)rec;

		if (format == FORMAT_JSON) {
			if (!sreq->printToJSON(sbTable, "ready", NULL, j)) {
				return false;
			}
		} else {
			// print it into sbTable
			if (!sreq->printToTable(sbTable, "ready", NULL, j)) {
				return false;
			}
		}
		// count row
		j++;
	}
	// need to load more?
	if ( st->m_count >= st->m_numRecs ||
	     // if list was a partial, this priority is short then
	     list->getListSize() < st->m_minRecSizes ) {
		// . try next priority
		// . if below 0 we are done
		if ( --st->m_priority < 0 ) st->m_done = true;
		// get startKeys/endKeys/minRecSizes
		st->m_startKey    = Doledb::makeFirstKey2 (st->m_priority);
		st->m_endKey      = Doledb::makeLastKey2  (st->m_priority);
		// if we printed something, print a blank line after it
		if ( st->m_count > 0 && format == FORMAT_HTML) {
			sbTable->safePrintf("<tr><td colspan=30><center>&vellip;</center></td></tr>\n");
		}
		// reset for each priority
		st->m_count = 0;
	}


	return true;
}

static bool generatePageHTML(CollectionRec *cr, SafeBuf *sb, const SafeBuf *doledbbuf) {
	// print reason why spiders are not active for this collection
	spider_status_t tmp2;
	const char *crawlMsg;
	getSpiderStatusMsg ( cr , &crawlMsg, &tmp2 );
	if ( crawlMsg && tmp2 != spider_status_t::SP_INITIALIZING )
		sb->safePrintf("<table cellpadding=5 style=\"max-width:600px\" border=0>"
		               "<tr>"
		               "<td>"
		               "For collection <i>%s</i>: "
		               "<b><font color=red>%s</font></b>"
		               "</td>"
		               "</tr>"
		               "</table>\n"
		               , cr->m_coll
		               , crawlMsg);

	// begin the table
	sb->safePrintf("<table class=\"main\" width=100%%>\n"
		           "<tr class=\"level1\"><th colspan=50>"
		           "Currently Spidering on This Host <span class=\"comment\">(%" PRId32" spiders)</span>"
		           "</th></tr>\n"
		           , g_spiderLoop.getNumSpidersOut()
	);

	// the table headers so SpiderRequest::printToTable() works
	if (!SpiderRequest::printTableHeader(sb, true)) {
		return false;
	}

	// count # of spiders out
	int32_t j = 0;
	// first print the spider recs we are spidering
	for (int32_t i = 0; i < (int32_t)MAX_SPIDERS; i++) {
		// get it
		XmlDoc *xd = g_spiderLoop.m_docs[i];
		// skip if empty
		if (!xd) continue;
		// sanity check
		if (!xd->m_sreqValid) { g_process.shutdownAbort(true); }
		// grab it
		SpiderRequest *oldsr = &xd->m_sreq;
		// get status
		const char *status = xd->m_statusMsg;
		// show that
		if (!oldsr->printToTable(sb, status, xd, j)) return false;
		// inc count
		j++;
	}
	// now print the injections as well!
	XmlDoc *xd = getInjectHead();
	for (; xd; xd = xd->m_nextInject) {
		// how does this happen?
		if (!xd->m_sreqValid) continue;
		// grab it
		SpiderRequest *oldsr = &xd->m_sreq;
		// get status
		SafeBuf xb;
		xb.safePrintf("[<font color=red><b>injecting</b></font>] %s", xd->m_statusMsg);
		char *status = xb.getBufStart();
		// show that
		if (!oldsr->printToTable(sb, status, xd, j)) return false;
		// inc count
		j++;
	}

	// end the table
	sb->safePrintf("</table>\n");
	sb->safePrintf("<br>\n");

	// then spider collection
	SpiderColl *sc = g_spiderCache.getSpiderColl(cr->m_collnum);

	// done if no sc
	if ( ! sc ) {
		return true;
	}

	/////
	//
	// READY TO SPIDER table
	//
	/////

	int32_t ns = sc->getDoledbIpTableCount();

	// begin the table
	sb->safePrintf ( "<table class=\"main\" width=100%%>\n"
	                "<tr class=\"level1\"><th colspan=50>"
	                "URLs Ready to Spider for collection "
	                "<font color=red>%s</font>"
	                " <span class=\"comment\">(%" PRId32" ips in doleiptable)</span>",
	                cr->m_coll ,
	                ns );

	// print time format: 7/23/1971 10:45:32
	time_t nowUTC = getTimeGlobal();
	struct tm *timeStruct ;
	char time[256];
	struct tm tm_buf;
	timeStruct = gmtime_r(&nowUTC,&tm_buf);
	strftime ( time , 256 , "%b %e %T %Y UTC", timeStruct );
	sb->safePrintf("</th></tr>\n");

	// the table headers so SpiderRequest::printToTable() works
	if (!SpiderRequest::printTableHeader(sb, false)) return false;
	// the the doledb spider recs
	const char *bs = doledbbuf->getBufStart();
	if (bs && !sb->safePrintf("%s", bs)) return false;
	// end the table
	sb->safePrintf ( "</table>\n" );
	sb->safePrintf ( "<br>\n" );



	/////////////////
	//
	// PRINT WAITING TREE
	//
	// each row is an ip. print the next url to spider for that ip.
	//
	/////////////////
	sb->safePrintf ( "<table class=\"main\" width=100%%>\n"
	                "<tr class=\"level1\"><th colspan=50>"
	                "IPs Waiting for Selection Scan for collection "
	                "<font color=red>%s</font>",
	                cr->m_coll );
	sb->safePrintf("<span class=\"comment\">");
	// print time format: 7/23/1971 10:45:32
	int64_t timems = gettimeofdayInMilliseconds();
	sb->safePrintf(" (current time = %" PRIu64") (totalcount=%" PRId32") (waittablecount=%" PRId32")",
	              timems, sc->m_waitingTree.getNumUsedNodes(), sc->getWaitingTableCount());

	char ipbuf[16];
	sb->safePrintf(" (spiderdb scanning ip %s)", iptoa(sc->getScanningIp(),ipbuf));

	sb->safePrintf("</span>");
	sb->safePrintf("</th></tr>\n");
	sb->safePrintf("<tr class=\"level2\">");
	sb->safePrintf("  <th>spidertime (MS)</th>\n");
	sb->safePrintf("  <th>spidertime</th>\n");
	sb->safePrintf("  <th>firstip</th>\n");
	sb->safePrintf("</tr>\n");
	// the the waiting tree

	int32_t count = 0;
	int32_t zero_spidertime_count = 0;
	bool truncated_output = false;
	{
		ScopedLock sl(sc->m_waitingTree.getLock());
		for (int32_t node = sc->m_waitingTree.getFirstNode_unlocked(); node >= 0;
		     node = sc->m_waitingTree.getNextNode_unlocked(node)) {
			// get key
			const key96_t *key = reinterpret_cast<const key96_t *>(sc->m_waitingTree.getKey_unlocked(node));
			// get ip from that
			int32_t firstIp = (key->n0) & 0xffffffff;
			// get the timedocs
			uint64_t spiderTimeMS = key->n1;
			// shift upp
			spiderTimeMS <<= 32;
			// or in
			spiderTimeMS |= (key->n0 >> 32);

			time_t spiderTimeSeconds = spiderTimeMS/1000;
			struct tm tm_buf;
			struct tm *stm = gmtime_r(&spiderTimeSeconds,&tm_buf);
			char humanreadableSpiderTime[64];
			sprintf(humanreadableSpiderTime,"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", stm->tm_year+1900,stm->tm_mon+1,stm->tm_mday,stm->tm_hour,stm->tm_min,stm->tm_sec,(int)(spiderTimeMS%1000));
			
			sb->safePrintf("<tr>"
			               "<td>%" PRId64"</td>"
			               "<td>%s</td>"
			               "<td><tt>%s</tt></td>"
			               "</tr>\n",
			               (int64_t)spiderTimeMS,
				       humanreadableSpiderTime,
			               iptoa(firstIp,ipbuf));
			// stop after 40
			if (++count == 40) {
				truncated_output = true;
				break;
			}
		}
		for (int32_t node = sc->m_waitingTree.getFirstNode_unlocked(); node >= 0;
		     node = sc->m_waitingTree.getNextNode_unlocked(node)) {
			// get key
			const key96_t *key = reinterpret_cast<const key96_t *>(sc->m_waitingTree.getKey_unlocked(node));
			// get the timedocs
			uint64_t spiderTimeMS = key->n1;
			if(spiderTimeMS==0)
				zero_spidertime_count++;
		}
	}
	
	if(truncated_output)
		sb->safePrintf("<tr><td colspan=3><center>&vellip;</center></td></tr>\n");
	
	sb->safePrintf("<tr><td class=\"bg0\" colspan=50>%d of %d nodes have spidertime==0</td></tr>\n",
		       zero_spidertime_count, sc->m_waitingTree.getNumUsedNodes());
	
	// end the table
	sb->safePrintf ( "</table>\n" );
	sb->safePrintf ( "<br>\n" );


	//print spidercoll->m_nextKeys[]
	sb->safePrintf ( "<table class=\"main\" width=100%%>\n"
	                "<tr class=\"level1\"><th colspan=50>"
	                "SpiderColl->m_nextKeys[]"
	                "</th></tr>\n");
	sb->safePrintf("<tr class=\"level2\">");
	sb->safePrintf("  <th>Priority</th>\n");
	sb->safePrintf("  <th>Key</th>\n");
	sb->safePrintf("</tr>\n");

	for(int i=0; i<MAX_SPIDER_PRIORITIES; i++) {
		char keystrbuf[sizeof(sc->m_nextKeys[0])*2+1];
		sb->safePrintf("<tr><td>%d</td><td><tt>%s</tt></td></tr>\n",
			       i,
			       KEYSTR(&sc->m_nextKeys[i], sizeof(sc->m_nextKeys[i]), keystrbuf));
	}

	sb->safePrintf ( "</table>\n" );
	sb->safePrintf ( "<br>\n" );


	return true;
}

/*
 * {
 * "response": {
 *     "statusCode": 0,
 *     "statusMsg": "Job is initializing.",
 *     "currentSpiders": 0,
 *
 * }
 * }
 */
static bool generatePageJSON(CollectionRec *cr, SafeBuf *sb, const SafeBuf *doledbbuf) {
	sb->safePrintf("{\n\"response\": {\n");

	spider_status_t crawlStatus;
	const char *crawlMsg;
	getSpiderStatusMsg ( cr , &crawlMsg , &crawlStatus );

	sb->safePrintf("\t\"statusCode\": %d,\n", (int)crawlStatus);
	sb->safePrintf("\t\"statusMsg\": \"%s\",\n", crawlMsg);
	sb->safePrintf("\t\"spiderCount\": %d,\n", g_spiderLoop.getNumSpidersOut());

	sb->safePrintf("\t\"spiders\": [\n");

	// count # of spiders out
	int32_t j = 0;
	// first print the spider recs we are spidering
	for (int32_t i = 0; i < (int32_t)MAX_SPIDERS; i++) {
		XmlDoc *xd = g_spiderLoop.m_docs[i];
		if (!xd) {
			continue;
		}

		// sanity check
		if (!xd->m_sreqValid) {
			g_process.shutdownAbort(true);
		}

		// grab it
		SpiderRequest *oldsr = &xd->m_sreq;
		if (!oldsr->printToJSON(sb, xd->m_statusMsg, xd, j)) {
			return false;
		}

		j++;
	}
	// now print the injections as well!
	XmlDoc *xd = getInjectHead();
	for (; xd; xd = xd->m_nextInject) {
		// how does this happen?
		if (!xd->m_sreqValid) {
			continue;
		}

		SpiderRequest *oldsr = &xd->m_sreq;

		// get status
		SafeBuf xb;
		xb.safePrintf("injecting - %s", xd->m_statusMsg);

		// show that
		if (!oldsr->printToJSON(sb, xb.getBufStart(), xd, j)) {
			return false;
		}

		// inc count
		j++;
	}

	if (j > 0) {
		sb->removeLastChar('\n');
		sb->removeLastChar(',');
		sb->removeLastChar('\t');
		sb->removeLastChar('\t');
	}

	// end the table
	sb->safePrintf("\t]\n");

	// then spider collection
	SpiderColl *sc = g_spiderCache.getSpiderColl(cr->m_collnum);

	// done if no sc
	if (!sc) {
		sb->safePrintf("}\n}\n");
		return true;
	}

	sb->safePrintf("\t,\n");

	/////
	//
	// READY TO SPIDER table
	//
	/////

	sb->safePrintf("\t\"doleIPCount\": %d,\n", sc->getDoledbIpTableCount());

	sb->safePrintf("\t\"doleIPs\": [\n");

	// the the doledb spider recs
	const char *bs = doledbbuf->getBufStart();
	if (bs && !sb->safePrintf("%s", bs)) {
		return false;
	}

	if (doledbbuf->length() > 0) {
		sb->removeLastChar('\n');
		sb->removeLastChar(',');
		sb->removeLastChar('\t');
		sb->removeLastChar('\t');
	}

	sb->safePrintf("\t],\n");

	/////////////////
	//
	// PRINT WAITING TREE
	//
	// each row is an ip. print the next url to spider for that ip.
	//
	/////////////////

	sb->safePrintf("\t\"waitingTreeCount\": %d,\n", sc->m_waitingTree.getNumUsedNodes());

	sb->safePrintf("\t\"waitingTrees\": [\n");

	// the the waiting tree
	char ipbuf[16];
	int32_t count = 0;
	{
		ScopedLock sl(sc->m_waitingTree.getLock());
		for (int32_t node = sc->m_waitingTree.getFirstNode_unlocked(); node >= 0; node = sc->m_waitingTree.getNextNode_unlocked(node)) {
			// get key
			const key96_t *key = reinterpret_cast<const key96_t *>(sc->m_waitingTree.getKey_unlocked(node));

			// get ip from that
			int32_t firstIp = (key->n0) & 0xffffffff;

			// get the timedocs
			uint64_t spiderTimeMS = key->n1;
			// shift upp
			spiderTimeMS <<= 32;
			// or in
			spiderTimeMS |= (key->n0 >> 32);

			if (count != 0) {
				sb->safePrintf("\t\t,\n");
			}

			sb->safePrintf("\t\t{\n");

			sb->safePrintf("\t\t\t\"spiderTime\": %" PRIu64",\n", spiderTimeMS);
			sb->safePrintf("\t\t\t\"firstIp\": \"%s\"\n", iptoa(firstIp,ipbuf));

			sb->safePrintf("\t\t}\n");

			// stop after 20
			if (++count == 20) break;
		}
	}
	sb->safePrintf("\t]\n");

	sb->safePrintf("}\n}\n");

	return true;
}

static bool sendPage(State11 *st) {
	// generate a query string to pass to host bar
	char qs[64]; sprintf ( qs , "&n=%" PRId32, st->m_numRecs );

	// store the page in here!
	SafeBuf sb;
	if( !sb.reserve ( 64*1024 ) ) {
		logError("Could not reserve needed mem, bailing!");
		return false;
	}

	char format = st->m_r.getReplyFormat();

	if (format == FORMAT_HTML) {
		g_pages.printAdminTop(&sb, st->m_socket, &st->m_r, qs);
	}

	// get spider coll
	collnum_t collnum = g_collectiondb.getCollnum ( st->m_coll );
	// and coll rec
	CollectionRec *cr = g_collectiondb.getRec ( collnum );
	if ( ! cr ) {
		// get the socket
		TcpSocket *s = st->m_socket;
		// then we can nuke the state
		mdelete ( st , sizeof(State11) , "PageSpiderdb" );
		delete (st);
		// erase g_errno for sending
		g_errno = 0;
		// now encapsulate it in html head/tail and send it off
		return g_httpServer.sendDynamicPage(s, sb.getBufStart(), sb.length());
	}

	bool result;
	const char *contentType;
	switch (format) {
		case FORMAT_JSON:
			result = generatePageJSON(cr, &sb, &st->m_safeBuf);
			contentType = "application/json";
			break;
		case FORMAT_HTML:
		default:
			result = generatePageHTML(cr, &sb, &st->m_safeBuf);
			contentType = NULL;
			break;
	}

	if (result) {
		// get the socket
		TcpSocket *s = st->m_socket;
		// then we can nuke the state
		mdelete ( st , sizeof(State11) , "PageSpiderdb" );
		delete (st);
		// erase g_errno for sending
		g_errno = 0;
		// now encapsulate it in html head/tail and send it off
		return g_httpServer.sendDynamicPage(s, sb.getBufStart(), sb.length(), -1, false, contentType);
	}

	return false;
}
