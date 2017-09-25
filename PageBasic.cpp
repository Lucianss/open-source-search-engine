#include "SafeBuf.h"
#include "HttpRequest.h"
#include "SearchInput.h"
#include "Collectiondb.h"
#include "Pages.h"
#include "Parms.h"
#include "Spider.h"
#include "SpiderColl.h"
#include "SpiderLoop.h"
#include "PageResults.h" // for RESULT_HEIGHT
#include "Stats.h"
#include "PageRoot.h"


// 5 seconds
#define DEFAULT_WIDGET_RELOAD 1000

///////////
//
// main > Basic > Settings
//
///////////

bool printSitePatternExamples ( SafeBuf *sb , HttpRequest *hr ) {

	// true = useDefault?
	CollectionRec *cr = g_collectiondb.getRec ( hr , true );
	if ( ! cr ) return true;

	/*
	// it is a safebuf parm
	char *siteList = cr->m_siteListBuf.getBufStart();
	if ( ! siteList ) siteList = "";

	SafeBuf msgBuf;
	char *status = "";
	int32_t max = 1000000;
	if ( cr->m_siteListBuf.length() > max ) {
		msgBuf.safePrintf( "<font color=red><b>"
				   "Site list is over %" PRId32" bytes large, "
				   "too many to "
				   "display on this web page. Please use the "
				   "file upload feature only for now."
				   "</b></font>"
				   , max );
		status = " disabled";
	}
	*/


	/*
	sb->safePrintf(
		       "On the command like you can issue a command like "

		       "<i>"
		       "gb addurls &lt; fileofurls.txt"
		       "</i> or "

		       "<i>"
		       "gb addfile &lt; *.html"
		       "</i> or "

		       "<i>"
		       "gb injecturls &lt; fileofurls.txt"
		       "</i> or "

		       "<i>"
		       "gb injectfile &lt; *.html"
		       "</i> or "

		       "to schedule downloads or inject content directly "
		       "into Gigablast."

		       "</td><td>"

		       "<input "
		       "size=20 "
		       "type=file "
		       "name=urls>"
		       "</td></tr>"

		       );
	*/	      

	// example table
	sb->safePrintf ( "<a name=examples></a>"
			 "<table %s>"
			 "<tr class=hdrow><td colspan=2>"
			 "<center><b>Site List Examples</b></tr></tr>"
			 //"<tr bgcolor=#%s>"
			 //"<td>"
			 ,TABLE_STYLE );//, DARK_BLUE);
			 

	sb->safePrintf(
		       //"*"
		       //"</td>"
		       //"<td>Spider all urls encountered. If you just submit "
		       //"this by itself, then Gigablast will initiate spidering "
		       //"automatically at dmoz.org, an internet "
		      //"directory of good sites.</td>"
		       //"</tr>"

		      "<tr>"
		      "<td>goodstuff.com</td>"
		      "<td>"
		      "Spider the url <i>goodstuff.com/</i> and spider "
		      "any links we harvest that have the domain "
		      "<i>goodstuff.com</i>"
		      "</td>"
		      "</tr>"

		      // protocol and subdomain match
		      "<tr>"
		      "<td>http://www.goodstuff.com/</td>"
		      "<td>"
		      "Spider the url "
		      "<i>http://www.goodstuff.com/</i> and spider "
		      "any links we harvest that start with "
		      "<i>http://www.goodstuff.com/</i>. NOTE: if the url "
		      "www.goodstuff.com redirects to foo.goodstuff.com then "
		      "foo.goodstuff.com still gets spidered "
		      "because it is considered to be manually added, but "
		      "no other urls from foo.goodstuff.com will be spidered."
		      "</td>"
		      "</tr>"

		      // protocol and subdomain match
		      "<tr>"
		      "<td>http://justdomain.com/foo/</td>"
		      "<td>"
		      "Spider the url "
		      "<i>http://justdomain.com/foo/</i> and spider "
		      "any links we harvest that start with "
		      "<i>http://justdomain.com/foo/</i>. "
		      "Urls that start with "
		      "<i>http://<b>www.</b>justdomain.com/</i>, for example, "
		      "will NOT match this."
		      "</td>"
		      "</tr>"

		      "<tr>"
		      "<td>seed:www.goodstuff.com/myurl.html</td>"
		      "<td>"
		      "Spider the url <i>www.goodstuff.com/myurl.html</i>. "
		      "Add any outlinks we find into the "
		      "spider queue, but those outlinks will only be "
		      "spidered if they "
		      "match ANOTHER line in this site list."
		      "</td>"
		      "</tr>"


		      // protocol and subdomain match
		      "<tr>"
		      "<td>site:http://www.goodstuff.com/</td>"
		      "<td>"
		      "Allow any urls starting with "
		      "<i>http://www.goodstuff.com/</i> to be spidered "
		      "if encountered."
		      "</td>"
		      "</tr>"

		      // subdomain match
		      "<tr>"
		      "<td>site:www.goodstuff.com</td>"
		      "<td>"
		      "Allow any urls starting with "
		      "<i>www.goodstuff.com/</i> to be spidered "
		      "if encountered."
		      "</td>"
		      "</tr>"

		      "<tr>"
		      "<td>-site:bad.goodstuff.com</td>"
		      "<td>"
		      "Do not spider any urls starting with "
		      "<i>bad.goodstuff.com/</i> to be spidered "
		      "if encountered."
		      "</td>"
		      "</tr>"

		      // domain match
		      "<tr>"
		      "<td>site:goodstuff.com</td>"
		      "<td>"
		      "Allow any urls starting with "
		      "<i>goodstuff.com/</i> to be spidered "
		      "if encountered."
		      "</td>"
		      "</tr>"

		      // spider this subdir
		      "<tr>"
		      "<td><nobr>site:"
		      "http://www.goodstuff.com/goodir/anotherdir/</nobr></td>"
		      "<td>"
		      "Allow any urls starting with "
		      "<i>http://www.goodstuff.com/goodir/anotherdir/</i> "
		      "to be spidered "
		      "if encountered."
		      "</td>"
		      "</tr>"


		      // exact match
		      
		      //"<tr>"
		      //"<td>exact:http://xyz.goodstuff.com/myurl.html</td>"
		      //"<td>"
		      //"Allow this specific url."
		      //"</td>"
		      //"</tr>"

		      /*
		      // local subdir match
		      "<tr>"
		      "<td>file://C/mydir/mysubdir/"
		      "<td>"
		      "Spider all files in the given subdirectory or lower. "
		      "</td>"
		      "</tr>"

		      "<tr>"
		      "<td>-file://C/mydir/mysubdir/baddir/"
		      "<td>"
		      "Do not spider files in this subdirectory."
		      "</td>"
		      "</tr>"
		      */

		      // connect to a device and index it as a stream
		      //"<tr>"
		      //"<td>stream:/dev/eth0"
		      //"<td>"
		      //"Connect to a device and index it as a stream. "
		      //"It will be treated like a single huge document for "
		      //"searching purposes with chunks being indexed in "
		      //"realtime. Or chunk it up into individual document "
		      //"chunks, but proximity term searching will have to "
		      //"be adjusted to compute query term distances "
		      //"inter-document."
		      //"</td>"
		      //"</tr>"

		      // negative subdomain match
		      "<tr>"
		      "<td>contains:goodtuff</td>"
		      "<td>Spider any url containing <i>goodstuff</i>."
		      "</td>"
		      "</tr>"

		      "<tr>"
		      "<td>-contains:badstuff</td>"
		      "<td>Do not spider any url containing <i>badstuff</i>."
		      "</td>"
		      "</tr>"

		      /*
		      "<tr>"
		      "<td>regexp:-pid=[0-9A-Z]+/</td>"
		      "<td>Url must match this regular expression. "
		      "Try to avoid using these if possible; they can slow "
		      "things down and are confusing to use."
		      "</td>"
		      "</tr>"
		      */

		      // tag match
		      "<tr><td>"
		      //"<td>tag:boots contains:boots<br>"
		      "<nobr>tag:boots site:www.westernfootwear."
		      "</nobr>com<br>"
		      "tag:boots cowboyshop.com<br>"
		      "tag:boots contains:/boots<br>"
		      "tag:boots site:www.moreboots.com<br>"
		      "<nobr>tag:boots http://lotsoffootwear.com/"
		      "</nobr><br>"
		      //"<td>t:boots -contains:www.cowboyshop.com/shoes/</td>"
		      "</td><td>"
		      "Advance users only. "
		      "Tag any urls matching these 5 url patterns "
		      "so we can use "
		      "the expression <i>tag:boots</i> in the "
		      "<a href=\"/admin/filters\">url filters</a> and perhaps "
		      "give such urls higher spider priority. "
		      "For more "
		      "precise spidering control over url subsets. "
		      "Preceed any pattern with the tagname followed by "
		      "space to tag it."
		      "</td>"
		      "</tr>"


		      "<tr>"
		      "<td># This line is a comment.</td>"
		      "<td>Empty lines and lines starting with # are "
		      "ignored."
		      "</td>"
		      "</tr>"

		      "</table>"
		      );

	return true;
}

