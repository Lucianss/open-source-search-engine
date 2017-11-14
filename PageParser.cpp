#include "PageParser.h"
#include "XmlDoc.h"
#include "Pages.h"
#include "HttpServer.h"
#include "HttpRequest.h"
#include "Process.h"
#include "Conf.h"
#include "Mem.h"


class State8 {
public:
	//Msg16 m_msg16;
	//Msg14 m_msg14;
	//Msg15 m_msg15;
	SafeBuf m_dbuf;
	//XmlDoc m_doc;
	//Url   m_url;
	//Url   m_rootUrl;
	const char *m_u;
	int32_t  m_ulen;
	char  m_rootQuality;
	char  m_coll[MAX_COLL_LEN];
	int32_t  m_collLen;
	//int32_t  m_sfn;
	//int32_t  m_urlLen;
	TcpSocket  *m_s;
	char  m_pwd[32];
	HttpRequest m_r;
	int32_t m_old;
	// recyle the link info from the title rec?
	int32_t m_recycle;
	// recycle the link info that was imported from another coll?
	int32_t m_recycle2;
	bool m_render;
	bool m_recompute;
	int32_t m_oips;
	char m_linkInfoColl[11];
	//	char m_buf[16384 * 1024];

	//int32_t m_page;
	// m_pbuf now points to m_sbuf if we are showing the parsing junk
	SafeBuf  m_xbuf;
	SafeBuf  m_wbuf;
	bool m_donePrinting;
	//SafeBuf  m_sbuf;
	// this is a buffer which cats m_sbuf into it
	//SafeBuf  m_sbuf2;

	// new state vars for Msg3b.cpp
	int64_t  m_docId;
	void      *m_state ;
	void    (* m_callback) (void *state);
	Query     *m_q;
	int64_t *m_termFreqs;
	float     *m_termFreqWeights;
	float     *m_affWeights;
	//score_t    m_total;
	bool       m_freeIt;
	bool       m_blocked;

	// these are from rearranging the code
	int32_t      m_indexCode;
	//uint64_t m_chksum1;

	bool m_didRootDom;
	bool m_didRootWWW;
	bool m_wasRootDom;

	// call Msg16 with a versino of title rec to do
	int32_t m_titleRecVersion;
	
	char m_hopCount;

	//TitleRec m_tr;

	//XmlDoc m_oldDoc;
	XmlDoc m_xd;
};

// TODO: meta redirect tag to host if hostId not ours
static bool processLoop ( void *state ) ;
static bool gotXmlDoc ( void *state ) ;
static bool sendErrorReply ( void *state , int32_t err ) ;
static bool sendPageParser2 ( TcpSocket    *s , 
                              HttpRequest  *r ,
                              class State8 *st ,
                              int64_t     docId ,
                              Query        *q ,
                              int64_t    *termFreqs       ,
                              float        *termFreqWeights ,
                              float        *affWeights ,
                              void         *state ,
                              void        (* callback)(void *state) ) ;


// . returns false if blocked, true otherwise
// . sets g_errno on error
// . make a web page displaying the config of this host
// . call g_httpServer.sendDynamicPage() to send it
// . TODO: don't close this socket until httpserver returns!!
bool sendPageParser ( TcpSocket *s , HttpRequest *r ) {
	return sendPageParser2 ( s , r , NULL , -1LL , NULL , NULL, 
				 NULL , NULL, NULL , NULL );
}

