#include "gb-include.h"

#include "SafeBuf.h"
#include "Collectiondb.h"
//#include "CollectionRec.h"
#include "Msg22.h"
#include "Query.h"
#include "HttpServer.h"
#include "Highlight.h"
#include "Pages.h"
#include "Tagdb.h"
#include "XmlDoc.h"
#include "Process.h"
#include "ip.h"
#include "GbUtil.h"
#include "Conf.h"
#include "Mem.h"


// TODO: redirect to host that has the titleRec locally

static bool sendErrorReply ( void *state , int32_t err ) ;
static void processLoopWrapper ( void *state ) ;
static bool processLoop ( void *state ) ;

class State2 {
public:
	char m_format;
	int32_t       m_niceness;
	XmlDoc     m_xd;
	lang_t       m_langId;
	TcpSocket *m_socket;
	HttpRequest m_r;
	char m_coll[MAX_COLL_LEN+2];
	bool       m_isMasterAdmin;
	SafeBuf m_qsb;
	char m_qtmpBuf[128];
	int32_t       m_qlen;
	bool       m_printed;
	int64_t  m_docId;
	bool       m_includeHeader;
	bool       m_includeBaseHref;
	bool       m_queryHighlighting;
	int32_t       m_strip;
	bool	   m_cnsPage;      // Are we in the click 'n' scroll page?
	bool	   m_printDisclaimer;
	bool       m_isBanned;
	bool       m_noArchive;
	SafeBuf    m_sb;
};
	

// . returns false if blocked, true otherwise
// . sets g_errno on error
bool sendPageGet ( TcpSocket *s , HttpRequest *r ) {
	// get the collection
	int32_t  collLen = 0;
	const char *coll    = r->getString("c",&collLen);
	if ( ! coll || ! coll[0] ) {
		coll = g_conf.getDefaultColl( );
		collLen = strlen(coll);
	}
	// ensure collection not too big
	if ( collLen >= MAX_COLL_LEN ) { 
		g_errno = ECOLLTOOBIG; 
		return g_httpServer.sendErrorReply(s,500,mstrerror(g_errno)); 
	}
	// get the collection rec
	CollectionRec *cr = g_collectiondb.getRec ( coll );
	if ( ! cr ) {
		g_errno = ENOCOLLREC;
		log("query: Archived copy retrieval failed. "
		    "No collection record found for "
		    "collection \"%s\".",coll);
		return g_httpServer.sendErrorReply(s,500,mstrerror(g_errno));
	}

	// . get fields from cgi field of the requested url
	// . get the search query
	int32_t  qlen = 0;
	const char *q = r->getString ( "q" , &qlen , NULL /*default*/);
	// ensure query not too big
	if ( qlen >= ABS_MAX_QUERY_LEN-1 ) { 
		g_errno=EQUERYTOOBIG; 
		return g_httpServer.sendErrorReply (s,500 ,mstrerror(g_errno));
	}
	// the docId
	int64_t docId = r->getLongLong ( "d" , 0LL /*default*/ );
	// get url
	const char *url = r->getString ( "u",NULL);

	if ( docId == 0 && ! url ) {
		g_errno = EMISSINGINPUT;
		return g_httpServer.sendErrorReply (s,500 ,mstrerror(g_errno));
	}

	// . get the titleRec
	// . TODO: redirect client to a better http server to save bandwidth
	State2 *st ;
	try { st = new (State2); }
	catch(std::bad_alloc&) {
		g_errno = ENOMEM;
		log("PageGet: new(%i): %s", 
		    (int)sizeof(State2),mstrerror(g_errno));
		return g_httpServer.sendErrorReply(s,500,mstrerror(g_errno));}
	mnew ( st , sizeof(State2) , "PageGet1" );
	// save the socket and if Host: is local in the Http request Mime
	st->m_socket   = s;
	st->m_isMasterAdmin  = g_conf.isCollAdmin ( s , r );
	st->m_docId    = docId;
	st->m_printed  = false;
	// include header ... "this page cached by Gigablast on..."
	st->m_includeHeader     = r->getLong ("ih"    , 1) ? true : false;
	st->m_includeBaseHref   = r->getLong ("ibh"   , 0) ? true : false;
	st->m_queryHighlighting = r->getLong ("qh"    , 1) ? true : false;
	st->m_strip             = r->getLong ("strip" , 0);
	st->m_cnsPage           = r->getLong ("cnsp"  , 1) ? true : false;
	const char *langAbbr = r->getString("qlang",NULL);
	st->m_langId = langUnknown;
	if ( langAbbr ) {
		lang_t langId = getLangIdFromAbbr ( langAbbr );
		st->m_langId = langId;
	}
	strncpy ( st->m_coll , coll , MAX_COLL_LEN+1 );
	// store query for query highlighting
	st->m_qsb.setBuf ( st->m_qtmpBuf,128,0,false );
	st->m_qsb.setLabel ( "qsbpg" );

	// save the query
	if ( q && qlen > 0 )
		st->m_qsb.safeStrcpy ( q );
	else
		st->m_qsb.safeStrcpy ( "" );
	
	st->m_qlen = qlen;
	st->m_isBanned = false;
	st->m_noArchive = false;
	st->m_socket = s;
	st->m_format = r->getReplyFormat();
	// default to 0 niceness
	st->m_niceness = 0;
	st->m_r.copy ( r );

	st->m_printDisclaimer = true;
	if ( st->m_cnsPage ) {
		st->m_printDisclaimer = false;
	}
	if ( st->m_strip ) {
		st->m_printDisclaimer = false;
	}

	// . fetch the TitleRec
	// . a max cache age of 0 means not to read from the cache
	XmlDoc *xd = &st->m_xd;
	// url based?
	if ( url ) {
		SpiderRequest sreq;
		strcpy(sreq.m_url, url );
		sreq.setDataSize();
		// this returns false if "coll" is invalid
		if ( ! xd->set4 ( &sreq , NULL , (char*)coll , NULL , st->m_niceness ) )
			goto hadSetError;
	}
	// . when getTitleRec() is called it will load the old one
	//   since XmlDoc::m_setFromTitleRec will be true
	// . niceness is 0
	// . use st->m_coll since XmlDoc just points to it!
	// . this returns false if "coll" is invalid
	else if ( ! xd->set3 ( docId , st->m_coll , 0 ) ) {
	hadSetError:
		mdelete ( st , sizeof(State2) , "PageGet1" );
		delete ( st );
		g_errno = ENOMEM;
		log("PageGet: set3: %s", mstrerror(g_errno));
		return g_httpServer.sendErrorReply(s,500,mstrerror(g_errno));
	}
	// if it blocks while it loads title rec, it will re-call this routine
	xd->setCallback ( st , processLoopWrapper );
	// good to go!
	return processLoop ( st );
}

