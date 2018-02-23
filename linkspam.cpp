// http://www.propeciauk.co.uk/links.htm
// http://www.hendersonvillehomepro.com/FavoriteLinks/Default.aspx
// http://www.viacreme-viacream-viagra.com/health/pharmacies.htm
// are the same description for viagrapunch.com. why did they not cancel?

#include "linkspam.h"
#include "Url.h"
#include "Linkdb.h"
#include "Xml.h"
#include "matches2.h"

static bool isLinkChain ( Xml *xml, const Url *linker, const Url *linkee, int32_t linkNode,
		   const char **note ) ;

// . here's some additional things to mark it as a log page, but these
//   depend on the content of the page, not the url itself.
// . fields: string, stringLen, id, section?
// . section is "1" if the substring identifies the start of a comment
//   section, so that any links above that identifier should be 
//   consider good, and any below, should be considered bad links.
//   Otherwise, if section is 0, if the match occurs anywhere on the
//   page then all links on the page should be considered bad.
static const Needle s_needles1[] = {
	{"open.thumbshots.org"          , 0 , 0  } ,
	//{"google-ad"                    , 0 , 0  } ,
	// indicates search results page
	// this often directly precedes the comment section
	{"[trackback"                   , 1 , 1  } ,
	{"class=\"comtext"              , 8 , 1  } ,
	{"class=\"comment"              , 8 , 1  } ,
	{"class=\"coment"               , 8 , 1  } ,
	{"class=\"trackback"            , 8 , 1  } ,
	{"class=\"ping"                 , 8 , 1  } ,
	{"class=\"followup"             , 8 , 1  } ,
	{"class=\"response"             , 8 , 1  } ,
	// this can signify a blog entry, not just a comment
	//{"class=\"entry"              , 8 , 1  } ,
	// these seem to be more indicative of posted comments
	{"class=\"posted"               , 8 , 1  },
	{"id=\"posted"                  , 8 , 1  },
	{"name=\"posted"                , 8 , 1  },
	// annoying little textbox thingy
	{"class=\"shoutbox"             , 8 , 1  } ,
	{"id=\"comment"                 , 8 , 1  } ,
	{"id=\"coment"                  , 8 , 1  } ,
	{"id=\"trackback"               , 8 , 1  } ,
	{"id=\"ping"                    , 8 , 1  } ,
	{"id=\"followup"                , 8 , 1  } ,
	{"id=\"response"                , 8 , 1  } ,
	{"name=\"comment"               , 8 , 1  } ,
	{"name=\"coment"                , 8 , 1  } ,
	{"name=\"trackback"             , 8 , 1  } ,
	{"name=\"ping"                  , 8 , 1  } ,
	{"name=\"followup"              , 8 , 1  } ,
	{"name=\"response"              , 8 , 1  } ,
	// a lot of the comment boards can be identified because
	// they have a bunch of mailto links, one before each comment
	//{"href=\"mailto"                , 8 , 1 },
	//{"href=mailto"                  , 8 , 1 },
	// wikipedias
	{"div class=\"editsection"      , 10, 1  } ,
	{"action=edit"                  , 10, 1  } ,
	// message boards
	{"anonymous user"               , 10, 1  } ,
	{"anonymer user"                , 10, 1  } ,
	{"date posted"                  , 10, 1  } ,
	{"post your notice"             , 10, 1  } ,
	{"edit this page"               , 10, 1  } ,
	// edit</a><br>
	{"edit<a]br"                    , 10, 1  } ,
	// link to edit a comment
	{">edit</a"                     , 10, 1  } ,
	// these often indicate blog entries, not just comments
	//{"postedon"                     , 10, 1 },
	//{"posted by "                   , 10, 1 },
	//{"posted at "                   , 10, 1 },
	{"reply with quote"             , 9 , 0  } ,
	{">post a reply"                , 10, 0  } ,
	{"post reply"                   , 10, 0  } ,
	{"submit post"                  , 10, 0  } ,
	{">post message"                , 10, 0  } ,
	{">post a comment"              , 10, 0  } ,
	{">leave a comment"             , 10, 0  } ,
	{">post comments"               , 10, 0  } ,
	// Comments</font> (0) after each posted entry...
	//{">comments<"                 , 10, 1  } ,
	{"comments: <"                  , 10, 1  } ,
	{"comments:<"                   , 10, 1  } ,
	//{"comment:"                   , 10, 1  } ,
	{"reacties:"                    , 10, 1  } ,
	{"comentarios:"                 , 10, 1  } ,
	{"comentários:"                 , 10, 1  } ,
	{">message:"                    , 10, 0  } ,
	{">mensagem:"                   , 10, 0  } ,
	{">faca seu comentario"         , 10, 0  } ,
	{">faça seu comentário"         , 10, 0  } ,
	// comment add in german
	{">Kommentar hinzuf"            , 10, 0  } ,
	{"rate this link"               , 10, 0  } ,
	{"link submit"                  , 10, 0  } ,
	{"links directory"              , 10, 0  } ,
	{">add my comment"              , 10, 0  } ,
	// title of the text area box
	{">your comment"                , 10, 0  } ,
	{"your comment<"                , 10, 0  } ,
	{">comment by"                  , 10, 1  } ,
	{">scrivi un commento"          , 10, 0  } ,
	{">scrivi il tuo commento"      , 10, 0  } ,
	{"add comment"                  , 10, 0  } ,
	{"trackbacks for the art"       , 12, 1  } ,
	{"these trackbacks have been re", 13, 1  } ,
	{"trackback pings"              , 13, 1  } ,
	{"read the rest of this com"    , 13, 1  } ,
	// that was the opinion of ...
	{"das war die meinung von"      , 13, 1  } ,
	{"resource partner"             , 49, 0  } ,
	{"partner link"                 , 50, 0  } ,
	{"partner site"                 , 51, 0  } ,
	{"sign the guestbook"           , 43, 0  } ,
	//{"add new comment"              , 14, 0 },
	//{"add message"                  , 14, 0 },
	// tagboard software allows free submits. it has this in 
	// an html comment tag...
	{"2002 natali ardianto"         , 14, 0  } ,
	// guestbooks
	{"guestbook</title"             , 13, 0  } ,
	{"gastenboek</title"            , 13, 0  } ,
	// link management software puts a search box on there
	{"search our links"             , 14, 0  } ,
	{"find all words option"        , 14, 0  } ,
	// link exchange indicators
	{"link you want to share"       , 14, 0  } ,
	{"link trader"                  , 14, 0  } ,
	{"link exchange"                , 15, 0  } ,
	{"link partner"                 , 16, 0  } ,
	{"link xchange"                 , 17, 0  } ,
	{"link swap"                    , 18, 0  } ,
	{"links trader"                 , 19, 0  } ,
	{"links exchange"               , 20, 0  } ,
	{"links partner"                , 21, 0  } ,
	{"links xchange"                , 22, 0  } ,
	{"links swap"                   , 23, 0  } ,
	{"list your site"               , 26, 0  } ,
	{"add your web site"            , 24, 0  } ,
	{"add your website"             , 25, 0  } ,
	{"add your site"                , 26, 0  } ,
	{"add your link"                , 27, 0  } ,
	{"add your url"                 , 28, 0  } ,
	{"add site"                     , 28, 0  } ,
	// email the webmaster to have your link on this page
	{"have your link"               , 28, 0  } ,
	{"add a web site"               , 29, 0  } ,
	{"add a website"                , 30, 0  } ,
	{"add a site"                   , 31, 0  } ,
	{"add a link"                   , 32, 0  } ,
	{"add a url"                    , 33, 0  } ,
	{"adding your web site"         , 34, 0  } ,
	{"adding your website"          , 35, 0  } ,
	{"adding your site"             , 36, 0  } ,
	{"adding your link"             , 37, 0  } ,
	{"adding your url"              , 38, 0  } ,
	{"adding a web site"            , 39, 0  } ,
	{"adding a website"             , 40, 0  } ,
	{"adding a site"                , 41, 0  } ,
	{"adding a link"                , 42, 0  } ,
	{"adding a url"                 , 43, 0  } ,
	{"add url"                      , 43, 0  } ,
	{"add resource"                 , 43, 0  } ,
	{"add link"                     , 43, 0  } ,
	{"add free link"                , 43, 0  } ,
	{"addlink"                      , 43, 0  } ,
	{"suggest a site"               , 43, 0  } ,
	{"swap links"                   , 43, 0  } ,
	{"considered for addition"      , 43, 0  } ,
	{"we are not affiliated"        , 43, 0  } ,
	{"have a site to add"           , 43, 0  } ,
	{"submit your web site"         , 34, 0  } ,
	{"submit your website"          , 35, 0  } ,
	{"submit your site"             , 36, 0  } ,
	{"submit your link"             , 37, 0  } ,
	{"submit your url"              , 38, 0  } ,
	{"submit a web site"            , 39, 0  } ,
	{"submit a website"             , 40, 0  } ,
	{"submit a site"                , 41, 0  } ,
	{"submit a link"                , 42, 0  } ,
	{"submit link"                  , 42, 0  } ,
	{"submit a url"                 , 43, 0  } ,
	// . article spammers using article-emporium.com, etc.
	// . these articles get circulated into regular websites
	{"submit your article"          , 43, 0  } ,
	{"submit articles"              , 43, 0  } ,
	{"submit an article"            , 43, 0  } ,
	{"for any feedback contact"     , 43, 0  } ,
	{"for any feedback mail"        , 43, 0  } ,
	{"for any feedback email"       , 43, 0  } ,
	{"other articles that might"    , 43, 0  } ,
	{"is a freelance"               , 43, 0  } ,
	{"author is an amateur"         , 43, 0  } ,
	{"article source"               , 43, 0  } ,
	{"word count:"                  , 43, 0  } ,
	{"for additional information on", 43, 1  } ,
	{"for more information on"      , 43, 1  } ,
	{"for further assistance visit" , 43, 1  } ,
	{"article submitted on"         , 43, 0  } ,
	{"please rate this"             , 43, 0  } ,
	{"rate the article"             , 43, 0  } ,
	//{"how would you rate"         , 43, 0  } ,
	{"add rating"                   , 43, 0  } ,
	{"trade text link"              , 44, 0  } ,
	{"trade link"                   , 45, 0  } ,
	{"exchange link"                , 46, 0  } ,
	{"exchanging link"              , 47, 0  } ,
	{"reciprocal link"              , 48, 0  } ,

	// new stuff
	{">sponsors<"                   , 48, 0  } ,
	{">sponsor<"                    , 48, 0  } ,
	{">sponsored<"                  , 48, 0  } ,
	{">submit site<"                , 48, 0  } ,
	{": sponsor"                    , 48, 0  } ,
	{"/sponsor/"                    , 48, 0  } ,
	{"*sponsors*"                   , 48, 0  } ,
	{">payperpost"                  , 48, 0  } ,
	{"sponsored post"               , 48, 0  } ,
	{"sponsored flag"               , 48, 0  } ,
	{"sponsoredflag"                , 48, 0  } ,
	{"sponsored listing"            , 48, 1  } ,
	{"sponsored link"               , 48, 1  } ,
	{"post is sponsor"              , 48, 0  } ,
	{"paid post"                    , 48, 0  } ,
	{"powered by"                   , 48, 0  } , // wordpress
	{"suggest your website"         , 48, 0  } ,
	{"advertisement:"               , 48, 1  } 
};