// . a new interface so Msg3b can call this with "s" set to NULL
// . returns false if blocked, true otherwise
// . sets g_errno on error
static bool sendPageParser2 ( TcpSocket   *s , 
                              HttpRequest *r ,
                              State8      *st ,
                              int64_t    docId ,
                              Query       *q ,
                              // in query term space, not imap space
                              int64_t   *termFreqs       ,
                              // in imap space
                              float       *termFreqWeights ,
                              // in imap space
                              float       *affWeights      ,
                              void        *state ,
                              void       (* callback)(void *state) ) {

	//log("parser: read sock=%" PRId32,s->m_sd);

	// might a simple request to addsomething to validated.*.txt file
	// from XmlDoc::print() or XmlDoc::validateOutput()
	//int64_t uh64 = r->getLongLong("uh64",0LL);
	const char *uh64str = r->getString("uh64",NULL);
	//char *divTag = r->getString("div",NULL);
	if ( uh64str ) {
		// make basic reply
		const char *reply = "HTTP/1.0 200 OK\r\n"
			"Connection: Close\r\n";
		// that is it! send a basic reply ok
		bool status = g_httpServer.sendDynamicPage( s , 
							    reply,
							    strlen(reply),
							    -1, //cachtime
							    false ,//postreply?
							    NULL, //ctype
							    -1 , //httpstatus
							    NULL,//cookie
							    "utf-8");
		return status;
	}

	// make a state
	if (   st ) st->m_freeIt = false;
	if ( ! st ) {
		try { st = new (State8); }
		catch(std::bad_alloc&) {
			g_errno = ENOMEM;
			log("PageParser: new(%i): %s", 
			    (int)sizeof(State8),mstrerror(g_errno));
			return g_httpServer.sendErrorReply(s,500,
						       mstrerror(g_errno));}
		mnew ( st , sizeof(State8) , "PageParser" );
		st->m_freeIt = true;
	}
	// msg3b uses this to get a score from the query
	st->m_state           = state;
	st->m_callback        = callback;
	st->m_q               = q;
	st->m_termFreqs       = termFreqs;
	st->m_termFreqWeights = termFreqWeights;
	st->m_affWeights      = affWeights;
	//st->m_total           = (score_t)-1;
	st->m_indexCode       = 0;
	st->m_blocked         = false;
	st->m_didRootDom      = false;
	st->m_didRootWWW      = false;
	st->m_wasRootDom      = false;
	st->m_u               = NULL;
	st->m_recompute       = false;
	//st->m_url.reset();

	// password, too
	int32_t pwdLen = 0;
	const char *pwd = r->getString ( "pwd" , &pwdLen );
	if ( pwdLen > 31 ) pwdLen = 31;
	if ( pwdLen > 0 ) strncpy ( st->m_pwd , pwd , pwdLen );
	st->m_pwd[pwdLen]='\0';

	// save socket ptr
	st->m_s = s;
	st->m_r.copy ( r );
	// get the collection
	const char *coll    = r->getString ( "c" , &st->m_collLen ,NULL /*default*/);
	if ( st->m_collLen > MAX_COLL_LEN )
		return sendErrorReply ( st , ENOBUFS );
	if ( ! coll )
		return sendErrorReply ( st , ENOCOLLREC );
	strcpy ( st->m_coll , coll );

	// version to use, if -1 use latest
	st->m_titleRecVersion = r->getLong("version",-1);
	if ( st->m_titleRecVersion == -1 ) 
		st->m_titleRecVersion = TITLEREC_CURRENT_VERSION;
	// default to 0 if not provided
	st->m_hopCount = r->getLong("hc",0);
	//int32_t  ulen    = 0;
	//char *u     = r->getString ( "u" , &ulen     , NULL /*default*/);
	int32_t  old     = r->getLong   ( "old", 0 );

	// url will override docid if given
	if ( ! st->m_u || ! st->m_u[0] ) 
		st->m_docId = r->getLongLong ("docid",-1);
	else    
		st->m_docId = -1;
	// set url in state class (may have length 0)
	//if ( u ) st->m_url.set ( u , ulen );
	//st->m_urlLen = ulen;
	st->m_u = st->m_r.getString("u",&st->m_ulen,NULL);
	// should we recycle link info?
	st->m_recycle  = r->getLong("recycle",0);
	st->m_recycle2 = r->getLong("recycleimp",0);
	st->m_render   = r->getLong("render" ,0) ? true : false;
	// for quality computation... takes way longer cuz we have to 
	// lookup the IP address of every outlink, so we can get its root
	// quality using Msg25 which needs to filter out voters from that IP
	// range.
	st->m_oips     = r->getLong("oips"    ,0);

	int32_t  linkInfoLen  = 0;
	// default is NULL
	const char *linkInfoColl = r->getString ( "oli" , &linkInfoLen, NULL );
	if ( linkInfoColl ) strcpy ( st->m_linkInfoColl , linkInfoColl );
	else st->m_linkInfoColl[0] = '\0';
	
	// should we use the old title rec?
	st->m_old    = old;
 	//no more setting the default root quality to 30, instead if we do not
 	// know it setting it to -1
 	st->m_rootQuality=-1;







	// header
	SafeBuf *xbuf = &st->m_xbuf;
	xbuf->safePrintf("<meta http-equiv=\"Content-Type\" "
			 "content=\"text/html; charset=utf-8\">\n");

	// print standard header
	g_pages.printAdminTop ( xbuf , st->m_s , &st->m_r );


	// print the standard header for admin pages
	const char *dd     = "";
	const char *rr     = "";
	const char *render = "";
	const char *us     = "";
	if ( st->m_u && st->m_u[0] ) us = st->m_u;
	//if ( st->m_sfn != -1 ) sprintf ( rtu , "%" PRId32,st->m_sfn );
	if ( st->m_old ) dd = " checked";
	if ( st->m_recycle            ) rr     = " checked";
	if ( st->m_render             ) render = " checked";

	xbuf->safePrintf(
			 "<style>"
			 ".poo { background-color:#%s;}\n"
			 "</style>\n" ,
			 LIGHT_BLUE );


	int32_t clen;
	const char *contentParm = r->getString("content",&clen,"");
	
	// print the input form
	xbuf->safePrintf (
		       "<style>\n"
		       "h2{font-size: 12px; color: #666666;}\n"

		       ".spam { border: 1px solid gray;"
		       "background: #af0000;"
		       "color: #ffffa0;}"
		       ".hs {color: #009900;}"
		       "</style>\n"
		       "<center>"

		  "<table %s>"

			  "<tr><td colspan=5><center><b>"
			  "Parser"
			  "</b></center></td></tr>\n"

		  "<tr class=poo>"
		  "<td>"
		  "<b>url</b>"
			  "<br><font size=-2>"
			  "Type in <b>FULL</b> url to parse."
			  "</font>"
		  "</td>"

		  "</td>"
		  "<td>"
		  "<input type=text name=u value=\"%s\" size=\"40\">\n"
		  "</td>"
		  "</tr>"


			  /*
		  "<tr class=poo>"
		  "<td>"
		  "Parser version to use: "
		  "</td>"
		  "<td>"
		  "<input type=text name=\"version\" size=\"4\" value=\"-1\"> "
		  "</td>"
		  "<td>"
		  "(-1 means to use latest title rec version)<br>"
		  "</td>"
		  "</tr>"
			  */

			  /*
		  "<tr class=poo>"
		  "<td>"
		  "Hop count to use: "
		  "</td>"
		  "<td>"
		  "<input type=text name=\"hc\" size=\"4\" value=\"%" PRId32"\"> "
		  "</td>"
		  "<td>"
		  "(-1 is unknown. For root urls hopcount is always 0)<br>"
		  "</td>"
		  "</tr>"
			  */

		  "<tr class=poo>"
		  "<td>"
			  "<b>use cached</b>"

			  "<br><font size=-2>"
			  "Load page from cache (titledb)?"
			  "</font>"

		  "</td>"
		  "<td>"
		  "<input type=checkbox name=old value=1%s> "
		  "</td>"
		  "</tr>"

			  /*
		  "<tr class=poo>"
		  "<td>"
		  "Reparse root:"
		  "</td>"
		  "<td>"
		  "<input type=checkbox name=artr value=1%s> "
		  "</td>"
		  "<td>"
		  "Apply selected ruleset to root to update quality"
		  "</td>"
		  "</tr>"
			  */

		  "<tr class=poo>"
		  "<td>"
		  "<b>recycle link info</b>"

			  "<br><font size=-2>"
			  "Recycle the link info from the title rec"
			  "Load page from cache (titledb)?"
			  "</font>"

		  "</td>"
		  "<td>"
		  "<input type=checkbox name=recycle value=1%s> "
		  "</td>"
		  "</tr>"

			  /*
		  "<tr class=poo>"
		  "<td>"
		  "Recycle Link Info Imported:"
		  "</td>"
		  "<td>"
		  "<input type=checkbox name=recycleimp value=1%s> "
		  "</td>"
		  "<td>"
		  "Recycle the link info imported from other coll"
		  "</td>"
		  "</tr>"
			  */

		  "<tr class=poo>"
		  "<td>"
		  "<b>render html</b>"

			  "<br><font size=-2>"
			  "Render document content as HTML"
			  "</font>"

		  "</td>"
		  "<td>"
		  "<input type=checkbox name=render value=1%s> "
		  "</td>"
		  "</tr>"

			  /*
		  "<tr class=poo>"
		  "<td>"
		  "Lookup outlinks' ruleset, ips, quality:"
		  "</td>"
		  "<td>"
		  "<input type=checkbox name=oips value=1%s> "
		  "</td>"
		  "<td>"
		  "To compute quality lookup IP addresses of roots "
		  "of outlinks."
		  "</td>"
		  "</tr>"

		  "<tr class=poo>"
		  "<td>"
		  "LinkInfo Coll:"
		  "</td>"
		  "<td>"
		  "<input type=text name=\"oli\" size=\"10\" value=\"\"> "
		  "</td>"
		  "<td>"
		  "Leave empty usually. Uses this coll to lookup link info."
		  "</td>"
		  "</tr>"
			  */

		  "<tr class=poo>"
		  "<td>"
		  "<b>optional query</b>"

			  "<br><font size=-2>"
			  "Leave empty usually. For title generation only."
			  "</font>"

		  "</td>"
		  "<td>"
		  "<input type=text name=\"q\" size=\"20\" value=\"\"> "
		  "</td>"
		       "</tr>",

			  TABLE_STYLE,
			  us ,
			   dd,
			   rr, 
			   render
			  );

	xbuf->safePrintf(
		  "<tr class=poo>"
			  "<td>"
			  "<b>content type below is</b>"
			  "<br><font size=-2>"
			  "Is the content below HTML? XML? JSON?"
			  "</font>"
			  "</td>"

		  "<td>"
		       //"<input type=checkbox name=xml value=1> "
		       "<select name=ctype>\n"
		       "<option value=%" PRId32" selected>HTML</option>\n"
		       "<option value=%" PRId32">XML</option>\n"
		       "<option value=%" PRId32">JSON</option>\n"
		       "</select>\n"

		  "</td>"
		       "</tr>",
		       (int32_t)CT_HTML,
		       (int32_t)CT_XML,
		       (int32_t)CT_JSON
			  );

	xbuf->safePrintf(

			  "<tr class=poo>"
			  "<td><b>content</b>"
			  "<br><font size=-2>"
			  "Use this content for the provided <i>url</i> "
			  "rather than downloading it from the web."
			  "</td>"

			  "<td>"
			  "<textarea rows=10 cols=80 name=content>"
			  "%s"
			  "</textarea>"
			  "</td>"
			  "</tr>"

		  "</table>"
		  "</center>"
		  "</form>"
		  "<br>",

			  //oips ,
			  contentParm );



	xbuf->safePrintf(
			 "<center>"
			 "<input type=submit value=Submit>"
			 "</center>"
			 );


	// just print the page if no url given
	if ( ! st->m_u || ! st->m_u[0] ) return processLoop ( st );


	XmlDoc *xd = &st->m_xd;
	// set this up
	SpiderRequest sreq;
	strcpy(sreq.m_url,st->m_u);
	int32_t firstIp = hash32n(st->m_u);
	if ( firstIp == -1 || firstIp == 0 ) firstIp = 1;
	// parentdocid of 0
	sreq.setKey( firstIp, 0LL, false );
	sreq.m_isPageParser = 1;
	sreq.m_hopCount = st->m_hopCount;
	sreq.m_hopCountValid = 1;
	sreq.m_fakeFirstIp   = 1;
	sreq.m_firstIp = firstIp;
	Url nu;
	nu.set(sreq.m_url);
	sreq.m_domHash32 = nu.getDomainHash32();
	sreq.m_siteHash32 = nu.getHostHash32();

	// . get provided content if any
	// . will be NULL if none provided
	// . "content" may contain a MIME
	int32_t  contentLen = 0;
	const char *content = r->getString ( "content" , &contentLen , NULL );
	// is the "content" url-encoded? default is true.
	// mark doesn't like to url-encode his content
	if ( ! content ) {
		content    = r->getUnencodedContent    ();
		contentLen = r->getUnencodedContentLen ();
	}
	// ensure null
	if ( contentLen == 0 ) content = NULL;

	uint8_t contentType = CT_HTML;
	if ( r->getBool("xml",0) ) contentType = CT_XML;

	contentType = r->getLong("ctype",contentType);//CT_HTML);


	// if facebook, load xml content from title rec...
	bool isFacebook = strstr(st->m_u,"http://www.facebook.com/") ? true : false;
	if ( isFacebook && ! content ) {
		int64_t docId = Titledb::getProbableDocId((char*)st->m_u);
		sprintf(sreq.m_url ,"%" PRIu64 "", (uint64_t) docId);
		sreq.m_isPageReindex = true;
	}

	// hack
	if ( content ) {
		st->m_dbuf.purge();
		st->m_dbuf.safeStrcpy(content);
		content = st->m_dbuf.getBufStart();
	}

	// . use the enormous power of our new XmlDoc class
	// . this returns false if blocked
	if ( ! xd->set4 ( &sreq       ,
			  NULL        ,
			  (char*)st->m_coll  ,
			  &st->m_wbuf        ,
			  0, //niceness
			  (char*)content ,
			  false, // deletefromindex
			  0, // forced ip
			  contentType ))
		// return error reply if g_errno is set
		return sendErrorReply ( st , g_errno );
	// make this our callback in case something blocks
	xd->setCallback ( st , processLoop );
	// . set xd from the old title rec if recycle is true
	// . can also use XmlDoc::m_loadFromOldTitleRec flag
	if ( st->m_recycle ) xd->m_recycleContent = true;

	return processLoop ( st );
}

