#include "Pages.h"
#include "TcpSocket.h"
#include "HttpRequest.h"
#include "HttpServer.h"
#include "Collectiondb.h"
#include "Parms.h"
#include "Errno.h"

#ifndef PRIVACORE_SAFE_VERSION

static bool sendPageAddDelColl(TcpSocket *s, HttpRequest *r, bool add);

bool sendPageAddColl ( TcpSocket *s , HttpRequest *r ) {
	return sendPageAddDelColl ( s , r , true ); 
}

bool sendPageDelColl ( TcpSocket *s , HttpRequest *r ) {
	return sendPageAddDelColl ( s , r , false ); 
}

bool sendPageAddDelColl ( TcpSocket *s , HttpRequest *r , bool add ) {
	// get collection name
	//int32_t  nclen;
	//char *nc   = r->getString ( "nc" , &nclen );
	//int32_t  cpclen;
	//char *cpc  = r->getString ( "cpc" , &cpclen );

	g_errno = 0;

	//bool cast = r->getLong("cast",0);

	const char *msg = NULL;

	// if any host in network is dead, do not do this
	//if ( g_hostdb.hasDeadHost() ) msg = "A host in the network is dead.";

	char format = r->getReplyFormat();


	if ( format == FORMAT_XML || format == FORMAT_JSON ) {
		// no addcoll given?
		int32_t  page = g_pages.getDynamicPageNumber ( r );
		const char *addcoll = r->getString("addcoll",NULL);
		const char *delcoll = r->getString("delcoll",NULL);
		if ( ! addcoll ) addcoll = r->getString("addColl",NULL);
		if ( ! delcoll ) delcoll = r->getString("delColl",NULL);
		if ( page == PAGE_ADDCOLL && ! addcoll ) {
			g_errno = EBADENGINEER;
			const char *msg = "no addcoll parm provided";
			return g_httpServer.sendErrorReply(s,g_errno,msg,NULL);
		}
		if ( page == PAGE_DELCOLL && ! delcoll ) {
			g_errno = EBADENGINEER;
			const char *msg = "no delcoll parm provided";
			return g_httpServer.sendErrorReply(s,g_errno,msg,NULL);
		}
		return g_httpServer.sendSuccessReply(s,format);
	}

	// error?
	const char *action = r->getString("action",NULL);
	const char *addColl = r->getString("addcoll",NULL);


	StackBuf<64*1024> p;


	//
	// CLOUD SEARCH ENGINE SUPPORT - GIGABOT ERRORS
	//

	SafeBuf gtmp;
	char *gmsg = NULL;
	// is it too big?
	if ( action && addColl && strlen(addColl) > MAX_COLL_LEN ) {
		gtmp.safePrintf("search engine name is too long");
		gmsg = gtmp.getBufStart();
	}
	// from Collectiondb.cpp::addNewColl() ensure coll name is legit
	const char *x = addColl;
	for ( ; x && *x ; x++ ) {
		if ( is_alnum_a(*x) ) continue;
		if ( *x == '-' ) continue;
		if ( *x == '_' ) continue; // underscore now allowed
		break;
	}
	if ( x && *x ) {
		g_errno = EBADENGINEER;
		gtmp.safePrintf("<font color=red>Error. \"%s\" is a "
				"malformed name because it "
				"contains the '%c' character.</font><br><br>",
				addColl,*x);
		gmsg = gtmp.getBufStart();
	}

	//
	// END GIGABOT ERRORS
	//



	//
	// CLOUD SEARCH ENGINE SUPPORT
	//
	// if added the coll successfully, do not print same page, jump to
	// printing the basic settings page so they can add sites to it.
	// crap, this GET request, "r", is missing the "c" parm sometimes.
	// we need to use the "addcoll" parm anyway. maybe print a meta
	// redirect then?
	char guide = r->getLong("guide",0);
	// do not redirect if gmsg is set, there was a problem with the name
	if ( action && ! msg && format == FORMAT_HTML && guide && ! gmsg ) {
		//return g_parms.sendPageGeneric ( s, r, PAGE_BASIC_SETTINGS );
		// just redirect to it
		if ( addColl )
			p.safePrintf("<meta http-equiv=Refresh "
				      "content=\"0; URL=/admin/settings"
				      "?guide=1&c=%s\">",
				      addColl);
		return g_httpServer.sendDynamicPage (s,
						     p.getBufStart(),
						     p.length());
	}


	// print standard header
	g_pages.printAdminTop ( &p , s , r , NULL, 
				"onload=document."
				"getElementById('acbox').focus();");


	if ( g_errno ) {
		msg = mstrerror( g_errno );
	}

	if ( msg && ! guide ) {
		const char *cc = "deleting";
		if ( add ) cc = "adding";
		p.safePrintf (
			  "<center>\n"
			  "<font color=red>"
			  "<b>Error %s collection: %s. "
			  "See log file for details.</b>"
			  "</font>"
			  "</center><br>\n",cc,msg);
	}

	//
	// CLOUD SEARCH ENGINE SUPPORT
	//
	if ( add && guide )
		printGigabotAdvice ( &p , PAGE_ADDCOLL , r , gmsg );



	// print the add collection box
	if ( add /*&& (! nc[0] || g_errno ) */ ) {

		const char *t1 = "Add Collection";
		if ( guide ) t1 = "Add Search Engine";

		p.safePrintf (
			  "<center>\n<table %s>\n"
			   "<tr class=hdrow><td colspan=2>"
			  "<center><b>%s</b></center>"
			  "</td></tr>\n"
			  ,TABLE_STYLE
			  ,t1
			      );
		const char *t2 = "collection";
		if ( guide ) t2 = "search engine";
		const char *str = addColl;
		if ( ! addColl ) str = "";
		p.safePrintf (
			      "<tr bgcolor=#%s>"
			      "<td><b>name of new %s to add</td>\n"
			      "<td><input type=text name=addcoll size=30 "
			      "id=acbox "
			      "value=\"%s\">"
			      "</td></tr>\n"
			      , LIGHT_BLUE
			      , t2 
			      , str
			      );

		// don't show the clone box if we are under gigabot the guide
		if ( ! guide )
			p.safePrintf(
				     "<tr bgcolor=#%s>"
				     "<td><b>clone settings from this "
				     "collection</b>"
				     "<br><font size=1>Copy settings from "
				     "this pre-existing collection. Leave "
				     "blank to "
				     "accept default values.</font></td>\n"
				     "<td><input type=text name=clonecoll "
				     "size=30>"
				     "</td>"
				     "</tr>"
				     , LIGHT_BLUE
				     );

		// collection pwds
		p.safePrintf(
			     "<tr bgcolor=#%s>"
			     "<td><b>collection passwords"
			     "</b>"
			     "<br><font size=1>List of white space separated "
			     "passwords allowed to adminster collection."
			     "</font>"
			     "</td>\n"
			     "<td><input type=text name=collpwd "
			     "size=60>"
			     "</td>"
			     "</tr>"
			     , LIGHT_BLUE
			     );

		// ips box for security
		p.safePrintf(
			     "<tr bgcolor=#%s>"
			     "<td><b>collection ips"
			     "</b>"

			     "<br><font size=1>List of white space separated "
			     "IPs allowed to adminster collection."
			     "</font>"

			     "</td>\n"
			     "<td><input type=text name=collips "
			     "size=60>"
			     "</td>"
			     "</tr>"
			     , LIGHT_BLUE
			     );

		// now list collections from which to copy the config
		//p.safePrintf (
		//	  "<tr><td><b>copy configuration from this "
		//	  "collection</b><br><font size=1>Leave blank to "
		//	  "accept default values.</font></td>\n"
		//	  "<td><input type=text name=cpc value=\"%s\" size=30>"
		//	  "</td></tr>\n",coll);
		p.safePrintf ( "</table></center><br>\n");

		// wrap up the form started by printAdminTop
		g_pages.printAdminBottom ( &p );
		int32_t bufLen = p.length();
		return g_httpServer.sendDynamicPage (s,p.getBufStart(),bufLen);
	}

	// if we added a collection, print its page
	//if ( add && nc[0] && ! g_errno ) 
	//	return g_parms.sendPageGeneric2 ( s , r , PAGE_SEARCH ,
	//					  nc , pwd );

	if ( g_collectiondb.getNumRecsUsed() > 0 ) {
		// print all collections out in a checklist so you can check the
		// ones you want to delete, the values will be the id of that collectn
		p.safePrintf (
			  "<center>\n<table %s>\n"
			  "<tr class=hdrow><td><center><b>Delete Collections"
			  "</b></center></td></tr>\n"
			  "<tr bgcolor=#%s><td>"
			  "<center><b>Select the collections you wish to delete. "
			  //"<font color=red>This feature is currently under "
			  //"development.</font>"
			  "</b></center></td></tr>\n"
			  "<tr bgcolor=#%s><td>"
			  // table within a table
			  "<center><table width=20%%>\n",
			  TABLE_STYLE,
			  LIGHT_BLUE,
			  DARK_BLUE
			      );

		for ( int32_t i = 0 ; i < g_collectiondb.getNumRecs(); i++ ) {
			CollectionRec *cr = g_collectiondb.getRec(i);
			if ( ! cr ) continue;
			p.safePrintf (
				  "<tr bgcolor=#%s><td>"
				  "<input type=checkbox name=delcoll value=\"%s\"> "
				  "%s</td></tr>\n",
				  DARK_BLUE,
				  cr->m_coll,cr->m_coll);
		}
		p.safePrintf( "</table></center></td></tr></table><br>\n" );
	}

	// wrap up the form started by printAdminTop
	g_pages.printAdminBottom ( &p );
	int32_t bufLen = p.length();
	return g_httpServer.sendDynamicPage (s,p.getBufStart(),bufLen);
}