static constexpr int32_t numNeedles1 = sizeof(s_needles1)/sizeof(Needle);

// now check outlinks on the page for these substrings
static const Needle s_needles2[] = {
	{"cyber-robotics.com" , 0 , 0  } ,
	{"cyberspacehq.com"   , 0 , 0  } ,
	{"links4trade.com"    , 0 , 0  } ,
	{"searchfeed.com"     , 0 , 0  } ,
	{"marketnex.com"      , 0 , 0  } ,
	{"partnersignup"      , 0 , 0  } ,
	{"publisher-network"  , 0 , 0  } ,
	//{"amazon.com"       , 0 , 0  } ,
	//{"dmoz.org"         , 0 , 0  } ,
	//{"dmoz.com"         , 0 , 0  } ,
	{"linksmanager"       , 0 , 0  } ,
	{"changinglinks"      , 0 , 0  } 
};

static constexpr int32_t numNeedles2 = sizeof(s_needles2)/sizeof(Needle);

//Check if a path is likely to contain uncontrolled links
//Eg. guestbooks, blog comments, link-trade, etc. There is nothing from with them per see
//but often a lot of them are unmonitored/unmoderated and link spammers insert links in them.
static bool isLinkfulPath(const char *path, size_t pathLen, const char **note) {
	if(pathLen<=1)
		return false;
	if(strncasestr(path,"guest",pathLen,5)) {
		*note = "path has guest";
		return true;
	} else if(strncasestr(path,"cgi",pathLen,3)) {
		*note = "path has cgi";
		return true;
	} else if(strncasestr(path,"gast",pathLen,4)) { // german
		*note = "path has gast";
		return true;
	} else if(strncasestr(path,"gaest",pathLen,5)) { //danish
		*note = "path has gaest";
		return true;
	} else if(strncasestr(path,"gbook",pathLen,5)) {
		*note = "path has gbook";
		return true;
	} else if(strncasestr(path,"akobook",pathLen,7)) { // vietnamese?
		*note = "path has akobook";
		return true;
	} else if(strncasestr(path,"/gb",pathLen,3)) {
		*note = "path has /gb";
		return true;
	} else if(strncasestr(path,"msg",pathLen,3 )) {
		*note = "path has msg";
		return true;
	} else if(strncasestr(path,"messag",pathLen,6)) {
		*note = "path has messag";
		return true;
	} else if(strncasestr(path,"board",pathLen,5)) {
		*note = "path has board";
		return true;
	} else if(strncasestr(path,"coment",pathLen,6)) {
		*note = "path has coment";
		return true;
	} else if(strncasestr(path,"comment",pathLen,7)) {
		*note = "path has comment";
		return true;
	} else if(strncasestr(path,"linktrader",pathLen,10)) {
		*note = "path has linktrader";
		return true;
	} else if(strncasestr(path,"tradelinks",pathLen,10)) {
		*note = "path has tradelinks";
		return true;
	} else if(strncasestr(path,"trade-links",pathLen,11)) {
		*note = "path has trade-links";
		return true;
	} else if(strncasestr(path,"linkexchange",pathLen,12)) {
		*note = "path has linkexchange";
		return true;
	} else if(strncasestr(path,"link-exchange",pathLen,13)) {
		*note = "path has link-exchange";
		return true;
	} else if(strncasestr(path,"reciprocal-link",pathLen,15)) {
		*note = "path has reciprocal-link";
		return true;
	} else if(strncasestr(path,"reciprocallink",pathLen,14)) {
		*note = "path has reciprocallink";
		return true;
	} else if(strncasestr(path,"/trackbacks/",pathLen,12)) {
		*note = "path has /trackbacks/";
		return true;
	}
	return false;
}