// returns true
bool sendErrorReply ( void *state , int32_t err ) {
	// ensure this is set
	if ( ! err ) { g_process.shutdownAbort(true); }
	// get it
	State2 *st = (State2 *)state;
	// get the tcp socket from the state
	TcpSocket *s = st->m_socket;

	// nuke state2
	mdelete ( st , sizeof(State2) , "PageGet1" );
	delete (st);

	return g_httpServer.sendErrorReply ( s, err, mstrerror(err) );
}


void processLoopWrapper ( void *state ) {
	processLoop ( state );
}



// returns false if blocked, true otherwise
bool processLoop ( void *state ) {
	// get it
	State2 *st = (State2 *)state;
	// get the tcp socket from the state
	TcpSocket *s = st->m_socket;
	// get it
	XmlDoc *xd = &st->m_xd;

	if ( ! xd->m_loaded ) {
		// callback
		xd->setCallback ( state , processLoop );
		// . and tell it to load from the old title rec
		// . this sets xd->m_oldTitleRec/m_oldTitleRecSize
		// . this sets xd->ptr_* and all other member vars from
		//   the old title rec if found in titledb.
		if ( ! xd->loadFromOldTitleRec ( ) ) return false;
	}

	if ( g_errno ) return sendErrorReply ( st , g_errno );
	// now force it to load old title rec
	//char **tr = xd->getTitleRec();
	SafeBuf *tr = xd->getTitleRecBuf();
	// blocked? return false if so. it will call processLoop() when it rets
	if ( tr == (void *)-1 ) return false;
	// we did not block. check for error? this will free "st" too.
	if ( ! tr ) return sendErrorReply ( st , g_errno );
	// if title rec was empty, that is a problem
	if ( xd->m_titleRecBuf.length() == 0 ) 
		return sendErrorReply ( st , ENOTFOUND);

	// set callback
	bool *na = xd->getIsNoArchive();
	// wait if blocked
	if ( na == (void *)-1 ) return false;
	// error?
	if ( ! na ) return sendErrorReply ( st , g_errno );
	// forbidden? allow turkeys through though...
	if ( ! st->m_isMasterAdmin && *na )
		return sendErrorReply ( st , ENOCACHE );

	SafeBuf *sb = &st->m_sb;


	// &page=4 will print rainbow sections
	if ( ! st->m_printed && st->m_r.getLong("page",0) ) {
		// do not repeat this call
		st->m_printed = true;
		// this will call us again since we called
		// xd->setCallback() above to us
		if ( ! xd->printDocForProCog ( sb , &st->m_r ) )
			return false;
	}

	const char *contentType = "text/html";
	char format = st->m_format;
	if ( format == FORMAT_XML ) contentType = "text/xml";
	if ( format == FORMAT_JSON ) contentType = "application/json";

	// if we printed a special page (like rainbow sections) then return now
	if ( st->m_printed ) {
		bool status = g_httpServer.sendDynamicPage (s,
							    sb->getBufStart(),
							    sb->length(),
							    -1,false,
							    contentType,
							    -1, NULL, "utf8" );
		// nuke state2
		mdelete ( st , sizeof(State2) , "PageGet1" );
		delete (st);
		return status;
	}

	// get the utf8 content
	char **utf8 = xd->getUtf8Content();
	// wait if blocked???
	if ( utf8 == (void *)-1 ) return false;
	// strange
	if ( xd->size_utf8Content<=0) {
		log("pageget: utf8 content <= 0");
		return sendErrorReply(st,EBADENGINEER );
	}
	// alloc error?
	if ( ! utf8 ) return sendErrorReply ( st , g_errno );

	// get this host
	Host *h = g_hostdb.getHost ( g_hostdb.m_myHostId );
	if ( ! h ) {
		log("pageget: hostid %" PRId32" is bad",g_hostdb.m_myHostId);
		return sendErrorReply(st,EBADENGINEER );
	}

	char *content    = xd->ptr_utf8Content;
	int32_t  contentLen = xd->size_utf8Content - 1;

	// for undoing the header
	int32_t startLen1 = sb->length();

	// we are always utfu
	if ( st->m_strip != 2 ) {
		sb->safePrintf( "<meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf8\">\n");

		// base href
		char *base = xd->ptr_firstUrl;
		if ( xd->ptr_redirUrl ) base = xd->ptr_redirUrl;
		sb->safePrintf ( "<BASE HREF=\"%s\">" , base );

		// default colors in case css files missing
		sb->safePrintf( "\n<style type=\"text/css\">\n"
			  "body{background-color:white;color:black;}\n"
			  "</style>\n");
	}

	if ( format == FORMAT_XML ) sb->reset();
	if ( format == FORMAT_JSON ) sb->reset();

	if ( xd->m_contentType == CT_JSON ) sb->reset();
	if ( xd->m_contentType == CT_XML  ) sb->reset();

	// for undoing the stuff below
	int32_t startLen2 = sb->length();//p;

	// query should be NULL terminated
	char *q    = st->m_qsb.getBufStart();
	int32_t  qlen = st->m_qsb.length(); // m_qlen;

	char styleTitle[128] =  "font-size:14px;font-weight:600;"
				"color:#000000;";
	char styleText[128]  =  "font-size:14px;font-weight:400;"
				"color:#000000;";
	char styleLink[128] =  "font-size:14px;font-weight:400;"
				"color:#0000ff;";
	char styleTell[128] =  "font-size:14px;font-weight:600;"
				"color:#cc0000;";

	// get the url of the title rec
	Url *f = xd->getFirstUrl();

	bool printDisclaimer = st->m_printDisclaimer;

	if ( xd->m_contentType == CT_JSON )
		printDisclaimer = false;

	if ( format == FORMAT_XML ) printDisclaimer = false;
	if ( format == FORMAT_JSON ) printDisclaimer = false;

	char tbuf[100];
	tbuf[0] = 0;
	time_t lastSpiderDate = xd->m_spideredTime;

	if ( printDisclaimer ||
	     format == FORMAT_XML ||
	     format == FORMAT_JSON ) {
		struct tm tm_buf;
		struct tm *timeStruct = gmtime_r(&lastSpiderDate,&tm_buf);
		strftime ( tbuf, 100,"%b %d, %Y UTC", timeStruct);
	}

	// We should always be displaying this disclaimer.
	// - May eventually want to display this at a different location
	//   on the page, or on the click 'n' scroll browser page itself
	//   when this page is not being viewed solo.
	if ( printDisclaimer ) {
		sb->safePrintf(
			  "<table border=\"1\" bgcolor=\"#ffffff"
			  "\" cellpadding=\"10\" "
			  "cellspacing=\"0\" width=\"100%%\" color=\"#ffffff\">"
			  "<tr"
			  "><td>"
			  "<span style=\"%s\">"
			  "This is Gigablast's cached page of </span>"
			  "<a href=\"%s\" style=\"%s\">%s</a>"
			  "" , styleTitle, f->getUrl(), styleLink,
			  f->getUrl() );

		// then the rest
		sb->safePrintf("<span style=\"%s\">. Gigablast is not responsible for the content of this page.</span>", styleTitle);

		sb->safePrintf ("<br/><span style=\"%s\">Cached: </span><span style=\"%s\">",styleTitle, styleText );

		sb->safeStrcpy(tbuf);

		// Moved over from PageResults.cpp
		sb->safePrintf( "</span> - <a href=\""
			      "/get?"
			      "q=%s&amp;c=%s&amp;"
			      "d=%" PRId64"&amp;strip=1\""
			      " style=\"%s\">"
			      "[stripped]</a>", 
			      q , st->m_coll ,
			      st->m_docId, styleLink ); 

		// a link to alexa
		if ( f->getUrlLen() > 5 ) {
			sb->safePrintf( " - <a href=\"http:"
					 "//web.archive.org/web/*/%s\""
					 " style=\"%s\">"
					 "[older copies]</a>" ,
					 f->getUrl(), styleLink );
		}

		if (st->m_noArchive){
			sb->safePrintf( " - <span style=\"%s\"><b>"
				     "[NOARCHIVE]</b></span>",
				     styleTell );
		}
		if (st->m_isBanned){
			sb->safePrintf(" - <span style=\"%s\"><b>"
				     "[BANNED]</b></span>",
				     styleTell );
		}

		// only print this if we got a query
		if ( qlen > 0 ) {
			sb->safePrintf("<br/><br/><span style=\"%s\"> "
				   "These search terms have been "
				   "highlighted:  ",
				   styleText );
		}
		
	}

	// . make the url that we're outputting for (like in PageResults.cpp)
	// . "thisUrl" is the baseUrl for click & scroll
	char thisUrl[MAX_URL_LEN];
	char *thisUrlEnd = thisUrl + MAX_URL_LEN;
	char *x = thisUrl;
	// . use the external ip of our gateway
	// . construct the NAT mapped port
	// . you should have used iptables to map port to the correct
	//   internal ip:port
	uint32_t  ip   = h->m_ip;
	uint16_t port = h->getInternalHttpPort();

	// . we no longer put the port in here
	// . but still need http:// since we use <base href=>
	char ipbuf[16];
	if (port == 80) sprintf(x,"http://%s/get?q=",iptoa(ip,ipbuf));
	else            sprintf(x,"http://%s:%hu/get?q=",iptoa(ip,ipbuf),port);
	x += strlen ( x );
	// the query url encoded
	int32_t elen = urlEncode ( x , thisUrlEnd - x , q , qlen );
	x += elen;

	sprintf ( x, "&d=%" PRId64,st->m_docId );
	x += strlen(x);		
	// set our query for highlighting
	Query qq;
	qq.set(q, st->m_langId, 1.0, 1.0, NULL, true, false, ABS_MAX_QUERY_TERMS); //todo:use wordvariation settings form original query, if any

	// print the query terms into our highlight buffer
	Highlight hi;
	// make words so we can set the scores to ignore fielded terms
	TokenizerResult qtr;
	plain_tokenizer_phase_1(q,qlen, &qtr);
	//TODO: should the query be phase-1 or phase-2 tokenized for highlighting?
	calculate_tokens_hashes(&qtr);

	// declare up here
	Matches m;

	// now set m.m_matches[] to those words in qw that match a query word
	// or phrase in qq.
	m.setQuery ( &qq );
	m.addMatches ( &qtr );
	int32_t hilen = 0;

	// and highlight the matches
	if ( printDisclaimer ) {
		hilen = hi.set ( sb, &qtr, &m );
		sb->safeStrcpy("</span></table></table>\n");
	}

	bool includeHeader = st->m_includeHeader;

	// do not show header for json object display
	if ( xd->m_contentType == CT_JSON )
		includeHeader = false;
	if ( xd->m_contentType == CT_XML )
		includeHeader = false;

	if ( format == FORMAT_XML ) includeHeader = false;
	if ( format == FORMAT_JSON ) includeHeader = false;

	// undo the header writes if we should
	if ( ! includeHeader ) {
		// including base href is off by default when not including
		// the header, so the caller must explicitly turn it back on
		if ( st->m_includeBaseHref ) sb->m_length=startLen2;//p=start2;
		else                         sb->m_length=startLen1;//p=start1;
	}

	if ( format == FORMAT_XML ) {
		sb->safePrintf("<response>\n");
		sb->safePrintf("<statusCode>0</statusCode>\n");
		sb->safePrintf("<statusMsg>Success</statusMsg>\n");
		sb->safePrintf("<url><![CDATA[");
		cdataEncode(sb, xd->m_firstUrl.getUrl());
		sb->safePrintf("]]></url>\n");
		sb->safePrintf("<docId>%" PRIu64"</docId>\n",xd->m_docId);
		sb->safePrintf("\t<cachedTimeUTC>%" PRId32"</cachedTimeUTC>\n",
			       (int32_t)lastSpiderDate);
		sb->safePrintf("\t<cachedTimeStr>%s</cachedTimeStr>\n",tbuf);
	}

	if ( format == FORMAT_JSON ) {
		sb->safePrintf("{\"response\":{\n");
		sb->safePrintf("\t\"statusCode\":0,\n");
		sb->safePrintf("\t\"statusMsg\":\"Success\",\n");
		sb->safePrintf("\t\"url\":\"");
		sb->jsonEncode(xd->m_firstUrl.getUrl());
		sb->safePrintf("\",\n");
		sb->safePrintf("\t\"docId\":%" PRIu64",\n",xd->m_docId);
		sb->safePrintf("\t\"cachedTimeUTC\":%" PRId32",\n",
			       (int32_t)lastSpiderDate);
		sb->safePrintf("\t\"cachedTimeStr\":\"%s\",\n",tbuf);
	}

	
	// identify start of <title> tag we wrote out
	char *sbstart = sb->getBufStart();
	char *sbend   = sb->getBufPtr();
	char *titleStart = NULL;
	char *titleEnd   = NULL;

	char ctype = (char)xd->m_contentType;

	// do not calc title or print it if doc is xml or json
	if ( ctype == CT_XML ) sbend = sbstart;
	if ( ctype == CT_JSON ) sbend = sbstart;

	for ( char *t = sbstart ; t < sbend ; t++ ) {
		// title tag?
		if ( t[0]!='<' ) continue;
		if ( to_lower_a(t[1])!='t' ||
		     to_lower_a(t[2])!='i' ||
		     to_lower_a(t[3])!='t' ||
		     to_lower_a(t[4])!='l' ||
		     to_lower_a(t[5])!='e' ) continue;
		// point to it
		char *x = t + 5;
		// max - to keep things fast
		char *max = x + 500;
		for ( ; *x && *x != '>' && x < max ; x++ );
		x++;
		// find end
		char *e = x;
		for ( ; *e && e < max ; e++ ) {
			if ( e[0]=='<' &&
			     to_lower_a(e[1])=='/' &&
			     to_lower_a(e[2])=='t' &&
			     to_lower_a(e[3])=='i' &&
			     to_lower_a(e[4])=='t' &&
			     to_lower_a(e[5])=='l' &&
			     to_lower_a(e[6])=='e' )
				break;
		}
		if ( e < max ) {
			titleStart = x;
			titleEnd   = e;
		}
		break;
	}

	// . print title at top!
	// . consider moving
	if ( titleStart ) {

		const char *ebuf = st->m_r.getString("eb");
		if ( ! ebuf ) ebuf = "";

		sb->safePrintf(
			       "<table border=1 "
			       "cellpadding=10 "
			       "cellspacing=0 "
			       "width=100%% "
			       "color=#ffffff>" );

		int32_t printLinks = st->m_r.getLong("links",0);

		if ( ! printDisclaimer && printLinks )
			sb->safePrintf(//p += sprintf ( p , 
				       // first put cached and live link
				       "<tr>"
				       "<td bgcolor=lightyellow>"
				       // print cached link
				       "&nbsp; "
				       "<b>"
				       "<a "
				       "style=\"font-size:18px;font-weight:600;"
				       "color:#000000;\" "
				       "href=\""
				       "/get?"
				       "c=%s&d=%" PRId64"&qh=0&cnsp=1&eb=%s\">"
				       "cached link</a>"
				       " &nbsp; "
				       "<a "
				       "style=\"font-size:18px;font-weight:600;"
				       "color:#000000;\" "
				       "href=%s>live link</a>"
				       "</b>"
				       "</td>"
				       "</tr>\n"
				       ,st->m_coll
				       ,st->m_docId 
				       ,ebuf
				       ,thisUrl // st->ptr_ubuf
				       );

		if ( printLinks ) {
			sb->safePrintf(//p += sprintf ( p ,
				       "<tr><td bgcolor=pink>"
				       "<span style=\"font-size:18px;"
				       "font-weight:600;"
				       "color:#000000;\">"
				       "&nbsp; "
				       "<b>PAGE TITLE:</b> "
				       );
			int32_t tlen = titleEnd - titleStart;
			sb->safeMemcpy ( titleStart , tlen );
			sb->safePrintf ( "</span></td></tr>" );
		}

		sb->safePrintf( "</table><br>\n" );

	}

	// is the content preformatted?
	bool pre = false;

	if ( format == FORMAT_XML || format == FORMAT_JSON ) {
		pre = false;
	} else {
		if ( ctype == CT_TEXT || ctype == CT_DOC || ctype == CT_PS ) {
			pre = true ;
		}
	}

	// if it is content-type text, add a <pre>
	if ( pre ) {
		sb->safePrintf("<pre>");
	}

	if ( st->m_strip == 1 ) {
		contentLen = stripHtml( content, contentLen, (int32_t)xd->m_version, st->m_strip );
	}

	// it returns -1 and sets g_errno on error, line OOM
	if ( contentLen == -1 ) {
		return sendErrorReply ( st , g_errno );
	}

	Xml xml;
	TokenizerResult doctr;

	// if no highlighting, skip it
	bool queryHighlighting = st->m_queryHighlighting;
	if (st->m_strip == 2 || xd->m_contentType == CT_JSON) {
		queryHighlighting = false;
	}

	SafeBuf tmp;
	SafeBuf *xb = sb;
	if ( format == FORMAT_XML || format == FORMAT_JSON ) {
		xb = &tmp;
	}
	

	if ( ! queryHighlighting ) {
		xb->safeMemcpy ( content , contentLen );
		xb->nullTerm();
	}
	else {
		// get the content as xhtml (should be NULL terminated)
		if ( ! xml.set ( content, contentLen, TITLEREC_CURRENT_VERSION, CT_HTML ) ) {
			return sendErrorReply ( st , g_errno );
		}

		xml_tokenizer_phase_1(&xml,&doctr);
		calculate_tokens_hashes(&doctr);

		Matches m;
		m.setQuery ( &qq );
		m.addMatches ( &doctr );

		hilen = hi.set ( xb, &doctr, &m);

		log(LOG_DEBUG, "query: Done highlighting cached page content");
	}


	if ( format == FORMAT_XML ) {
		sb->safePrintf("\t<content><![CDATA[");
		cdataEncode(sb, xb->getBufStart());
		sb->safePrintf("]]></content>\n");
		sb->safePrintf("</response>\n");
	}

	if ( format == FORMAT_JSON ) {
		sb->safePrintf("\t\"content\":\"\n");
		sb->jsonEncode ( xb->getBufStart() );
		sb->safePrintf("\"\n}\n}\n");
	}


	// if it is content-type text, add a </pre>
	if ( pre ) {
		sb->safeMemcpy ( "</pre>" , 6 );
	}

	// now encapsulate it in html head/tail and send it off
	// sendErr:
	contentType = "text/html";
	if ( st->m_strip == 2 ) contentType = "text/xml";

	if (xd->m_contentType == CT_JSON) {
		contentType = "application/json";
	} else if ( xd->m_contentType == CT_XML ) {
		contentType = "text/xml";
	}

	if ( format == FORMAT_XML ) {
		contentType = "text/xml";
	} else if ( format == FORMAT_JSON ) {
		contentType = "application/json";
	}

	// safebuf, sb, is a member of "st" so this should copy the buffer
	// when it constructs the http reply, and we gotta call delete(st)
	// AFTER this so sb is still valid.
	bool status = g_httpServer.sendDynamicPage (s, sb->getBufStart(), sb->length(), -1, false,
	                                            contentType, -1, NULL, "utf8" );

	// nuke state2
	mdelete ( st , sizeof(State2) , "PageGet1" );
	delete (st);

	// and convey the status
	return status;
}