static bool printScrollingWidget(SafeBuf *sb, CollectionRec *cr) {

	sb->safePrintf("<script type=\"text/javascript\">\n\n");

	// if user has the scrollbar at the top
	// in the widget we do a search every 15 secs
	// to try to load more recent results. we should
	// return up to 10 results above your last 
	// top docid and 10 results below it. that way
	// no matter which of the 10 results you were
	// viewing your view should remaing unchanged.
	sb->safePrintf(

		       // global var
		       "var forcing;"

		       "function widget123_handler_reload() {"
		       // return if reply is not fully ready
		       "if(this.readyState != 4 )return;"

		       // if error or empty reply then do nothing
		       "if(!this.responseText)return;"
		       // get the widget container
		       "var w=document.getElementById(\"widget123\");"

		       // GET DOCID of first div/searchresult
		       "var sd=document.getElementById("
		       "\"widget123_scrolldiv\");"
		       "var cd;"
		       "if ( sd ) cd=sd.firstChild;"
		       "var fd=0;"
		       // if nodetype is 3 that means it says
		       // 'No results. Waiting for spider to kick in.'
		       "if(cd && cd.nodeType==1) fd=cd.getAttribute('docid');"

		       // if the searchbox has the focus then do not
		       // update the content just yet...
		       "var qb=document.getElementById(\"qbox\");"
		       "if(qb&&qb==document.activeElement)"
		       "return;"

		       //"alert(this.responseText);"

		       // or if not forced and they scrolled down
		       // don't jerk them back up again. unless
		       // the inner html starts with 'No results'!
		       "if(!forcing&&sd&&sd.scrollTop!=0&&cd&&cd.nodeType==1)"
		       "return;"


		       // just set the widget content to the reply
		       "w.innerHTML=this.responseText;"

		       //
		       // find that SAME docid in response and see
		       // how many new results were added above it
		       //
		       "var added=0;"
		       // did we find the docid?
		       "var found=0;"
		       // get div again since we updated innerHTML
		       "sd=document.getElementById("
		       "\"widget123_scrolldiv\");"
		       // scan the kids
		       "var kid=sd.firstChild;"
		       // begin the while loop to scan the kids
		       "while (kid) {"
		       // if div had no docid it might have been a line
		       // break div, so ignore
		       "if (!kid.hasAttribute('docid') ) {"
		       "kid=kid.nextSibling;"
		       "continue;"
		       "}"
		       // set kd to docid of kid
		       "var kd=kid.getAttribute('docid');"
		       // stop if we hit our original top docid
		       "if(kd==fd) {found=1;break;}"
		       // otherwise count it as a NEW result we got
		       "added++;"
		       // advance kid
		       "kid=kid.nextSibling;"
		       // end while loop
		       "}"

		       //"alert(\"added=\"+added);"

		       // how many results did we ADD above the
		       // reported "topdocid" of the widget?
		       // it should be in the ajax reply from the
		       // search engine. how many result were above
		       // the given "topdocid".
		       //"var ta=document.getElementById(\"topadd\");"
		       //"var added=0;"
		       //"if(ta)added=ta.value;"

		       // if nothing added do nothing
		       "if (added==0)return;"

		       // if original top docid not found, i guess we
		       // added too many new guys to the top of the
		       // search results, so don't bother scrolling
		       // just reset to top
		       "if (!found) return;"

		       // show that
		       //"alert(this.responseText);"

		       // get the div that has the scrollbar
		       "var sd=document.getElementById("
		       "\"widget123_scrolldiv\");"
		       // save current scroll pos
		       "var oldpos=parseInt(sd.scrollTop);"
		       // note it
		       //"alert (sd.scrollTop);"
		       // preserve the relative scroll position so we
		       // do not jerk around since we might have added 
		       // "added" new results to the top.
		       "sd.scrollTop += added*%" PRId32";"

		       // try to scroll out new results if we are
		       // still at the top of the scrollbar and
		       // there are new results to scroll.
		       "if(oldpos==0)widget123_scroll();}\n\n"

		       // for preserving scrollbar position
		       ,(int32_t)RESULT_HEIGHT +2*PADDING

		       );


	// scroll the widget up until we hit the 0 position
	sb->safePrintf(
		       "function widget123_scroll() {"
		       // only scroll if at the top of the widget
		       // and not scrolled down so we do not
		       // interrupt
		       "var sd=document.getElementById("
		       "\"widget123_scrolldiv\");"
		       // TODO: need parseInt here?
		       "var pos=parseInt(sd.scrollTop);"
		       // note it
		       //"alert (sd.scrollTop);"
		       // if already at the top of widget, return
		       "if(pos==0)return;"
		       // decrement by 3 pixels
		       "pos=pos-3;"
		       // do not go negative
		       "if(pos<0)pos=0;"
		       // assign to scroll up. TODO: need +\"px\"; ?
		       "sd.scrollTop=pos;"
		       // all done, then return
		       "if(pos==0) return;"
		       // otherwise, scroll more in 3ms
		       // TODO: make this 1000ms on result boundaries
		       // so it delays on each new result. perhaps make
		       // it less than 1000ms if we have a lot of 
		       // results above us!
		       "setTimeout('widget123_scroll()',3);}\n\n"

		       );

	// this function appends the search results to what is
	// already in the widget.
	sb->safePrintf(
		       "function widget123_handler_append() {"
		       // return if reply is not fully ready
		       "if(this.readyState != 4 )return;"
		       // i guess we are done... release the lock
		       "outstanding=0;"
		       // if error or empty reply then do nothing
		       "if(!this.responseText)return;"
		       // if too small
		       "if(this.responseText.length<=3)return;"
		       // get the widget container
		       "var w=document.getElementById("
		       "\"widget123_scrolldiv\");"
		       // just set the widget content to the reply
		       "w.innerHTML+=this.responseText;"
		       "}\n\n"
		       );


	//sb->safePrintf ( "</script>\n\n" );

	int32_t widgetWidth = 300;
	int32_t widgetHeight = 500;

	// make the ajax url that gets the search results
	SafeBuf ub;
	ub.safePrintf("/search"
		      //"format=ajax"
		      "?c=%s"
		      //"&prepend=gbsortbyint%%3Agbspiderdate"
		      "&q=-gbstatus:0+gbsortbyint%%3Agbindexdate"
		      "&sc=0" // no site clustering
		      "&dr=0" // no deduping
			      // 10 results at a time
		      "&n=10"
		      "&widgetheight=%" PRId32
		      "&widgetwidth=%" PRId32
		      , cr->m_coll
		      , widgetHeight
		      , widgetWidth
		      );
	//ub.safePrintf("&topdocid="
	//	      );

	// get the search results from neo as soon as this div is
	// being rendered, and set its contents to them
	sb->safePrintf(//"<script type=text/javascript>"

		       "function widget123_reload(force) {"
			 
		       // when the user submits a new query in the
		       // query box we set force to false when
		       // we call this (see PageResults.cpp) so that
		       // we do not register multiple timeouts
		       "if ( ! force ) "
		       "setTimeout('widget123_reload(0)',%" PRId32");"

		       // get the query box
		       "var qb=document.getElementById(\"qbox\");"

		       // if forced then turn off focus for searchbox
		       // since it was either 1) the initial call
		       // or 2) someone submitted a query and
		       // we got called from PageResults.cpp
		       // onsubmit event.
		       "if (force&&qb) qb.blur();"


		       // if the searchbox has the focus then do not
		       // reload!! unless force is true..
		       "if(qb&&qb==document.activeElement&&!force)"
		       "return;"

		       //"var ee=document.getElementById(\"sbox\");"
		       //"if (ee)alert('reloading '+ee.style.display);"

		       // do not do timer reload if searchbox is
		       // visible because we do not want to interrupt
		       // a possible search
		       //"if(!force&&ee && ee.style.display=='')return;"


		       // do not bother timed reloading if scrollbar pos
		       // not at top or near bottom
		       "var sd=document.getElementById("
		       "\"widget123_scrolldiv\");"

		       "if ( sd && !force ) {"
		       "var pos=parseInt(sd.scrollTop);"
		       "if (pos!=0) return;"
		       "}"


		       "var client=new XMLHttpRequest();"
		       "client.onreadystatechange="
		       "widget123_handler_reload;"

		       // . this url gets the search results
		       // . get them in "ajax" format so we can embed
		       //   them into the base html as a widget
		       "var u='%s&format=ajax';"

		       // append our query from query box if there
		       "var qv;"
		       "if (qb) qv=qb.value;"
		       "if (qv){"
		       //"u+='&q=';"
		       "u+='&prepend=';"
		       "u+=encodeURI(qv);"
		       "}"

		       // set global var so handler knows if we were
		       // forced or not
		       "forcing=force;"

		       // get the docid at the top of the widget
		       // so we can get SURROUNDING search results,
		       // like 10 before it and 10 after it for
		       // our infinite scrolling
		       //"var td=document.getElementById('topdocid');"
		       //"if ( td ) u=u+\"&topdocid=\"+td.value;"

		       //"alert('reloading');"

		       "client.open('GET',u);"
		       "client.send();"
		       "}\n\n"

		       // when page loads, populate the widget immed.
		       "widget123_reload(1);\n\n"

		       // initiate the timer loop since it was
		       // not initiated on that call since we had to
		       // set force=1 to load in case the query box
		       // was currently visible.
		       "setTimeout('widget123_reload(0)',%" PRId32");"

		       //, widgetHeight
		       , (int32_t)DEFAULT_WIDGET_RELOAD
		       , ub.getBufStart()
		       , (int32_t)DEFAULT_WIDGET_RELOAD
		       );

	//
	// . call this when scrollbar gets 5 up from bottom
	// . but if < 10 new results are appended, then stop!
	//
	sb->safePrintf(
		       "var outstanding=0;\n\n"

		       "function widget123_append() {"
			      
		       // bail if already outstanding
		       "if (outstanding) return;"

		       // if scrollbar not near bottom, then return
		       "var sd=document.getElementById("
		       "\"widget123_scrolldiv\");"
		       "if ( sd ) {"
		       "var pos=parseInt(sd.scrollTop);"
		       "if (pos < (sd.scrollHeight-%" PRId32")) "
		       "return;"
		       "}"

		       // . this url gets the search results
		       // . just get them so we can APPEND them to
		       //   the widget, so it will be just the
		       //   "results" divs
		       "var u='%s&format=append';"

		       // . get score of the last docid in our widget
		       // . it should be persistent.
		       // . it is like a bookmark for scrolling
		       // . append results AFTER it into the widget
		       // . this way we can deal with the fact that
		       //   we may be adding 100s of results to this
		       //   query per second, especially if spidering
		       //   at a high rate. and this will keep the
		       //   results we append persistent.
		       // . now we scan the children "search result"
		       //   divs of the "widget123_scrolldiv" div
		       //   container to get the last child and get
		       //   its score/docid so we can re-do the search
		       //   and just get the search results with
		       //   a score/docid LESS THAN that. THEN our
		       //   results should be contiguous.
		       // . get the container div, "cd"
		       "var cd=document.getElementById("
		       "'widget123_scrolldiv');"
		       // must be there
		       "if(!cd)return;"
		       // get the last child div in there
		       "var d=cd.lastChild.previousSibling;"
		       // must be there
		       "if(!d)return;"
		       // now that we added <hr> tags after each div do this!
		       "d=d.previousSibling;"
		       // must be there
		       "if(!d)return;"
		       // get docid/score
		       "u=u+\"&maxserpscore=\"+d.getAttribute('score');"
		       "u=u+\"&minserpdocid=\"+d.getAttribute('docid');"

		       // append our query from query box if there
		       "var qb=document.getElementById(\"qbox\");"
		       "var qv;"
		       "if (qb) qv=qb.value;"
		       "if (qv){"
		       //"u+='&q=';"
		       "u+='&prepend=';"
		       "u+=encodeURI(qv);"
		       "}"


		       // turn on the lock to prevent excessive calls
		       "outstanding=1;"

		       //"alert(\"scrolling2 u=\"+u);"

		       "var client=new XMLHttpRequest();"
		       "client.onreadystatechange="
		       "widget123_handler_append;"

		       //"alert('appending scrollTop='+sd.scrollTop+' scrollHeight='+sd.scrollHeight+' 5results=%" PRId32"'+u);"
		       "client.open('GET',u);"
		       "client.send();"
		       "}\n\n"

		       "</script>\n\n"

		       // if (pos < (sd.scrollHeight-%" PRId32")) return...
		       // once user scrolls down to within last 5
		       // results then try to append to the results.
		       , widgetHeight +5*((int32_t)RESULT_HEIGHT+2*PADDING)


		       , ub.getBufStart()

		       //,widgetHeight +5*((int32_t)RESULT_HEIGHT+2*PADDING
		       );


	// then the WIDGET MASTER div. set the "id" so that the
	// style tag the user sets can control its appearance.
	// when the browser loads this the ajax sets the contents
	// to the reply from neo.

	// on scroll call widget123_append() which will append
	// more search results if we are near the bottom of the
	// widget.

	sb->safePrintf("<div id=widget123 "
		       "style=\"border:2px solid black;"
		       "position:relative;border-radius:10px;"
		       "width:%" PRId32"px;height:%" PRId32"px;\">"
		       , widgetWidth
		       , widgetHeight
		       );

	//sb->safePrintf("<style>"
	//	      "a{color:white;}"
	//	      "</style>");


	sb->safePrintf("Waiting for Server Response...");


	// end the containing div
	sb->safePrintf("</div>");

	return true;
}