//Check if the document looks like a web statistics page
static bool isWebstatisticsPage(const Xml *xml) {
	// does title contain "web statistics for"?
	int32_t titleLen;
	const char *title = xml->getString("title", &titleLen);
	if(title && titleLen > 0) {
		// normalize title into buffer, remove non alnum chars
		char buf[256];
		char *dst    = buf;
		char *dstEnd = buf + 250;
		const char *src    = title;
		const char *srcEnd = title + titleLen;
		while(dst < dstEnd && src < srcEnd) {
			// remove punct
			if(is_alnum_a(*src) )
				*dst++ = to_lower_a(*src);
			src++;
		}
		*dst = '\0';
		// see if it matches some catch phrases
		bool val = false;
		if      ( strstr (buf,"webstatisticsfor"      )) val = true;
		if      ( strstr (buf,"webserverstatisticsfor")) val = true;
		else if ( strstr (buf,"usagestatisticsfor"    )) val = true;
		else if ( strstr (buf,"siteusageby"           )) val = true;
		else if ( strstr (buf,"surfstatsloganal"      )) val = true;
		else if ( strstr (buf,"webstarterhelpstats"   )) val = true;
		else if ( strstr (buf,"sitestatistics"        )) val = true;
		return val;
	}
	return false;
}


// . we set the bit in linkdb for a doc if this returns true
// . it precludes a doc from voting if its bits is set in linkdb
// . this saves resources
// . the isLinkSpam() function is used when we have the linkee url
// . note is only set if the whole doc can not vote for some reason
// . otherwise, each outlink in "links" is assigned a "note" to indicate if 
//   the outlink is a spam link or not
// . returns true on success, false on error
bool setLinkSpam ( int32_t       ip                 ,
		   const Url       *linker             ,
		   int32_t       siteNumInlinks     ,
		   Xml       *xml                ,
		   Links     *links              ,
		   bool       isContentTruncated ) {
	// if the doc got truncated we may be missing valuable identifiers
	// that identify the doc as a guestbook or something
	if ( isContentTruncated ) {
		links->setAllSpamBits("doc too big");
		return true;
	}
	// get linker quality
	//int32_t q = tr->getDocQuality();
	// do not allow .info or .biz to vote ever for now
	const char *tld    = linker->getTLD();
	int32_t  tldLen = linker->getTLDLen();
	if ( tldLen == 4 && strncmp ( tld, "info" , tldLen) == 0 && //q < 55 )
	     siteNumInlinks < 20 ) {
		links->setAllSpamBits("low quality .info linker");
		return true;
	}
	if ( tldLen == 3 && strncmp ( tld, "biz" , tldLen) == 0 && //q < 55 )
	     siteNumInlinks < 20 ) {
		links->setAllSpamBits("low quality .biz linker");
		return true;
	}

	// guestbook in hostname - domain?
	const char *hd  = linker->getHost();
	const char *hd2 = linker->getDomain();
	int32_t  hdlen = hd2 - hd;
	if ( hd && hd2 && hdlen < 30 ) {
		bool hasIt = false;
		if ( strnstr ( hd , "guestbook", hdlen ) ) hasIt = true;
		if ( hasIt ) {
			links->setAllSpamBits("guestbook in hostname");
			return true;
		}
	}

	// do not allow any cgi url to vote
	if ( linker->isCgi() ) {
		links->setAllSpamBits("path is cgi");
		return true;
	}

	// if the page has just one rel=nofollow tag then we know they
	// are not a guestbook
	//if ( links->hasRelNoFollow() ) plen = 0;
	const char *note = NULL;
	if(isLinkfulPath(linker->getPath(),linker->getPathLen(),&note)) {
		links->setAllSpamBits(note);
		return true;
	}

	// does title contain "web statistics for"?
	if(isWebstatisticsPage(xml)) {
		links->setAllSpamBits("stats page");
		return true;
	}

	/////////////////////////////////////////////////////
	//
	// check content for certain keywords and phrases
	//
	/////////////////////////////////////////////////////

	const char *haystack     = xml->getContent();
	int32_t  haystackSize = xml->getContentLen();

	// do not call them "bad links" if our link occurs before any
	// comment section. our link's position therefore needs to be known,
	// that is why we pass in linkPos. 
	// "n" is the number it matches.
	NeedleMatch needleMatches1[numNeedles1];

	bool hadPreMatch;
	getMatches2(s_needles1, needleMatches1, numNeedles1, haystack, haystackSize, NULL, &hadPreMatch);

	// see if we got a hit
	char *minPtr = NULL;
	note = NULL;
	for ( int32_t i = 0 ; i < numNeedles1 ; i++ ) {
		// open.thumbshots.org needs multiple counts
		if ( i == 0 && needleMatches1[i].m_count < 5 ) continue;
		// skip if no matches on this string
		if ( needleMatches1[i].m_count <= 0  ) continue;
		// ok, if it had its section bit set to 0 that means the
		// whole page is link spam!
		if ( s_needles1[i].m_isSection == 0 ) {
			links->setAllSpamBits(s_needles1[i].m_string );
			return true;
		}
		// get the char ptr
		char *ptr = needleMatches1[i].m_firstMatch;
		// set to the min
		if ( ! minPtr || ptr < minPtr ) { 
			note   = s_needles1[i].m_string; 
			minPtr = ptr;
		}
	}

	// convert the char ptr into a link node following it
	int32_t aa = 0;
	if ( minPtr ) aa = links->getNumLinks();
	int32_t mini = -1;
	for ( int32_t i = 0 ; i < aa ; i++ ) {
		// get the link's char ptr into the content
		int32_t  linkNode = links->getNodeNum(i);
		char *linkPos  = NULL;
		if ( linkNode >= 0 ) linkPos = xml->getNode ( linkNode );
		// now we can compare, if BEFORE this comment section
		// indicating tag, we are NOT link spam, so continue
		if ( linkPos < minPtr ) continue;
		// otherwise, we are the first, stop.
		mini = i;
		break;
	}

	// now count all the links BELOW this match as link spam
	// but everyone else is ok!
	if ( minPtr && mini >= 0 ) 
		links->setSpamBits ( note , mini );

	// now check outlinks on the page for these substrings
	haystack     = links->getLinkBuf();
	haystackSize = links->getLinkBufLen();
	NeedleMatch needleMatches2[numNeedles2];
	getMatches2(s_needles2, needleMatches2, numNeedles2, haystack, haystackSize, NULL, NULL);

	// see if we got a hit
	for ( int32_t i = 0 ; i < numNeedles2 ; i++ ) {
		// skip if did not match
		if ( needleMatches2[i].m_count <= 0 ) continue;
		// the whole doc is considered link spam
		links->setAllSpamBits(s_needles2[i].m_string);
		return true;
	}

	//skiplinks:
	// check for certain post tag, indicative of a comment-friendly blog
	// <form method=post ... action=*comments*cgi-bin>
	// <form method="post" 
	//       action="http://www.mydomain.com/cgi-bin/mt-comments.cgi" 
	//       name="comments_form" ...>
	// <form method=POST 
	//  action="http://peaceaction.org/wboard/wwwboard.cgi">
	int32_t nn = xml->getNumNodes();
	bool gotTextArea = false;
	bool gotSubmit   = false;
	for ( int32_t i=0; i < nn ; i++ ) {
		// <textarea> tags are bad... but only if we have not
		// matched "track" or whatever from above... check for that
		// if you uncommment this... otherwise you disable all blogs!
		// Only do this check if we did match a comment related phrase
		// in s_needles1[] BUT it was BEFORE our outlink. That 
		// basically means that we do *not* recognize the format of 
		// the comment page and so therefore need to be more 
		// restrictive about allowing this page to vote.
		if ( ! hadPreMatch ) {
			// is it a <textarea> tag?
			if ( xml->getNodeId ( i ) == TAG_TEXTAREA ) 
				gotTextArea = true;
			// is it an <input> tag?
			int32_t len = 0;
			if ( xml->getNodeId ( i ) == TAG_INPUT &&
			     xml->getString(i,"submit",&len)) gotSubmit = true;
		}

		if ( xml->getNodeId ( i ) != TAG_FORM ) continue;
			
		// get the method field of this base tag
		int32_t  slen;
		char *s = (char *) xml->getString(i,"method",&slen);
		// if not thee, skip it
		if ( ! s || slen <= 0 ) continue;
		// get the action url
		s = (char *) xml->getString(i,"action",&slen);
		if ( ! s || slen <= 0 ) continue;
		char c = s[slen];
		s[slen]='\0';
		bool val = false;
		// this is a bit too strong, but i'ev seen an action of
		// "cgi-bin/mt-leaveone.cgi" so we can't rely on "mt-comment"
		if      ( strstr ( s , "comment" ) ) val = true;
		else if ( strstr ( s , "/MT/" ) ) val = true;
		else if ( strstr ( s , "/mt/" ) ) val = true;
		// they can have these search boxes though
		if ( val && strstr ( s , "/mt/mt-search" ) ) val = false;
		s[slen] = c;
		if ( val ) {
			links->setAllSpamBits("post page");
			return true;
		}
	}

	if ( gotTextArea && gotSubmit ) {
		links->setAllSpamBits("textarea tag");
		return true;
	}

	// edu, gov, etc. can have link chains
	if ( tldLen >= 3 && strncmp ( tld, "edu" , 3) == 0 ) return true;
	if ( tldLen >= 3 && strncmp ( tld, "gov" , 3) == 0 ) return true;

	// if linker is naughty, he cannot vote... how did he make it in?
	if ( linker->isAdult() ) {
		links->setAllSpamBits("linker is sporny");
		return true;
	}

	// . if they link to any adult site, consider them link spam
	// . just consider a 100 link radius around linkNode
	int32_t nl = links->getNumLinks();
	for ( int32_t i = 0 ; i < nl ; i++ ) {
		// skip if this link is internal, we will add it to linkdb
		// anyway... this will save us some processing time
		if ( links->isInternalDom(i) ) continue;
		// otherwise, normalize it...
		Url uu;
		uu.set( links->getLinkPtr( i ), links->getLinkLen( i ) );

		// . is it near sporny links? (naughty domains or lotsa -'s)
		// . if we are in a list of ads, chances are good the true
		//   nature of the ads will emerge...
		if ( uu.isAdult() ) {
			links->setAllSpamBits("has sporny outlinks");
			log(LOG_DEBUG,"build: %s has sporny outlinks.",
			    uu.getUrl());
			return true;
		}

		// check if this url is a link chain
		//if ( q >= 60 ) continue;
		if ( siteNumInlinks >= 50 ) continue;
		const char *np = NULL;
		// get the xml node of link #i
		int32_t xmlNode = links->getNodeNum ( i );
		if ( isLinkChain ( xml , linker, &uu, xmlNode, &np ))
			links->setSpamBit ( np , i );
	}
	return true;
}