bool sendPageCloneColl ( TcpSocket *s , HttpRequest *r ) {
	char format = r->getReplyFormat();

	const char *coll = r->getString("c");

	if ( format == FORMAT_XML || format == FORMAT_JSON ) {
		if ( ! coll ) {
			g_errno = EBADENGINEER;
			const char *msg = "no c parm provided";
			return g_httpServer.sendErrorReply(s,g_errno,msg,NULL);
		}
		return g_httpServer.sendSuccessReply(s,format);
	}

	StackBuf<64*1024> p;

	// print standard header
	g_pages.printAdminTop ( &p , s , r );

	const char *msg = NULL;
	if ( g_errno ) msg = mstrerror(g_errno);

	if ( msg ) {
		p.safePrintf (
			  "<center>\n"
			  "<font color=red>"
			  "<b>Error cloning collection: %s. "
			  "See log file for details.</b>"
			  "</font>"
			  "</center><br>\n",msg);
	}

	// print the clone box

	p.safePrintf (
		      "<center>\n<table %s>\n"
		      "<tr class=hdrow><td colspan=2>"
		      "<center><b>Clone Collection</b></center>"
		      "</td></tr>\n",
		      TABLE_STYLE);

	p.safePrintf (
		      "<tr bgcolor=#%s>"
		      "<td><b>clone settings from this collection</b>"
		      "<br><font size=1>Copy settings FROM this "
		      "pre-existing collection into the currently "
		      "selected collection."
		      "</font></td>\n"
		      "<td><input type=text name=clonecoll size=30>"
		      "</td>"
		      "</tr>"

		      , LIGHT_BLUE
		      );

	p.safePrintf ( "</table></center><br>\n");
	// wrap up the form started by printAdminTop
	g_pages.printAdminBottom ( &p );
	int32_t bufLen = p.length();
	return g_httpServer.sendDynamicPage (s,p.getBufStart(),bufLen);
}

#endif