bool sendPageWidgets ( TcpSocket *socket , HttpRequest *hr ) {

	// true = usedefault coll?
	CollectionRec *cr = g_collectiondb.getRec ( hr , true );
	if ( ! cr ) {
		g_httpServer.sendErrorReply(socket,500,"invalid collection");
		return true;
	}

	StackBuf<128000> sb;

	printFrontPageShell ( &sb, "widgets", cr , true );

	sb.safePrintf("<br>");
	sb.safePrintf("<br>");

	//char format = hr->getReplyFormat();
	//if ( format == FORMAT_HTML )
	printGigabotAdvice ( &sb , PAGE_BASIC_STATUS , hr , NULL );

	printScrollingWidget ( &sb , cr );

	return g_httpServer.sendDynamicPage (socket, 
					     sb.getBufStart(), 
					     sb.length(),
					     0); // cachetime
}



///////////
//
// main > Basic > Status
//
///////////
bool sendPageBasicStatus ( TcpSocket *socket , HttpRequest *hr ) {
	StackBuf<128000> sb;

	char format = hr->getReplyFormat();


	// true = usedefault coll?
	CollectionRec *cr = g_collectiondb.getRec ( hr , true );
	if ( ! cr ) {
		g_httpServer.sendErrorReply(socket,500,"invalid collection");
		return true;
	}

	if ( format == FORMAT_JSON || format == FORMAT_XML) {
		// this is in PageCrawlBot.cpp
		printCrawlDetails2 ( &sb , cr , format );
		const char *ct = "text/xml";
		if ( format == FORMAT_JSON ) ct = "application/json";
		return g_httpServer.sendDynamicPage (socket, 
						     sb.getBufStart(), 
						     sb.length(),
						     0, // cachetime
						     false,//POSTReply        ,
						     ct);
	}

	// print standard header 
	if ( format == FORMAT_HTML ) {
		// this prints the <form tag as well
		g_pages.printAdminTop ( &sb , socket , hr );

		// table to split between widget and stats in left and right panes
		sb.safePrintf("<TABLE id=pane>"
			      "<TR><TD valign=top>");
	}

	int32_t savedLen1, savedLen2;

	//
	// widget
	//
	// put the widget in here, just sort results by spidered date
	//
	// the scripts do "infinite" scrolling both up and down.
	// but if you are at the top then new results will load above
	// you and we try to maintain your current visual state even though
	// the scrollbar position will change.
	//
	if ( format == FORMAT_HTML ) {

		// save position so we can output the widget code
		// so user can embed it into their own web page
		savedLen1 = sb.length();
		
		printScrollingWidget ( &sb , cr );

		savedLen2 = sb.length();

		// the right table pane is the crawl stats
		sb.safePrintf("</TD><TD valign=top>");

		//
		// show stats
		//
		const char *crawlMsg;
		int32_t crawlStatus = -1;
		getSpiderStatusMsg ( cr , &crawlMsg, &crawlStatus );

		sb.safePrintf(
			      "<table id=stats border=0 cellpadding=5>"

			      "<tr>"
			      "<td><b>Crawl Status Code:</td>"
			      "<td>%" PRId32"</td>"
			      "</tr>"

			      "<tr>"
			      "<td><b>Crawl Status Msg:</td>"
			      "<td>%s</td>"
			      "</tr>"
			      , crawlStatus
			      , crawlMsg);

		// print link to embed the code in their own site
		SafeBuf embed;
		embed.htmlEncode(sb.getBufStart()+savedLen1,
				 savedLen2-savedLen1,
				 false); // encodePoundSign #?
		// convert all ''s to "'s for php's echo ''; cmd
		embed.replaceChar('\'','\"');

		sb.safePrintf("<tr>"
			      "<td valign=top>"
			      "<a onclick=\""
			      "var dd=document.getElementById('hcode');"
			      "if ( dd.style.display=='none' ) "
			      "dd.style.display=''; "
			      "else "
			      "dd.style.display='none';"
			      "\" style=color:blue;>"
			      "<u>"
			      "show Widget HTML code"
			      "</u>"
			      "</a>"
			      "</td><td>"
			      "<div id=hcode style=display:none;"
			      "max-width:800px;>"
			      "%s"
			      "</div>"
			      "</td></tr>"
			      , embed.getBufStart() );

		sb.safePrintf("<tr>"
			      "<td valign=top>"
			      "<a onclick=\""
			      "var dd=document.getElementById('pcode');"
			      "if ( dd.style.display=='none' ) "
			      "dd.style.display=''; "
			      "else "
			      "dd.style.display='none';"
			      "\" style=color:blue;>"
			      "<u>"
			      "show Widget PHP code"
			      "</u>"
			      "</a>"
			      "</td>"
			      "<td>"
			      "<div id=pcode style=display:none;"
			      "max-width:800px;>"
			      "<i>"
			      "echo '"
			      "%s"
			      "';"
			      "</i>"
			      "</div>"
			      "</td></tr>"
			      , embed.getBufStart() );


		sb.safePrintf("</table>\n\n");

		// end the right table pane
		sb.safePrintf("</TD></TR></TABLE>");
	}


	//if ( format != FORMAT_JSON )
	//	// wrap up the form, print a submit button
	//	g_pages.printAdminBottom ( &sb );

	return g_httpServer.sendDynamicPage (socket, 
					     sb.getBufStart(), 
					     sb.length(),
					     0); // cachetime
}
	