bool isLinkSpam ( const Url *linker,
		  int32_t ip ,
		  int32_t siteNumInlinks ,
		  //TitleRec *tr, 
		  Xml *xml, 
		  Links *links ,
		  int32_t maxDocLen , 
		  const char **note ,
		  const Url *linkee ,
		  // node position of the linkee in the linker's content
		  int32_t  linkNode ) {
	// same host linkers can be link spam (TODO: make same ip block)
	// because we only allow up to 10 to vote as a single voter
	if ( linkee ) {
		const char *h1    = linkee->getHost();
		int32_t  h1len = linkee->getHostLen();
		const char *h2    = linker->getHost();
		int32_t h2len = linker->getHostLen();
		//if ( tr ) h2    = tr->getUrl()->getHost();
		//if ( tr ) h2len = tr->getUrl()->getHostLen();
		if ( h1len == h2len && strncmp ( h1 , h2 , h1len ) == 0 ) 
			return false;
	}
	// do not allow .info or .biz to vote ever for now
	const char *tld    = linker->getTLD();
	int32_t  tldLen = linker->getTLDLen();
	if ( tldLen == 4 && strncmp ( tld, "info" , tldLen) == 0 ) {
		*note = ".info tld";
		return true;
	}
	if ( tldLen == 3 && strncmp ( tld, "biz" , tldLen) == 0 ) {
		*note = ".biz tld";
		return true;
	}

	// i saw a german doc get its textarea cut out because of this, so
	// we need this here
	if ( xml && xml->getContentLen() > maxDocLen ) {
		*note ="doc too big";
		return true; 
	}

	// guestbook in hostname - domain?
	const char *hd  = linker->getHost();
	const char *hd2 = linker->getDomain();
	int32_t  hdlen = hd2 - hd;
	if ( hd && hd2 && hdlen < 30 ) {
		bool hasIt = false;
		if ( strnstr ( hd , "guestbook", hdlen ) ) hasIt = true;
		if ( hasIt ) { 
			*note = "guestbook in hostname"; 
			return true; 
		}
	}

	// do not allow any cgi url to vote
	if ( linker->isCgi() ) { *note = "path is cgi"; return true; }

	// if the page has just one rel=nofollow tag then we know they
	// are not a guestbook
	//if ( links->hasRelNoFollow() ) plen = 0;
	if(isLinkfulPath(linker->getPath(),linker->getPathLen(),note))
		return true;

	if( !xml ) {
		return false;
	}

	// does title contain "web statistics for"?
	if(isWebstatisticsPage(xml)) {
		*note = "stats page";
		return true;
	}

	/////////////////////////////////////////////////////
	//
	// check content for certain keywords and phrases
	//
	/////////////////////////////////////////////////////

	const char *haystack     = xml->getContent();
	int32_t  haystackSize = xml->getContentLen();

	char *linkPos = NULL;
	if ( linkNode >= 0 ) linkPos = xml->getNode ( linkNode );

	// do not call them "bad links" if our link occurs before any
	// comment section. our link's position therefore needs to be known,
	// that is why we pass in linkPos.
	NeedleMatch needleMatches1[numNeedles1];
	bool hadPreMatch;
	getMatches2(s_needles1, needleMatches1, numNeedles1, haystack, haystackSize, linkPos, &hadPreMatch);

	// see if we got a hit
	for ( int32_t i = 0 ; i < numNeedles1 ; i++ ) {
		int32_t need = 1;
		// open.thumbshots.org needs multiple counts
		if ( i == 0 ) need = 5;
		if ( needleMatches1[i].m_count < need ) continue;
		*note = s_needles1[i].m_string;
		return true;
	}

	// now check outlinks on the page for these substrings
	haystack     = links->getLinkBuf();
	haystackSize = links->getLinkBufLen();
	NeedleMatch needleMatches2[numNeedles2];
	getMatches2(s_needles2, needleMatches2, numNeedles2, haystack, haystackSize, NULL, NULL);

	// see if we got a hit
	for ( int32_t i = 0 ; i < numNeedles2 ; i++ ) {
		int32_t need = 1;
		// open.thumbshots.org needs multiple counts
		//if ( i == 9 ) need = 5;
		if ( needleMatches2[i].m_count < need ) continue;
		*note = s_needles2[i].m_string;
		return true;
	}

	//skiplinks:
	// check for certain post tag, indicative of a comment-friendly blog
	// <form method=post ... action=*comments*cgi-bin>
	// <form method="post" 
	//       action="http://www.mydomain.com/cgi-bin/mt-comments.cgi" 
	//       name="comments_form" ...>
	// <form method=POST 
	//  action="http://peaceaction.org/wboard/wwwboard.cgi">
	int32_t nn = xml->getNumNodes();
	bool gotTextArea = false;
	bool gotSubmit   = false;
	for ( int32_t i=0; i < nn ; i++ ) {
		// <textarea> tags are bad... but only if we have not
		// matched "track" or whatever from above... check for that
		// if you uncommment this... otherwise you disable all blogs!
		// Only do this check if we did match a comment related phrase
		// in s_needles1[] BUT it was BEFORE our outlink. That 
		// basically means that we do *not* recognize the format of 
		// the comment page and so therefore need to be more 
		// restrictive about allowing this page to vote.
		if ( ! hadPreMatch ) {
			// is it a <textarea> tag?
			if ( xml->getNodeId ( i ) == TAG_TEXTAREA ) 
				gotTextArea = true;
			// is it an <input> tag?
			int32_t len = 0;
			if ( xml->getNodeId ( i ) == TAG_INPUT &&
			     xml->getString(i,"submit",&len)) gotSubmit = true;
		}

		if ( xml->getNodeId ( i ) != TAG_FORM ) continue;
			
		// get the method field of this base tag
		int32_t  slen;
		char *s = (char *) xml->getString(i,"method",&slen);
		// if not thee, skip it
		if ( ! s || slen <= 0 ) continue;
		// get the action url
		s = (char *) xml->getString(i,"action",&slen);
		if ( ! s || slen <= 0 ) continue;
		char c = s[slen];
		s[slen]='\0';
		bool val = false;
		// this is a bit too strong, but i'ev seen an action of
		// "cgi-bin/mt-leaveone.cgi" so we can't rely on "mt-comment"
		if      ( strstr ( s , "comment" ) ) val = true;
		else if ( strstr ( s , "/MT/" ) ) val = true;
		else if ( strstr ( s , "/mt/" ) ) val = true;
		// they can have these search boxes though
		if ( val && strstr ( s , "/mt/mt-search" ) ) val = false;
		s[slen] = c;
		if ( val ) { *note = "post page"; return true; }
	}

	if ( gotTextArea && gotSubmit ) {
		*note = "textarea tag";
		return true;
	}

	// edu, gov, etc. can have link chains
	if ( tldLen >= 3 && strncmp ( tld, "edu" , 3) == 0 ) return false;
	if ( tldLen >= 3 && strncmp ( tld, "gov" , 3) == 0 ) return false;

	// if linker is naughty, he cannot vote
	if ( linker->isAdult() )
		return true;

	// if being called from PageTitledb.cpp for displaying a titlerec, 
	// then do not call this, because no linkee is provided in that case.
	if ( !linkee ) {
		*note = "linkee not found";
		return false;//true;
	}

	// . if they link to any adult site, consider them link spam
	// . just consider a 100 link radius around linkNode
	int32_t nl = links->getNumLinks();

	// init these before the loop
	int32_t  hlen  = linkee->getHostLen();
	const char *host  = linkee->getHost();
	const char *uu    = linkee->getUrl();
	const char *uuend = host + hlen;
	int32_t  uulen = uuend - uu;
	int32_t  x     = linkNode;
 loop:

	// return true right away if it is a link chain
	if ( siteNumInlinks < 1000 && 
	     isLinkChain ( xml , linker, linkee , x , note ) ) 
		return true;

	// if no domain, that's it
	if ( ! uu || uulen <= 0 ) return false;

	// . see if this domain is linked to in other areas of the document.
	// . if any of those areas are not link chains, then assume we are
	//   not a link chain
	for ( x++ ; x < nl ; x++ ) {
		char *link = links->getLinkPtr(x);
		int32_t  linkLen = links->getLinkLen(x);
		if ( ! link          ) continue;
		if ( linkLen <= 0    ) continue;
		if ( linkLen > uulen ) continue;
		if ( strncmp ( link , uu , uulen ) != 0 ) continue;
		// got a match, is it a link chain? if not, them we are not
		goto loop;
	}

	return false;

}