bool processLoop ( void *state ) {
	// cast it
	State8 *st = (State8 *)state;
	// get the xmldoc
	XmlDoc *xd = &st->m_xd;

	// error?
	if ( g_errno ) return sendErrorReply ( st , g_errno );

	if ( st->m_u && st->m_u[0] ) {
		// now get the meta list, in the process it will print out a 
		// bunch of junk into st->m_xbuf
		char *metalist = xd->getMetaList ( );
		if ( ! metalist ) return sendErrorReply ( st , g_errno );
		// return false if it blocked
		if ( metalist == (void *)-1 ) return false;
		// for debug...
		if ( ! xd->m_indexCode ) xd->doConsistencyTest ( false );
		// print it out
		xd->printDoc( &st->m_xbuf );
	}

	// print reason we can't analyze it (or index it)
	//if ( st->m_indexCode != 0 ) {
	//	st->m_xbuf.safePrintf ("<br><br><b>indexCode: %s</b>\n<br>", 
	//			  mstrerror(st->m_indexCode));
	//}

	// print the final tail
	//p += g_httpServer.printTail ( p , pend - p );

	//log("parser: send sock=%" PRId32,st->m_s->m_sd);
	
	// now encapsulate it in html head/tail and send it off
	bool status = g_httpServer.sendDynamicPage( st->m_s , 
						    st->m_xbuf.getBufStart(), 
						    st->m_xbuf.length() ,
						    -1, //cachtime
						    false ,//postreply?
						    NULL, //ctype
						    -1 , //httpstatus
						    NULL,//cookie
						    "utf-8");
	// delete the state now
	if ( st->m_freeIt ) {
		mdelete ( st , sizeof(State8) , "PageParser" );
		delete (st);
	}
	// return the status
	return status;
}