// Criteria for being a link chain:
//
// 1. the "linkee" is in a chain of outlinks to external domains
// 2. all outlinks to the same hostname as "linkee" are in link chains
// 3. no plain text is present between "linkee" and one of the other
//    outlinks in the chain
// 4. this might hurt blogrolls, and resource pages, but such links
//    are kind of low quality anyway.
static bool isLinkChain ( Xml *xml, const Url *linker, const Url *linkee, int32_t linkNode, const char **note ) {

	//log(LOG_DEBUG,"build: doing %s",linker->m_url);

	// if the linkee is internal (by domain) then not a link chain
	if ( linkee->getDomainLen() == linker->getDomainLen() &&
	     strncmp ( linkee->getDomain() , linker->getDomain(),linkee->getDomainLen())==0)
		return false;

	const char *linkPos = NULL;
	if ( linkNode >= 0 ) linkPos = xml->getNode ( linkNode );

	// did we have text to the left/right of this link and after/before
	// the neighboring link? assume not.
	bool leftText  = false;
	bool rightText = false;

	// the links on the left and right
	Url  leftUrl;
	Url  rightUrl;
	bool leftMalformed  = false;
	bool rightMalformed = false;
	// these do not have constructors so we must reset them
	leftUrl.reset();
	rightUrl.reset();
	int32_t i ;
	// . see if we are alone in a table or not
	// . table must occur before/after our left/right neighbor link
	bool tableLeft   = false;
	bool tableRight  = false;

	// going backwards from linkNode we are not in a link
	bool inLink = false;

	// get the start of an anchor tag on our immediate left
	for ( i = linkNode - 1 ; i >= 0 ; i-- ) {
		// do not look too far
		if ( linkPos - xml->getNode(i) >= 1500 ) break;
		if ( linkNode - i >= 90                ) break;
		// NOTE: if you add more tags to this list, then also add
		// to Vector::setPairHashes() as well
		// stop at <title> or </title> tags
		if ( xml->getNodeId(i) == TAG_TITLE ) break;
		// stop at <ul> or </ul> tags
		// no, otherwise, these lists are always "link chain left"
		//if ( xml->getNodeId(i) == TAG_UL ) break;
		// stop at <table> or </table> tags
		if ( xml->getNodeId(i) == TAG_TABLE ) {
			if ( ! xml->isBackTag(i) ) tableLeft = true;
			break;
		}

		// check for *plain* text
		if ( ! inLink && xml->getNodeId(i) == TAG_TEXTNODE ) {
			// get the node as a string
			char *p    = xml->getNode(i);
			char *pend = p + xml->getNodeLen(i);
			// check for elipsis, that is a sign that we are a serp
			for ( char *s = p ; s+2 < pend ; s++ ) {
				//if ( is_alnum(*s) ) break;
				if ( *s != '.' ) continue;
				s++;
				if ( *s != '.' ) continue;
				s++;
				if ( *s != '.' ) continue;
				// ok, got it
				*note = "search result right";
				return true;
			}
			// if we already got text, but searching still for ...
			if ( leftText ) continue;
			// does it have alnum
			if ( ! has_alpha_utf8 ( p , pend ) ) continue;
			leftText = true; 
			// do not break yet, cont search for ellipsis!
		}

		// keep chugging if not an anchor tag, <a> or </a>
		if ( xml->getNodeId(i) != TAG_A ) continue;
		// if we are </a> then we are now in a link since we are moving
		// backwards
		if ( xml->isBackTag(i) ) { inLink = true; continue; }
		// if we hit a forward tag and inLink was false... we had
		// no corresponding back tag, so disconsider any text
		if ( ! inLink ) rightText = false;
		// no longer in an <a> tag
		inLink = false;

		// ok, get the url from this anchor tag
		int32_t  ulen = 0;
		char *u = (char *) xml->getString ( i, "href", &ulen );
		// if we did not get one, that means it could have been
		// malformed... like the href had a quote right b4 it
		if ( ulen == 0 ) leftMalformed = true;
		// normalize
		if ( ulen > 0 )
			leftUrl.set( linker, u, ulen );
		// . if NOT from the same domain, break out, otherwise continue
		// . this helps us find the <table> tag in ad tables with 
		//   multiple links to the same domain
		// . this helps us accept a list of links to the same domain if
		//   there is left/right text, like the guy that had a list
		//   to 3 different gigablast.com links in a row with no
		//   text in between
		if ( leftUrl.getDomainLen() != linkee->getDomainLen()  ) 
			break;
		if( strncmp(leftUrl.getDomain(), linkee->getDomain(), linkee->getDomainLen()) != 0 ) 
			break;
	}

	// we start off in link text, since linkNode is an <a> tag
	inLink = true;
	// now loop through all the nodes after us
	for ( i = linkNode + 1 ; i < xml->getNumNodes() ; i++ ) {
		// stop if we've gone too far
		if ( xml->getNode(i) - linkPos >= 1580 ) break;
		if ( i - linkNode >= 95                ) break;
		// stop at <title> or </title> tags
		if ( xml->getNodeId(i) == TAG_TITLE ) break;
		// stop at <table> or </table> tags
		if ( xml->getNodeId(i) == TAG_TABLE ) {
			// note it for table ads
			if ( xml->isBackTag(i) ) tableRight = true;
			break;
		}

		// check for *plain* text
		if ( ! inLink && xml->getNodeId(i) == TAG_TEXTNODE ) {
			// get the node as a string
			char *p    = xml->getNode(i);
			char *pend = p + xml->getNodeLen(i);
			// check for elipsis, that is a sign that we are a serp
			for ( char *s = p ; s+2 < pend ; s++ ) {
				//if ( is_alnum(*s) ) break;
				if ( *s != '.' ) continue;
				s++;
				if ( *s != '.' ) continue;
				s++;
				if ( *s != '.' ) continue;
				// ok, got it
				*note = "search result right";
				return true;
			}
			// if we already got text, but searching still for ...
			if ( rightText ) continue;
			// does it have alnum
			if ( ! has_alpha_utf8 ( p , pend ) ) continue;
			rightText = true; 
			// do not break yet, cont search for ellipsis!
		}

		// keep chugging if not an anchor tag, <a> or </a>
		if ( xml->getNodeId(i) != TAG_A ) continue;
		// skip if not a forward tag
		if ( xml->isBackTag(i) ) { inLink = false; continue; }
		// we are now in a link
		inLink = true;
		// stop text here
		//stopTextScan = i;
		// ok, get the url
		int32_t  ulen = 0;
		char *u = (char *) xml->getString ( i, "href", &ulen );
		// if we did not get one, that means it could have been
		// malformed... like the href had a quote right b4 it
		if ( ulen == 0 ) rightMalformed = true;
		// normalize
		if ( ulen > 0 )
			rightUrl.set( linker, u, ulen );
		// . if NOT from the same domain, break out, otherwise continue
		// . this helps us find the <table> tag in ad tables with 
		//   multiple links to the same domain
		// . this helps us accept a list of links to the same domain if
		//   there is left/right text, like the guy that had a list
		//   to 3 different gigablast.com links in a row with no
		//   text in between
		if ( rightUrl.getDomainLen() != linkee->getDomainLen() ) 
			break;
		if ( strncmp(rightUrl.getDomain(), linkee->getDomain(), linkee->getDomainLen()) != 0 ) 
			break;
	}

	if ( tableLeft && tableRight ) {
		*note = "ad table";
		return true;
	}

	// if we had text on both sides of us, we are not a link chain
	if ( leftText && rightText ) return false;

	if      ( ! leftText  && rightText ) *note = "link chain left";
	else if ( ! rightText && leftText  ) *note = "link chain right";
	else                                 *note = "link chain middle";

	return true;
}