// returns true
bool sendErrorReply ( void *state , int32_t err ) {
	// ensure this is set
	if ( ! err ) { g_process.shutdownAbort(true); }
	// get it
	State8 *st = (State8 *)state;
	// get the tcp socket from the state
	TcpSocket *s = st->m_s;

	char tmp [ 1024*32 ] ;
	sprintf ( tmp , "<b>had server-side error: %s</b><br>",
		  mstrerror(g_errno));
	// nuke state8
	mdelete ( st , sizeof(State8) , "PageGet1" );
	delete (st);
	// erase g_errno for sending
	//g_errno = 0;
	// . now encapsulate it in html head/tail and send it off
	//return g_httpServer.sendDynamicPage ( s , tmp , strlen(tmp) );
	return g_httpServer.sendErrorReply ( s, err, mstrerror(err) );
}

// for procog
bool sendPageAnalyze ( TcpSocket *s , HttpRequest *r ) {

	// make a state
	State8 *st;
	try { st = new (State8); }
	catch(std::bad_alloc&) {
		g_errno = ENOMEM;
		log("PageParser: new(%i): %s", 
		    (int)sizeof(State8),mstrerror(g_errno));
		return g_httpServer.sendErrorReply(s,500,
						   mstrerror(g_errno));}
	mnew ( st , sizeof(State8) , "PageParser" );
	st->m_freeIt = true;
	st->m_state           = NULL;
	//st->m_callback        = callback;
	//st->m_q               = q;
	//st->m_termFreqs       = termFreqs;
	//st->m_termFreqWeights = termFreqWeights;
	//st->m_affWeights      = affWeights;
	//st->m_total           = (score_t)-1;
	st->m_indexCode       = 0;
	st->m_blocked         = false;
	st->m_didRootDom      = false;
	st->m_didRootWWW      = false;
	st->m_wasRootDom      = false;
	st->m_u               = NULL;

	// password, too
	int32_t pwdLen = 0;
	const char *pwd = r->getString ( "pwd" , &pwdLen );
	if ( pwdLen > 31 ) pwdLen = 31;
	if ( pwdLen > 0 ) strncpy ( st->m_pwd , pwd , pwdLen );
	st->m_pwd[pwdLen]='\0';

	// save socket ptr
	st->m_s = s;
	st->m_r.copy ( r );

	// get the collection
	const char *coll    = r->getString ( "c" , &st->m_collLen ,NULL /*default*/);
	if ( ! coll ) coll = g_conf.m_defaultColl;
	int32_t collLen = strlen(coll);
	if ( collLen > MAX_COLL_LEN ) return sendErrorReply ( st , ENOBUFS );
	strcpy ( st->m_coll , coll );

	// version to use, if -1 use latest
	st->m_titleRecVersion = r->getLong("version",-1);
	if ( st->m_titleRecVersion == -1 ) 
		st->m_titleRecVersion = TITLEREC_CURRENT_VERSION;
	// default to 0 if not provided
	st->m_hopCount = r->getLong("hc",0);
	int32_t  old     = r->getLong   ( "old", 0 );

	// url will override docid if given
	st->m_docId = r->getLongLong ("d",-1);
	st->m_docId = r->getLongLong ("docid",st->m_docId);

	int32_t ulen;
	const char *u = st->m_r.getString("u",&ulen,NULL);
	if ( ! u ) u = st->m_r.getString("url",&ulen,NULL);
	if ( ! u && st->m_docId == -1LL ) 
		return sendErrorReply ( st , EBADREQUEST );

	// set url in state class (may have length 0)
	//if ( u ) st->m_url.set ( u , ulen );
	//st->m_urlLen = ulen;
	st->m_u = u;
	st->m_ulen = 0;
	if ( u ) st->m_ulen = strlen(u);
	// should we recycle link info?
	st->m_recycle  = r->getLong("recycle",1);
	st->m_recycle2 = r->getLong("recycleimp",0);
	st->m_render   = r->getLong("render" ,0) ? true : false;
	st->m_recompute = r->getLong("recompute" ,0) ? true : false;
	// for quality computation... takes way longer cuz we have to 
	// lookup the IP address of every outlink, so we can get its root
	// quality using Msg25 which needs to filter out voters from that IP
	// range.
	st->m_oips     = r->getLong("oips"    ,0);
	//st->m_page = r->getLong("page",1);

	int32_t  linkInfoLen  = 0;
	// default is NULL
	const char *linkInfoColl = r->getString ( "oli" , &linkInfoLen, NULL );
	if ( linkInfoColl ) strcpy ( st->m_linkInfoColl , linkInfoColl );
	else st->m_linkInfoColl[0] = '\0';
	
	// should we use the old title rec?
	st->m_old    = old;
 	//no more setting the default root quality to 30, instead if we do not
 	// know it setting it to -1
 	st->m_rootQuality=-1;

	// header
	//st->m_xbuf.safePrintf("<meta http-equiv=\"Content-Type\" "
	//		 "content=\"text/html; charset=utf-8\">\n");

	XmlDoc *xd = &st->m_xd;

	int32_t isXml = r->getLong("xml",0);

	// if got docid, use that
	if ( st->m_docId != -1 ) {
		if ( ! xd->set3 ( st->m_docId,
				  st->m_coll,
				  0 ) ) // niceness
			// return error reply if g_errno is set
			return sendErrorReply ( st , g_errno );
		// make this our callback in case something blocks
		xd->setCallback ( st , gotXmlDoc );
		xd->m_pbuf = &st->m_wbuf;
		// reset this flag
		st->m_donePrinting = false;
		// . set xd from the old title rec if recycle is true
		// . can also use XmlDoc::m_loadFromOldTitleRec flag
		//if ( st->m_recycle ) xd->m_recycleContent = true;
		xd->m_recycleContent = true;
		// force this on
		//xd->m_useSiteLinkBuf = true;
		//xd->m_usePageLinkBuf = true;
		if ( isXml ) xd->m_printInXml = true;
		// now tell it to fetch the old title rec
		if ( ! xd->loadFromOldTitleRec () )
			// return false if this blocks
			return false;
		return gotXmlDoc ( st );
	}			

	// set this up
	SpiderRequest sreq;
	if ( st->m_u ) strcpy(sreq.m_url,st->m_u);
	int32_t firstIp = hash32n(st->m_u);
	if ( firstIp == -1 || firstIp == 0 ) firstIp = 1;
	// parentdocid of 0
	sreq.setKey( firstIp, 0LL, false );
	sreq.m_isPageParser = 1;
	sreq.m_hopCount = st->m_hopCount;
	sreq.m_hopCountValid = 1;
	sreq.m_fakeFirstIp   = 1;
	sreq.m_firstIp = firstIp;
	Url nu;
	nu.set(sreq.m_url);
	sreq.m_domHash32 = nu.getDomainHash32();
	sreq.m_siteHash32 = nu.getHostHash32();

	// . get provided content if any
	// . will be NULL if none provided
	// . "content" may contain a MIME
	int32_t  contentLen = 0;
	const char *content = r->getString ( "content" , &contentLen , NULL );
	if ( ! content ) {
		content    = r->getUnencodedContent    ();
		contentLen = r->getUnencodedContentLen ();
	}
	// ensure null
	if ( contentLen == 0 ) content = NULL;

	int32_t ctype = r->getLong("ctype",CT_HTML);

	// . use the enormous power of our new XmlDoc class
	// . this returns false if blocked
	if ( ! xd->set4 ( &sreq       ,
			  NULL        ,
			  (char*)st->m_coll  ,
			  // we need this so the term table is set!
			  &st->m_wbuf        , // XmlDoc::m_pbuf
			  0, // niceness
			  (char*)content ,
			  false, // deletefromindex
			  0, // forced ip
			  ctype ))
		// return error reply if g_errno is set
		return sendErrorReply ( st , g_errno );
	// make this our callback in case something blocks
	xd->setCallback ( st , gotXmlDoc );
	// reset this flag
	st->m_donePrinting = false;
	// prevent a core here in the event we download the page content
	xd->m_crawlDelayValid = true;
	xd->m_crawlDelay = 0;
	// . set xd from the old title rec if recycle is true
	// . can also use XmlDoc::m_loadFromOldTitleRec flag
	//if ( st->m_recycle ) xd->m_recycleContent = true;
	// only recycle if docid is given!!
	if ( st->m_recycle ) xd->m_recycleContent = true;
	// force this on
	//xd->m_useSiteLinkBuf = true;
	//xd->m_usePageLinkBuf = true;
	if ( isXml ) xd->m_printInXml = true;

	return gotXmlDoc ( st );
}

bool gotXmlDoc ( void *state ) {
	// cast it
	State8 *st = (State8 *)state;
	// get the xmldoc
	XmlDoc *xd = &st->m_xd;

	// if we loaded from old title rec, it should be there!

	// error?
	if ( g_errno ) return sendErrorReply ( st , g_errno );

	bool printIt = false;
	if ( st->m_u && st->m_u[0] ) printIt = true;
	if ( st->m_docId != -1LL ) printIt = true;
	if ( st->m_donePrinting ) printIt = false;

	// do not re-call this if printDocForProCog blocked... (check length())
	if ( printIt ) {
		// mark as done
		st->m_donePrinting = true;
		// always re-compute the page inlinks dynamically, do not
		// use the ptr_linkInfo1 stored in titlerec!!
		// NO! not if set from titlerec/docid
		if ( st->m_recompute )
			xd->m_linkInfo1Valid = false;
		// . print it out
		// . returns false if blocks, true otherwise
		// . sets g_errno on error
		if ( ! xd->printDocForProCog ( &st->m_xbuf, &st->m_r ) )
			return false;
		// error?
		if ( g_errno ) return sendErrorReply ( st , g_errno );
	}

	int32_t isXml = st->m_r.getLong("xml",0);
	char ctype2 = CT_HTML;
	if ( isXml ) ctype2 = CT_XML;
	// now encapsulate it in html head/tail and send it off
	bool status = g_httpServer.sendDynamicPage( st->m_s , 
						    st->m_xbuf.getBufStart(), 
						    st->m_xbuf.length() ,
						    -1, //cachtime
						    false ,//postreply?
						    &ctype2,
						    -1 , //httpstatus
						    NULL,//cookie
						    "utf-8");
	// delete the state now
	if ( st->m_freeIt ) {
		mdelete ( st , sizeof(State8) , "PageParser" );
		delete (st);
	}
	// return the status
	return status;
}
