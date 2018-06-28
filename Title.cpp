#include "gb-include.h"

#include "Query.h"
#include "Title.h"
#include "Sections.h"
#include "Pops.h"
#include "Pos.h"
#include "Matches.h"
#include "HashTable.h"
#include "HttpMime.h"
#include "Linkdb.h"
#include "TitleSummaryCodepointFilter.h"
#include "StopWords.h"
#include "Process.h"
#include "Conf.h"
#include "Xml.h"
#include "Mem.h"
#include "GbUtil.h"


#ifdef _VALGRIND_
#include <valgrind/memcheck.h>
#endif

// test urls
// http://www.thehindu.com/2009/01/05/stories/2009010555661000.htm
// http://xbox360.ign.com/objects/142/14260912.html
// http://www.scmp.com/portal/site/SCMP/menuitem.2c913216495213d5df646910cba0a0a0?vgnextoid=edeb63a0191ae110VgnVCM100000360a0a0aRCRD&vgnextfmt=teaser&ss=Markets&s=Business
// http://www.legacy.com/shelbystar/Obituaries.asp?Page=LifeStory&PersonId=122245831
// http://web.me.com/bluestocking_bb/The_Bluestocking_Guide/Book_Reviews/Entries/2009/1/6_Hamlet.html
// http://larvatusprodeo.net/2009/01/07/partisanship-politics-and-participation/
// http://content-uk.cricinfo.com/ausvrsa2008_09/engine/current/match/351682.html
// www4.gsb.columbia.edu/cbs-directory/detail/6335554/Schoenberg 
// http://www.washingtonpost.com/wp-dyn/content/article/2008/10/29/AR2008102901960.html
// http://www.w3.org/2008/12/wcag20-pressrelease.html
// http://www.usnews.com/articles/business/best-careers/2008/12/11/best-careers-2009-librarian.html
// http://www.verysmartbrothas.com/2008/12/09/
// http://www.slashgear.com/new-palm-nova-handset-to-have-touchscreen-and-qwerty-keyboard-0428710/

// still bad
// http://66.231.188.171:8500/search?k3j=668866&c=main&n=20&ldays=1&q=url%3Ahttp%3A%2F%2Fmichellemalkin.com%2F2008%2F12%2F29%2Fgag-worthy%2F selects
// "gag-worthy" instead of 
// "Gag-worthy: Bipartisan indignance over .Barack the Magic Negro. parody"
// http://www.1800pocketpc.com/2009/01/09/web-video-downloader-00160-download-videos-from-youtube-on-your-pocket-pc.html : need to fix the numbers in the
// path somehow so similarity is higher


Title::Title() {
	m_title[0] = '\0';
	m_titleLen = 0;
	m_titleTagStart = -1;
	m_titleTagEnd   = -1;
	
	m_maxTitleLen = 0;
}

Title::~Title() {
}

void Title::reset() {
	m_title[0] = '\0';
	m_titleLen = 0;
	m_titleTagStart = -1;
	m_titleTagEnd   = -1;
}

void Title::setTitle(const std::string &title) {
	m_titleLen = title.copy(m_title, MAX_TITLE_LEN);
	m_title[m_titleLen] = '\0';
}

bool Title::setTitleFromTags( Xml *xml, int32_t maxTitleLen, uint8_t contentType ) {
	/// @todo cater for CT_DOC (when antiword is replaced)
	// only allow html & pdf documents for now
	if ( contentType != CT_HTML && contentType != CT_PDF ) {
		return false;
	}

	/// @todo ALC configurable minTitleLen so we can tweak this as needed
	const int minTitleLen = 3;

	// meta property = "og:title"
	if ( contentType == CT_HTML &&
	     xml->getTagContent("property", "og:title", m_title, MAX_TITLE_LEN, minTitleLen, maxTitleLen, &m_titleLen, true, TAG_META) ) {
		logDebug(g_conf.m_logDebugTitle, "title: generated from meta property og:title. title='%.*s'", m_titleLen, m_title );

		return true;
	}

	// meta name = "title"
	if ( contentType == CT_HTML &&
	     xml->getTagContent("name", "title", m_title, MAX_TITLE_LEN, minTitleLen, maxTitleLen, &m_titleLen, true, TAG_META) ) {
		logDebug(g_conf.m_logDebugTitle, "title: generated from meta property title. title='%.*s'", m_titleLen, m_title );

		return true;
	}

	// title
	if ( xml->getTagContent( "", "", m_title, MAX_TITLE_LEN, minTitleLen, maxTitleLen, &m_titleLen, true, TAG_TITLE ) ) {
		if ( contentType == CT_PDF ) {
			// when using pdftohtml, the title tag is the filename when PDF property does not have title tag
			const char *result = strnstr( m_title, "/in.", m_titleLen );
			if ( result != NULL ) {
				char *endp = NULL;

				// do some further verification to avoid screwing up title
				if ( ( strtoll( result + 4, &endp, 10 ) > 0 ) && ( endp == m_title + m_titleLen ) ) {
					m_title[0] = '\0';
					m_titleLen = 0;
					return false;
				}
			}
		}

		logDebug(g_conf.m_logDebugTitle, "title: generated from title tag. title='%.*s'", m_titleLen, m_title );

		return true;
	}

	logDebug(g_conf.m_logDebugTitle, "title: unable to generate title from meta/title tags");

	return false;
}

// types of titles. indicates where they came from.
enum class title_source_t {
	TT_LINKTEXTLOCAL  = 1,
	TT_LINKTEXTREMOTE,
	TT_RSSITEMLOCAL,
	TT_RSSITEMREMOTE,
	TT_BOLDTAG,
	TT_HTAG,
	TT_TITLETAG,
	TT_FIRSTLINE,
	TT_DIVTAG,
	TT_FONTTAG,
	TT_ATAG,
	TT_TDTAG,
	TT_PTAG,
	TT_URLPATH,
	TT_TITLEATT,
};
#define MAX_TIT_CANDIDATES 100

static bool isAlpha(const TokenRange &token) {
	if(!token.is_alfanum)
		return false;
	if(is_ascii_digit_string(token.token_start,token.token_end()))
		return true;
	return false;
}


// does word qualify as a subtitle delimeter?
static bool isWordQualified(const TokenRange &token) {
	// must be punct word
	if ( token.is_alfanum ) {
		return false;
	}

	// scan the chars
	unsigned x;
	for ( x = 0; x < token.token_len; x++ ) {
		if ( token.token_start[x] == ' ' ) {
			continue;
		}
		break;
	}

	// does it qualify as a subtitle delimeter?
	bool qualified = false;
	if ( x < token.token_len ) {
		qualified = true;
	}

	// fix amazon.com from splitting on period
	if ( token.token_len == 1 ) {
		qualified = false;
	}

	return qualified;
}


// returns false and sets g_errno on error
bool Title::setTitle ( Xml *xml, const TokenizerResult *tr, int32_t maxTitleLen, const Query *query,
                       LinkInfo *linkInfo, const Url *firstUrl, const char *filteredRootTitleBuf, int32_t filteredRootTitleBufSize,
                       uint8_t contentType, uint8_t langId ) {
	// make Msg20.cpp faster if it is just has
	// Msg20Request::m_setForLinkInfo set to true, no need to extricate a title.
	if ( maxTitleLen <= 0 ) {
		return true;
	}

	m_maxTitleLen = maxTitleLen;

	// if this is too big the "first line" algo can be huge!!!
	// and really slow everything way down with a huge title candidate
	int32_t maxTitleWords = 128;

	// assume no title
	reset();

	int32_t NW = tr->size();

	//
	// now get all the candidates
	//

	// . allow up to 100 title CANDIDATES
	// . "as" is the word # of the first word in the candidate
	// . "bs" is the word # of the last word IN the candidate PLUS ONE
	int32_t n = 0;
	int32_t as[MAX_TIT_CANDIDATES];
	int32_t bs[MAX_TIT_CANDIDATES];
	float scores[MAX_TIT_CANDIDATES];
	const TokenizerResult *cptrs[MAX_TIT_CANDIDATES];
	title_source_t types[MAX_TIT_CANDIDATES];
	int32_t parent[MAX_TIT_CANDIDATES];

	// record the scoring algos effects
	float  baseScore        [MAX_TIT_CANDIDATES];
	float  noCapsBoost      [MAX_TIT_CANDIDATES];
	float  qtermsBoost      [MAX_TIT_CANDIDATES];
	float  inCommonCandBoost[MAX_TIT_CANDIDATES];

	// reset these
	for ( int32_t i = 0 ; i < MAX_TIT_CANDIDATES ; i++ ) {
		// assume no parent
		parent[i] = -1;
	}

	// xml and words class for each link info, rss item
	Xml   tx[MAX_TIT_CANDIDATES];
	TokenizerResult ttr[MAX_TIT_CANDIDATES];
	int32_t  ti = 0;

	// restrict how many link texts and rss blobs we check for titles
	// because title recs like www.google.com have hundreds and can
	// really slow things down to like 50ms for title generation
	int32_t kcount = 0;
	int32_t rcount = 0;

	//int64_t x = gettimeofdayInMilliseconds();

	// . get every link text
	// . TODO: repeat for linkInfo2, the imported link text
	for ( Inlink *k = NULL; linkInfo && (k = linkInfo->getNextInlink(k)) ; ) {
		// fast skip check for link text
		if ( k->size_linkText >= 3 && ++kcount >= 20 ) continue;
		// fast skip check for rss item
		if ( k->size_rssItem > 10 && ++rcount >= 20 ) continue;

		// set Url
		Url u;
		u.set( k->getUrl(), k->size_urlBuf );

		// is it the same host as us?
		bool sh = true;

		// skip if not from same host and should be
		if ( firstUrl->getHostLen() != u.getHostLen() ) {
			sh = false;
		}

		// skip if not from same host and should be
		if ( strncmp( firstUrl->getHost(), u.getHost(), u.getHostLen()) != 0 ) {
			sh = false;
		}

		// get the link text
		if ( k->size_linkText >= 3 ) {
			char *p    = k->getLinkText();
			int32_t  plen = k->size_linkText - 1;
			if ( ! verifyUtf8 ( p , plen ) ) {
				log("title: set4 bad link text from url=%s", k->getUrl());
				continue;
			}

			// now the words.
			plain_tokenizer_phase_1(k->getLinkText(), k->size_linkText - 1, &(ttr[ti]));
			calculate_tokens_hashes(&(ttr[ti]));

			// set the bookends, it is the whole thing
			cptrs   [n] = &ttr[ti];
			as      [n] = 0;
			bs      [n] = ttr[ti].size();
			// score higher if same host
			if ( sh ) scores[n] = 1.05;
			// do not count so high if remote!
			else      scores[n] = 0.80;
			// set the type
			if ( sh ) types [n] = title_source_t::TT_LINKTEXTLOCAL;
			else      types [n] = title_source_t::TT_LINKTEXTREMOTE;
			// another candidate
			n++;
			// use xml and words
			ti++;
			// break out if too many already. save some for below.
			if ( n + 30 >= MAX_TIT_CANDIDATES ) break;
		}
		// get the rss item
		if ( k->size_rssItem <= 10 ) continue;
		// . returns false and sets g_errno on error
		if ( ! k->setXmlFromRSS ( &tx[ti] ) ) return false;
		// get the word range
		int32_t tslen;
		bool isHtmlEnc;
		char *ts = tx[ti].getRSSTitle ( &tslen , &isHtmlEnc );
		// skip if not in the rss
		if ( ! ts ) continue;
		// skip if empty
		if ( tslen <= 0 ) continue;
		// now set words to that
		plain_tokenizer_phase_1(ts, tslen, &(ttr[ti]));
		calculate_tokens_hashes(&(ttr[ti]));

		// point to that
		cptrs   [n] = &ttr[ti];
		as      [n] = 0;
		bs      [n] = ttr[ti].size();
		// increment since we are using it
		ti++;
		// base score for rss title
		if ( sh ) scores[n] = 5.0;
		// if not same host, treat like link text
		else      scores[n] = 2.0;
		// set the type
		if ( sh ) types [n] = title_source_t::TT_RSSITEMLOCAL;
		else      types [n] = title_source_t::TT_RSSITEMREMOTE;
		// advance
		n++;
		// break out if too many already. save some for below.
		if ( n + 30 >= MAX_TIT_CANDIDATES ) break;
	}

	//logf(LOG_DEBUG,"title: took1=%" PRId64,gettimeofdayInMilliseconds()-x);
	//x = gettimeofdayInMilliseconds();

	// . set the flags array
	// . indicates what words are in title candidates already, but
	//   that is set below
	// . up here we set words that are not allowed to be in candidates,
	//   like words that are in a link that is not a self link

	int32_t  need = tr->size();
	StackBuf<10000> flagsBuf;
	if(!flagsBuf.reserve(need))
		return false;
	char *flags = flagsBuf.getBufStart();

	// clear it
	memset ( flags , 0 , need );

	// scan to set link text flags
	// loop over all "words" in the html body
	bool inLink   = false;
	bool selfLink = false;
	for ( int32_t i = 0 ; i < NW ; i++ ) {
		const auto &token = (*tr)[i];
		// if in a link that is not self link, cannot be in a candidate
		if ( inLink && ! selfLink ) {
			flags[i] |= 0x02;
		}

		// out of a link
		if ( token.nodeid == (TAG_A | BACKBIT) ) {
			inLink = false;
		}

		// if not start of <a> tag, skip it
		if ( token.nodeid != TAG_A ) {
			continue;
		}

		// flag it
		inLink = true;

		// get the node in the xml
		int32_t xn = token.xml_node_index;

		// is it a self link?
		int32_t len;
		char *link = xml->getString(xn,"href",&len);

		// . set the url class to this
		// . TODO: use the base url in the doc
		Url u;
		u.set( link, len, true, false );

		// compare
		selfLink = u.equals ( firstUrl );

		// skip if not selfLink
		if ( ! selfLink ) {
			continue;
		}

		// if it is a selflink , check for an "onClick" tag in the
		// anchor tag to fix that Mixx issue for:
		// http://www.npr.org/templates/story/story.php?storyId=5417137

		int32_t  oclen;
		char *oc = xml->getString(xn,"onclick",&oclen);

		if ( ! oc ) {
			oc = xml->getString(xn,"onClick",&oclen);
		}

		// assume not a self link if we see that...
		if ( oc ) {
			selfLink = false;
		}

		// if this <a href> link has a "title" attribute, use that
		// instead! that thing is solid gold.
		int32_t  atlen;
		char *atitle = xml->getString(xn,"title",&atlen);

		// stop and use that, this thing is gold!
		if ( ! atitle || atlen <= 0 ) {
			continue;
		}

		// craziness? ignore it...
		if ( atlen > 400 ) {
			continue;
		}

		// if it contains permanent, permalink or share, ignore it!
		if ( strncasestr ( atitle, "permalink", atlen ) ||
		     strncasestr ( atitle,"permanent", atlen) ||
		     strncasestr ( atitle,"share", atlen) ) {
			continue;
		}

		// do not count the link text as viable
		selfLink = false;

		// aw, dammit
		if ( ti >= MAX_TIT_CANDIDATES ) {
			continue;
		}

		// other dammit
		if ( n >= MAX_TIT_CANDIDATES ) {
			break;
		}

		// ok, process it
		plain_tokenizer_phase_1(atitle, atlen, &(ttr[ti]));
		calculate_tokens_hashes(&(ttr[ti]));

		// set the bookends, it is the whole thing
		cptrs   [n] = &ttr[ti];
		as      [n] = 0;
		bs      [n] = ttr[ti].size();
		scores  [n] = 3.0; // not ALWAYS solid gold!
		types   [n] = title_source_t::TT_TITLEATT;

		// we are using the words class
		ti++;

		// advance
		n++;

		// break out if too many already. save some for below.
		if ( n + 20 >= MAX_TIT_CANDIDATES ) {
			break;
		}
	}

	//logf(LOG_DEBUG,"title: took2=%" PRId64,gettimeofdayInMilliseconds()-x);
	//x = gettimeofdayInMilliseconds();

	//int64_t *wids = WW->getWordIds();
	// . find the last positive scoring guy
	// . do not consider title candidates after "r" if "r" is non-zero
	// . FIXES http://larvatusprodeo.net/2009/01/07/partisanship-politics-and-participation/

	// the candidate # of the title tag
	int32_t tti = -1;

	// allow up to 4 tags from each type
	char table[512];

	// sanity check
	if ( (unsigned)getNumXmlNodes() > sizeof(table) ) { g_process.shutdownAbort(true); }

	// clear table counts
	memset(table, 0, sizeof(table));

	// loop over all "words" in the html body
	for ( int32_t i = 0 ; i < NW ; i++ ) {
		// come back up here if we encounter another "title-ish" tag
		// within our first alleged "title-ish" tag
	subloop:
		// stop after 30k of text
		if ( (*tr)[i].start_pos > 200000 ) {
			break; // 1106
		}

		// get the tag id minus the back tag bit
		nodeid_t tid = (*tr)[i].nodeid & BACKBITCOMP;


		// pen up and pen down for these comment like tags
		if ( tid == TAG_SCRIPT || tid == TAG_STYLE ) {
			// ignore "titles" in script or style tags
			if ( ! ((*tr)[i].nodeid & BACKBIT) ) {
				continue;
			}
		}

		/// @todo ALC we should allow more tags than just link
		// skip if not a good tag. we're already checking for title tag in Title::setTitleFromTags
		if (tid != TAG_A) {
			continue;
		}

		// must NOT be a back tag
		if ( (*tr)[i].nodeid & BACKBIT ) {
			continue;
		}

		// skip if we hit our limit
		if ( table[tid] >= 4 ) {
			continue;
		}

		// skip over tag/word #i
		i++;

		// no words in links, unless it is a self link
		if ( i < NW && (flags[i] & 0x02) ) {
			continue;
		}

		// the start should be here
		int32_t start = -1;

		// do not go too far
		int32_t max = i + 200;

		// find the corresponding back tag for it
		for (  ; i < NW && i < max ; i++ ) {
			// hey we got it, BUT we got no alnum word first
			// so the thing was empty, so loop back to subloop
			if ( ((*tr)[i].nodeid & BACKBITCOMP) == tid  &&   
			     ((*tr)[i].nodeid & BACKBIT    ) && 
			     start == -1 ) {
				goto subloop;
			}

			// if we hit another title-ish tag, loop back up
			if ( ((*tr)[i].nodeid & BACKBITCOMP) == TAG_TITLE || ((*tr)[i].nodeid & BACKBITCOMP) == TAG_A ) {
				// if no alnum text, restart at the top
				if ( start == -1 ) {
					goto subloop;
				}

				// otherwise, break out and see if title works
				break;
			}

			// if we hit a breaking tag...
			if ( isBreakingTagId ( (*tr)[i].nodeid & BACKBITCOMP ) &&
			     // do not consider <span> tags breaking for 
			     // our purposes. i saw a <h1><span> setup before.
			     (*tr)[i].nodeid != TAG_SPAN ) {
				break;
			}

			// skip if not alnum word
			if ( ! (*tr)[i].is_alfanum ) {
				continue;
			}

			// if we hit an alnum word, break out
			if ( start == -1 ) {
				start = i;
			}
		}

		// if no start was found, must have had a 0 score in there
		if ( start == -1 ) {
			continue;
		}

		// if we exhausted the doc, we are done
		if ( i >= NW ) {
			break;
		}

		// skip if way too big!
		if ( i >= max ) {
			continue;
		}

		// if was too long do not consider a title
		if ( i - start > 300 ) {
			continue;
		}

		// . skip if too many bytes
		// . this does not include the length of word #i, but #(i-1)
		if ( (*tr)[i].end_pos - (*tr)[start].start_pos > 1000 ) {
			continue;
		}

		// count it
		table[tid]++;

		// max it out if we are positive scoring. stop after the
		// first positive scoring guy in a section. this might
		// hurt the "Hamlet" thing though...

		// store a point to the title tag guy. Msg20.cpp needs this
		// because the zak's proximity algo uses it in Summary.cpp
		// and in Msg20.cpp

		// only get the first one! often the 2nd on is in an iframe!! which we now expand into here.
		if ( tid == TAG_TITLE && m_titleTagStart == -1 ) {
			m_titleTagStart = start;
			m_titleTagEnd   = i;

			// save the candidate # because we always use this
			// as the title if we are a root
			if ( tti < 0 ) {
				tti = n;
			}
		}

		// point to words class of the body that was passed in to us
		cptrs[n] = tr;
		as[n] = start;
		bs[n] = i;
		if ( tid == TAG_B ) {
			types[n] = title_source_t::TT_BOLDTAG;
			scores[n] = 1.0;
		} else if ( tid == TAG_H1 ) {
			types[n] = title_source_t::TT_HTAG;
			scores[n] = 1.8;
		} else if ( tid == TAG_H2 ) {
			types[n] = title_source_t::TT_HTAG;
			scores[n] = 1.7;
		} else if ( tid == TAG_H3 ) {
			types[n] = title_source_t::TT_HTAG;
			scores[n] = 1.6;
		} else if ( tid == TAG_TITLE ) {
			types[n] = title_source_t::TT_TITLETAG;
			scores[n] = 3.0;
		} else if ( tid == TAG_DIV ) {
			types[n] = title_source_t::TT_DIVTAG;
			scores[n] = 1.0;
		} else if ( tid == TAG_TD ) {
			types[n] = title_source_t::TT_TDTAG;
			scores[n] = 1.0;
		} else if ( tid == TAG_P ) {
			types[n] = title_source_t::TT_PTAG;
			scores[n] = 1.0;
		} else if ( tid == TAG_FONT ) {
			types[n] = title_source_t::TT_FONTTAG;
			scores[n] = 1.0;
		} else if ( tid == TAG_A ) {
			types[n] = title_source_t::TT_ATAG;
			// . self link is very powerful BUT
			//   http://www.npr.org/templates/story/story.php?storyId=5417137
			//   doesn't use it right! so use
			//   1.3 instead of 3.0. that has an "onClick" thing in the
			//   <a> tag, so check for that!
			// this was bad for
			// http://www.spiritualwoman.net/?cat=191
			// so i am demoting from 3.0 to 1.5
			scores[n] = 1.5;
		}

		// count it
		n++;

		// start loop over at tag #i, for loop does an i++, so negate
		// that so this will work
		i--;

		// break out if too many already. save some for below.
		if ( n + 10 >= MAX_TIT_CANDIDATES ) {
			break;
		}
	}

	//logf(LOG_DEBUG,"title: took3=%" PRId64,gettimeofdayInMilliseconds()-x);
	//x = gettimeofdayInMilliseconds();

	// to handle text documents, throw in the first line of text
	// as a title candidate, just make the score really low
	bool textDoc = (contentType == CT_UNKNOWN || contentType == CT_TEXT);

	if (textDoc) {
		// make "i" point to first alphabetical word in the document
		int32_t i ;

		for ( i = 0 ; i < NW && !isAlpha((*tr)[i]) ; i++);

		// if we got a first alphabetical word, then assume that to be the start of our title
		if ( i < NW && n < MAX_TIT_CANDIDATES ) {
			// first word in title is "t0"
			int32_t t0 = i;
			// find end of first line
			int32_t numWords = 0;

			// set i to the end now. we MUST find a \n to terminate the
			// title, otherwise we will not have a valid title
			while (i < NW && numWords < maxTitleWords && ((*tr)[i].is_alfanum || !has_char((*tr)[i].token_start,(*tr)[i].token_end(), '\n'))) {
				if((*tr)[i].is_alfanum) {
					numWords++;
				}

				++i;
			}

			// "t1" is the end
			int32_t t1 = -1;

			// we must have found our \n in order to set "t1"
			if (i <= NW && numWords < maxTitleWords ) {
				t1 = i;
			}

			// set the ptrs
			cptrs   [n] =  tr;

			// this is the last resort i guess...
			scores  [n] =  0.5;
			types   [n] =  title_source_t::TT_FIRSTLINE;
			as      [n] =  t0;
			bs      [n] =  t1;

			// add it as a candidate if t0 and t1 were valid
			if (t0 >= 0 && t1 > t0) {
				n++;
			}
		}
	}

	//logf(LOG_DEBUG,"title: took4=%" PRId64,gettimeofdayInMilliseconds()-x);
	//x = gettimeofdayInMilliseconds();

	{
		// now add the last url path to contain underscores or hyphens
		const char *pstart = firstUrl->getPath();

		// get first url
		const Url *fu = firstUrl;

		// start at the end
		const char *p = fu->getUrl() + fu->getUrlLen();

		// end pointer
		const char *pend = NULL;

		// come up here for each path component
		while ( p >= pstart ) {
			// save end
			pend = p;

			// skip over /
			if ( *p == '/' ) {
				p--;
			}

			// now go back to next /
			int32_t count = 0;
			for ( ; p >= pstart && *p !='/' ; p-- ) {
				if ( *p == '_' || *p == '-' ) {
					count++;
				}
			}

			// did we get it?
			if ( count > 0 ) {
				break;
			}
		}

		// did we get any?
		if ( p > pstart && n < MAX_TIT_CANDIDATES ) {
			// now set words to that
			plain_tokenizer_phase_1(p, pend-p, &(ttr[ti]));
			calculate_tokens_hashes(&(ttr[ti]));

			// point to that
			cptrs   [n] = &ttr[ti];
			as      [n] = 0;
			bs      [n] = ttr[ti].size();
			scores  [n] = 1.0;
			types   [n] = title_source_t::TT_URLPATH;

			// increment since we are using it
			ti++;

			// advance
			n++;
		}
	}

	// save old n
	int32_t oldn = n;

	// . do not split titles if we are a root url maps.yahoo.com was getting "Maps" for the title
	if ( firstUrl->isRoot() ) {
		oldn = -2;
	}

	// point to list of \0 separated titles
	const char *rootTitleBuf    = NULL;
	const char *rootTitleBufEnd = NULL;

	// get the root title if we are not root!
	if (filteredRootTitleBuf) {
#ifdef _VALGRIND_
		VALGRIND_CHECK_MEM_IS_DEFINED(filteredRootTitleBuf,filteredRootTitleBufSize);
#endif
		// point to list of \0 separated titles
		rootTitleBuf    = filteredRootTitleBuf;
		rootTitleBufEnd =  filteredRootTitleBuf + filteredRootTitleBufSize;
	}

	{
		// convert into an array
		int32_t nr = 0;
		const char *pr = rootTitleBuf;
		const char *rootTitles[20];
		int32_t  rootTitleLens[20];

		// loop over each root title segment
		for ( ; pr && pr < rootTitleBufEnd ; pr += strnlen(pr,rootTitleBufEnd-pr) + 1 ) {
			// if we had a query...
			if ( query ) {
				Matches m;

				m.setQuery ( query );

				// see if root title segment has query terms in it
				m.addMatches ( const_cast<char*>(pr), strnlen(pr,rootTitleBufEnd-pr), MF_TITLEGEN );

				// if matches query, do NOT add it, we only add it for
				// removing from the title of the page...
				if ( m.getNumMatches() ) {
					continue;
				}
			}
			// point to it. it should start with an alnum already
			// since it is the "filtered" list of root titles...
			// if not, fix it in xmldoc then.
			rootTitles   [nr] = pr;
			rootTitleLens[nr] = strlen(pr);
			// advance
			nr++;
			// no breaching
			if ( nr >= 20 ) break;
		}

		// now split up candidates in children candidates by tokenizing
		// using :, | and - as delimters.
		// the hyphen must have a space on at least one side, so "cd-rom" does
		// not create a pair of tokens...
		// FIX: for the title:
		// Best Careers 2009: Librarian - US News and World Report
		// we need to recognize "Best Careers 2009: Librarian" as a subtitle
		// otherwise we don't get it as the title. so my question is are we
		// going to have to do all the permutations at some point? for now
		// let's just add in pairs...
		for ( int32_t i = 0 ; i < oldn && n + 3 < MAX_TIT_CANDIDATES ; i++ ) {
			// stop if no root title segments
			if ( nr <= 0 ) break;
			// get the word info
			const TokenizerResult *tr = cptrs[i];
			int32_t   a = as[i];
			int32_t   b = bs[i];
			// init
			int32_t lasta = a;
			bool prev  = false;
			// char length in bytes
			//int32_t charlen = 1;
			// see how many we add
			int32_t added = 0;
			size_t skipTo = 0;
			bool qualified = true;
			// . scan the words looking for a token
			// . sometimes the candidates end in ": " so put in "k < b-1"
			// . made this from k<b-1 to k<b to fix
			//   "Hot Tub Time Machine (2010) - IMDb" to strip IMDb
			for ( int32_t k = a ; k < b && n + 3 < MAX_TIT_CANDIDATES; k++){
				const auto &token = (*tr)[k];
				// skip if not alnum
				if ( !token.is_alfanum ) {
					// in order for next alnum word to
					// qualify for "clipping" if it matches
					// the root title, there has to be more
					// than just spaces here, some punct.
					// otherwise title
					// "T. D. Jakes: Biography from Answers.com"
					// becomes
					// "T. D. Jakes: Biography from"
					qualified=isWordQualified(token);
					continue;
				}
				// gotta be qualified!
				if ( ! qualified ) continue;
				// skip if in root title
				if ( token.start_pos<skipTo ) continue;
				// does this match any root page title segments?
				int32_t j;
				for ( j = 0 ; j < nr ; j++ ) {
					// . compare to root title
					// . break out if we matched!
					if ( ! strncmp( token.token_start, rootTitles[j], rootTitleLens[j] ) ) {
						break;
					}
				}

				// if we did not match a root title segment,
				// keep on chugging
				if ( j >= nr ) continue;
				// . we got a root title match!
				// . skip over
				skipTo = token.start_pos + rootTitleLens[j]; //FIXME: the whole skipTo logic looks overly complicated and may be unnecessary
				// must land on qualified punct then!!
				int32_t e = k+1;
				for ( ; e<b && (*tr)[e].start_pos<skipTo ; e++ );
				// ok, word #e must be a qualified punct
				if ( e<b &&
				     ! isWordQualified((*tr)[e]))
					// assume no match then!!
					continue;
				// if we had a previous guy, reset the end of the
				// previous candidate
				if ( prev ) {
					bs[n-2] = k;
					bs[n-1] = k;
				}
				// . ok, we got two more candidates
				// . well, only one more if this is not the 1st time
				if ( ! prev ) {
					cptrs   [n] = cptrs   [i];
					scores  [n] = scores  [i];
					types   [n] = types   [i];
					as      [n] = lasta;
					bs      [n] = k;
					parent  [n] = i;
					n++;
					added++;
				}
				// the 2nd one
				cptrs   [n] = cptrs   [i];
				scores  [n] = scores  [i];
				types   [n] = types   [i];
				as      [n] = e + 1;
				bs      [n] = bs      [i];
				parent  [n] = i;
				n++;
				added++;

				// now add in the last pair as a whole token
				cptrs   [n] = cptrs   [i];
				scores  [n] = scores  [i];
				types   [n] = types   [i];
				as      [n] = lasta;
				bs      [n] = bs      [i];
				parent  [n] = i;
				n++;
				added++;

				// nuke the current candidate then since it got
				// split up to not contain the root title...
				//cptrs[i] = NULL;

				// update this
				lasta = k+1;

				// if we encounter another delimeter we will have to revise bs[n-1], so note that
				prev = true;
			}

			// nuke the current candidate then since it got
			// split up to not contain the root title...
			if ( added ) {
				scores[i] = 0.001;
				//cptrs[i] = NULL;
			}

			// erase the pair if that there was only one token
			if ( added == 3 ) n--;
		}
	}

	for ( int32_t i = 0 ; i < n ; i++ ) baseScore[i] = scores[i];
	
	//
	// . now punish by 0.85 for every lower case non-stop word it has
	// . reward by 1.1 if has a non-stopword in the query
	//
	for ( int32_t i = 0 ; i < n ; i++ ) {
		// point to the words
		const TokenizerResult *tr = cptrs[i];

		// skip if got nuked above
		if ( ! tr ) {
			continue;
		}

		// skip if empty
		if ( tr->empty() ) {
			continue;
		}

		// get the word boundaries
		int32_t a = as[i];
		int32_t b = bs[i];

		// record the boosts
		float ncb = 1.0;
		float qtb = 1.0;

		// a flag
		bool uncapped = false;

		// scan the words in this title candidate
		for ( int32_t j = a ; j < b ; j++ ) {
			const auto &token = (*tr)[j];
			// skip stop words
			if(isQueryStopWord(token.token_start, token.token_len, token.token_hash, langId)) {
				continue;
			}

			// punish if uncapitalized non-stopword
			if ( ! is_capitalized_utf8(token.token_start) ) {
				uncapped = true;
			}

			// skip if no query
			if ( ! query ) {
				continue;
			}

			int64_t wid = (*tr)[j].token_hash;

			// reward if in the query
			if ( query->getWordNum(wid) >= 0 ) {
				qtb       *= 1.5;
				scores[i] *= 1.5;
			}
		}

		// . only punish once if missing a capitalized word hurts us for:
		//   http://content-uk.cricinfo.com/ausvrsa2008_09/engine/current/match/351682.html
		if ( uncapped ) {
			ncb *= 1.00;
			scores[i] *= 1.00;
		}

		// punish if a http:// title thingy
		const char *s = (*tr)[a].token_start;
		int32_t size = (*tr)[b].end_pos-(*tr)[a].start_pos;
		if(size<0) size=0;
		if ( size > 9 && memcmp("http://", s, 7) == 0 ) {
			ncb *= .10;
		}

		// set these guys
		scores[i] *= ncb;

		noCapsBoost[i]  = ncb;
		qtermsBoost[i]  = qtb;
	}

	// . now compare each candidate to the other candidates
	// . give a boost if matches
	for ( int32_t i = 0 ; i < n ; i++ ) {
		// point to the words
		const TokenizerResult *tr1 = cptrs[i];

		// skip if got nuked above
		if ( ! tr1 ) {
			continue;
		}

		int32_t a1 = as[i];
		int32_t b1 = bs[i];

		// reset some flags
		char localFlag1 = 0;
		char localFlag2 = 0;

		// record the boost
		float iccb = 1.0;

		// total boost
		float total = 1.0;

		// to each other candidate
		for ( int32_t j = 0 ; j < n ; j++ ) {
			// not to ourselves
			if ( j == i ) {
				continue;
			}

			// or our derivatives
			if ( parent[j] == i ) {
				continue;
			}

			// or derivates to their parent
			if ( parent[i] == j ) {
				continue;
			}

			// only check parents now. do not check kids.
			// this was only for when doing percent contained
			// not getSimilarity() per se
			//if ( parent[j] != -1 ) continue;

			// TODO: do not accumulate boosts from a parent
			// and its kids, subtitles...
			//
			// do not compare type X to type Y
			if ( types[i] == title_source_t::TT_TITLETAG ) {
				if ( types[j] == title_source_t::TT_TITLETAG ) {
					continue;
				}
			}

			// do not compare a div candidate to another div cand
			// http://friendfeed.com/foxiewire?start=30
			// likewise, a TD to another TD
			// http://content-uk.cricinfo.com/ausvrsa2008_09/engine/match/351681.html
			// ... etc.
			if ( types[i] == title_source_t::TT_BOLDTAG ||
			     types[i] == title_source_t::TT_HTAG    ||
			     types[i] == title_source_t::TT_DIVTAG  ||
			     types[i] == title_source_t::TT_TDTAG   ||
			     types[i] == title_source_t::TT_FONTTAG    ) {
				if ( types[j] == types[i] ) continue;
			}
			// . do not compare one kid to another kid
			// . i.e. if we got "x | y" as a title and "x | z"
			//   as a link text, it will emphasize "x" too much
			//   http://content-uk.cricinfo.com/ausvrsa2008_09/engine/current/match/351682.html
			if ( parent[j] != -1 && parent[i] != -1 ) continue;

			// . body type tags are mostly mutually exclusive
			// . for the legacy.com url mentioned below, we have
			//   good stuff in <td> tags, so this hurts us...
			// . but for the sake of 
			//   http://larvatusprodeo.net/2009/01/07/partisanship-politics-and-participation/
			//   i put bold tags back

			if ( types[i] == title_source_t::TT_LINKTEXTLOCAL ) {
				if ( types[j] == title_source_t::TT_LINKTEXTLOCAL ) continue;
			}
			if ( types[i] == title_source_t::TT_RSSITEMLOCAL ) {
				if ( types[j] == title_source_t::TT_RSSITEMLOCAL ) continue;
			}

			// only compare to one local link text for each i
			if ( types[j] == title_source_t::TT_LINKTEXTLOCAL && localFlag1 ) {
				continue;
			}
			if ( types[j] == title_source_t::TT_RSSITEMLOCAL  && localFlag2 ) {
				continue;
			}
			if ( types[j] == title_source_t::TT_LINKTEXTLOCAL ) {
				localFlag1 = 1;
			}
			if ( types[j] == title_source_t::TT_RSSITEMLOCAL  ) {
				localFlag2 = 1;
			}

			// not link title attr to link title attr either
			// fixes http://www.spiritualwoman.net/?cat=191
			if ( types[i] == title_source_t::TT_TITLEATT &&
			     types[j] == title_source_t::TT_TITLEATT )
				continue;

			// get our words
			const TokenizerResult *tr2 = cptrs[j];

			// skip if got nuked above
			if ( ! tr2 ) continue;
			int32_t   a2 = as   [j];
			int32_t   b2 = bs   [j];

			// how similar is title #i to title #j ?
			float fp = getSimilarity ( tr2 , a2 , b2 , tr1 , a1 , b1 );

			// error?
			if ( almostEqualFloat(fp, -1.0) ) return false;

			// custom boosting...
			float boost = 1.0;
			if      ( fp >= .95 ) boost = 3.0;
			else if ( fp >= .90 ) boost = 2.0;
			else if ( fp >= .85 ) boost = 1.5;
			else if ( fp >= .80 ) boost = 1.4;
			else if ( fp >= .75 ) boost = 1.3;
			else if ( fp >= .70 ) boost = 1.2;
			else if ( fp >= .60 ) boost = 1.1;
			else if ( fp >= .50 ) boost = 1.08;
			else if ( fp >= .40 ) boost = 1.04;

			// limit total
			total *= boost;
			if ( total > 100.0 ) break;
			// if you are matching the url path, that is pretty 
			// good so give more!
			// actually, that would hurt:
			// http://michellemalkin.com/2008/12/29/gag-worthy/

			// custom boosting!
			if ( fp > 0.0 && g_conf.m_logDebugTitle )
				logf(LOG_DEBUG,"title: i=%" PRId32" j=%" PRId32" fp=%.02f "
				     "b=%.02f", i,j,fp,boost);
			// apply it
			scores[i] *= boost;

			iccb      *= boost;
		}

		inCommonCandBoost[i] = iccb;
	}

	//logf(LOG_DEBUG,"title: took7=%" PRId64,gettimeofdayInMilliseconds()-x);
	//x = gettimeofdayInMilliseconds();


	flagsBuf.purge();

	// now get the highest scoring candidate title
	float max    = -1.0;
	int32_t  winner = -1;
	for ( int32_t i = 0 ; i < n ; i++ ) {
		// skip if got nuked
		if ( ! cptrs[i] ) {
			continue;
		}

		if ( winner != -1 && scores[i] <= max ) {
			continue;
		}

		// url path's cannot be titles in and of themselves
		if ( types[i] == title_source_t::TT_URLPATH ) {
			continue;
		}

		// skip if empty basically, like if title was exact
		// copy of root, then the whole thing got nuked and
		// some empty string added, where a > b
		if ( as[i] >= bs[i] ) {
			continue;
		}

		// got one
		max = scores[i];

		// save it
		winner = i;
	}

	// if we are a root, always pick the title tag as the title
	if ( oldn == -2 && tti >= 0 ) {
		winner = tti;
	}

	// if no winner, all done. no title
	if ( winner == -1 ) {
		// last resort use file name
		if ((contentType == CT_PDF) && (firstUrl->getFilenameLen() != 0)) {
			TokenizerResult tr;
			plain_tokenizer_phase_1(firstUrl->getFilename(), firstUrl->getFilenameLen(), &tr);
			calculate_tokens_hashes(&tr);
			if (!copyTitle(&tr, 0, tr.size())) {
				return false;
			}
		}
		return true;
	}

	// point to the words class of the winner
	const TokenizerResult *tr4 = cptrs[winner];
	// skip if got nuked above
	if ( ! tr4 ) { g_process.shutdownAbort(true); }

	// the string ranges from word #a up to and including word #b
	int32_t a = as[winner];
	int32_t b = bs[winner];
	// sanity check
	if ( a < 0 || (unsigned)b > tr4->size() ) { g_process.shutdownAbort(true); }

	// save the title
	if ( ! copyTitle(tr4, a, b) ) {
		return false;
	}

	/*
	// debug logging
	SafeBuf sb;
	SafeBuf *pbuf = &sb;

	log("title: candidates for %s",xd->getFirstUrl()->getUrl() );

	pbuf->safePrintf("<div stype=\"border:1px solid black\">");
	pbuf->safePrintf("<b>***Finding Title***</b><br>\n");

	pbuf->safePrintf("<table cellpadding=5 border=2><tr>"
			 "<td colspan=20><center><b>Title Generation</b>"
			 "</center></td>"
			 "</tr>\n<tr>"
			 "<td>#</td>"
			 "<td>type</td>"
			 "<td>parent</td>"
			 "<td>base score</td>"
			 "<td>format penalty</td>"
			 "<td>query term boost</td>"
			 "<td>candidate intersection boost</td>"
			 "<td>FINAL SCORE</td>"
			 "<td>title</td>"
			 "</tr>\n" );
			 

	// print out all candidates
	for ( int32_t i = 0 ; i < n ; i++ ) {
		char *ts = "unknown";
		if ( types[i] == title_source_t::TT_LINKTEXTLOCAL  ) ts = "local inlink text";
		if ( types[i] == title_source_t::TT_LINKTEXTREMOTE ) ts = "remote inlink text";
		if ( types[i] == title_source_t::TT_RSSITEMLOCAL   ) ts = "local rss title";
		if ( types[i] == title_source_t::TT_RSSITEMREMOTE  ) ts = "remote rss title";
		if ( types[i] == title_source_t::TT_BOLDTAG        ) ts = "bold tag";
		if ( types[i] == title_source_t::TT_HTAG           ) ts = "header tag";
		if ( types[i] == title_source_t::TT_TITLETAG       ) ts = "title tag";
		if ( types[i] == title_source_t::TT_FIRSTLINE      ) ts = "first line in text";
		if ( types[i] == title_source_t::TT_FONTTAG        ) ts = "font tag";
		if ( types[i] == title_source_t::TT_ATAG           ) ts = "anchor tag";
		if ( types[i] == title_source_t::TT_DIVTAG         ) ts = "div tag";
		if ( types[i] == title_source_t::TT_TDTAG          ) ts = "td tag";
		if ( types[i] == title_source_t::TT_PTAG           ) ts = "p tag";
		if ( types[i] == title_source_t::TT_URLPATH        ) ts = "url path";
		if ( types[i] == title_source_t::TT_TITLEATT       ) ts = "title attribute";
		// get the title
		pbuf->safePrintf(
				 "<tr>"
				 "<td>#%" PRId32"</td>"
				 "<td><nobr>%s</nobr></td>"
				 "<td>%" PRId32"</td>"
				 "<td>%0.2f</td>" // baseScore
				 "<td>%0.2f</td>"
				 "<td>%0.2f</td>"
				 "<td>%0.2f</td>"
				 "<td>%0.2f</td>"
				 "<td>",
				 i,
				 ts ,
				 parent[i],
				 baseScore[i],
				 noCapsBoost[i],
				 qtermsBoost[i],
				 inCommonCandBoost[i],
				 scores[i]);
		// ptrs
		Words *w = cptrs[i];
		int32_t   a = as[i];
		int32_t   b = bs[i];
		// skip if no words
		if ( w->getNumWords() <= 0 ) continue;
		// the word ptrs
		char **wptrs = w->getWordPtrs();
		// string ptrs
		char *ptr  = wptrs[a];//w->getWord(a);
		int32_t  size = w->getStringSize(a,b);
		// it is utf8
		pbuf->safeMemcpy ( ptr , size );
		// end the line
		pbuf->safePrintf("</td></tr>\n");
	}

	pbuf->safePrintf("</table>\n<br>\n");

	// log these for now
	log("title: %s",sb.getBufStart());
	*/

	return true;

}

// . returns 0.0 to 1.0
// . what percent of the alnum words in "w1" are in "w2" from words in [t0,t1)
// . gets 50% points if has all single words, and the other 50% if all phrases
// . Scores class applies to w1 only, use NULL if none
// . use word popularity information for scoring rarer term matches more
// . ONLY CHECKS FIRST 1000 WORDS of w2 for speed
float Title::getSimilarity ( const TokenizerResult *tr1, int32_t i0 , int32_t i1 ,
			     const TokenizerResult *tr2, int32_t t0 , int32_t t1 ) {
	// if either empty, that's 0% contained
	if ( tr1->empty() ) return 0;
	if ( tr2->empty() ) return 0;
	if ( i0 >= i1 ) return 0;
	if ( t0 >= t1 ) return 0;

	// invalids vals
	if ( i0 < 0   ) return 0;
	if ( t0 < 0   ) return 0;

	// . for this to be useful we must use idf
	// . get the popularity of each word in w1
	// . w1 should only be a few words since it is a title candidate
	// . does not add pop for word #i if scores[i] <= 0
	// . take this out for now since i removed the unified dict,
	//   we could use this if we added popularity to g_wiktionary
	//   but it would have to be language dependent
	Pops pops1;
	Pops pops2;
	if ( ! pops1.set ( tr1 , i0 , i1 ) ) return -1.0;
	if ( ! pops2.set ( tr2 , t0 , t1 ) ) return -1.0;

	// now hash the words in w1, the needle in the haystack
	int32_t nw1 = tr1->size();
	if ( i1 > nw1 ) i1 = nw1;
	HashTable table;

	// this augments the hash table
	int64_t lastWid   = -1;
	float     lastScore = 0.0;

	// but we cannot have more than 1024 slots then
	if ( ! table.set ( 1024 ) ) return -1.0;

	// and table auto grows when 90% full, so limit us here
	int32_t count    = 0;
	int32_t maxCount = 20;

	// sum up everything we add
	float sum = 0.0;

	// loop over all words in "tr1" and hash them
	for ( int32_t i = i0 ; i < i1 ; i++ ) {
		const auto &token = (*tr1)[i];

		// skip if not indexable
		if ( !token.is_alfanum ) {
			continue;
		}
		// the word id
		int64_t wid = token.token_hash;

		// no room left in table!
		if ( count++ > maxCount ) {
			//logf(LOG_DEBUG, "query: Hash table for title "
			//    "generation too small. Truncating words from w1.");
			break;
		}

		// . make this a float. it ranges from 0.0 to 1.0
		// . 1.0 means the word occurs in 100% of documents sampled
		// . 0.0 means it occurs in none of them
		// . but "val" is the complement of those two statements!
		float score = 1.0 - pops1.getNormalizedPop(i);

		// accumulate
		sum += score;

		// add to table
		if ( ! table.addKey ( (int32_t)wid , (int32_t)score , NULL ) ) {
			return -1.0;
		}

		// if no last wid, continue
		if ( lastWid == -1LL ) {
			lastWid = wid;
			lastScore = score;
			continue;
		}

		// . what was his val?
		// . the "val" of the phrase: 
		float phrScore = score + lastScore;

		// do not count as much as single words
		phrScore *= 0.5;

		// accumulate
		sum += phrScore;

		// get the phrase id
		int64_t pid = hash64 ( wid , lastWid );

		// now add that
		if ( ! table.addKey ( (int32_t)pid , (int32_t)phrScore , NULL ) )
			return -1.0;
		// we are now the last wid
		lastWid   = wid;
		lastScore = score;
	}

	// sanity check. it can't grow cuz we keep lastWids[] 1-1 with it
	if ( table.getNumSlots() != 1024 ) {
		log(LOG_LOGIC,"query: Title has logic bug.");
		return -1.0;
	}

	// accumulate scores of words that are found
	float found = 0.0;

	// reset
	lastWid = -1LL;

	// loop over all words in "tr2" and hash them
	for ( int32_t i = t0 ; i < t1 ; i++ ) {
		const auto &token = (*tr2)[i];

		// skip if not indexable
		if ( !token.is_alfanum ) {
			continue;
		}
		// the word id
		int64_t wid = token.token_hash;

		// . make this a float. it ranges from 0.0 to 1.0
		// . 1.0 means the word occurs in 100% of documents sampled
		// . 0.0 means it occurs in none of them
		// . but "val" is the complement of those two statements!
		float score = 1.0 - pops2.getNormalizedPop(i);

		// accumulate
		sum += score;

		// is it in table? 
		int32_t slot = table.getSlot ( (int32_t)wid ) ;

		// . if in table, add that up to "found"
		// . we essentially find his wid AND our wid, so 2.0 times
		if ( slot >= 0 ) {
			found += 2.0 * score;
		}

		// now the phrase
		if ( lastWid == -1LL ) {
			lastWid = wid;
			lastScore = score;
			continue;
		}

		// . what was his val?
		// . the "val" of the phrase: 
		float phrScore = score + lastScore;

		// do not count as much as single words
		phrScore *= 0.5;

		// accumulate
		sum += phrScore;

		// get the phrase id
		int64_t pid = hash64 ( wid , lastWid );

		// is it in table? 
		slot = table.getSlot ( (int32_t)pid ) ;

		// . accumulate if in there
		// . we essentially find his wid AND our wid, so 2.0 times
		if ( slot >= 0 ) found += 2.0 * phrScore;

		// we are now the last wid
		lastWid   = wid;
		lastScore = score;
	}

	// do not divide by zero
	if ( almostEqualFloat(sum, 0.0) ) {
		return 0.0;
	}

	// sanity check
	//if ( found > sum              ) { g_process.shutdownAbort(true); }
	if ( found < 0.0 || sum < 0.0 ) { g_process.shutdownAbort(true); }
	// . return the percentage matched
	// . will range from 0.0 to 1.0
	return found / sum;
}

// . copy just words in [t0,t1)
// . returns false on error and sets g_errno
bool Title::copyTitle(const TokenizerResult *tr, int32_t t0, int32_t t1) {
	// skip initial punct
	int32_t            nw    = tr->size();

	// sanity check
	if ( t1 < t0 ) { g_process.shutdownAbort(true); }

	// don't breech number of words
	if ( t1 > nw ) {
		t1 = nw;
	}

	// no title?
	if ( nw == 0 || t0 == t1 ) {
		reset();
		return true;
	}

	const char *end = (*tr)[t1-1].token_end();

	// allocate title
	int32_t need = end - (*tr)[t0].token_start;

	// add 3 bytes for "..." and 1 for \0
	need += 5;

	// return false if could not hold the title
	if ( need > MAX_TITLE_LEN ) {
		m_title[0] = '\0';
		m_titleLen = 0;
		log("query: Could not alloc %" PRId32" bytes for title.",need);
		return false;
	}

	// point to the title to transcribe
	const char *src    = (*tr)[t0].token_start;
	const char *srcEnd = end;

	// include a \" or \'
	if ( t0 > 0 && ( src[-1] == '\'' || src[-1] == '\"' ) ) {
		src--;
	}

	//FIXME: assumption that tokens are contiguous in memory
	// and remove terminating | or :
	for ( ; 
	      srcEnd > src && 
		      (srcEnd[-1] == ':' || 
		       srcEnd[-1] == ' ' ||
		       srcEnd[-1] == '-' ||
		       srcEnd[-1] == '\n' ||
		       srcEnd[-1] == '\r' ||
		       srcEnd[-1] == '|'   )    ; 
	      srcEnd-- );

	// store in here
	char *dst    = m_title;

	// leave room for "...\0"
	char *dstEnd = m_title + need - 4;

	// size of character in bytes, usually 1
	char cs ;

	// point to last punct char
	char *lastp = dst;//NULL;

	int32_t charCount = 0;
	// copy the node @p into "dst"
	for ( ; src < srcEnd ; src += cs , dst += cs ) {
		// get src size
		cs = getUtf8CharSize ( src );

		// break if we are full!
		if ( dst + cs >= dstEnd ) {
			break;
		}

		// or hit our max char limit
		if ( charCount++ >= m_maxTitleLen ) {
			break;
		}

		// skip unwanted character
		if (isUtf8UnwantedSymbols(src)) {
			dst -= cs;
			continue;
		}

		// remember last punct for cutting purposes
		if ( ! is_alnum_utf8 ( src ) ) {
			lastp = dst;
		}

		// encode it as an html entity if asked to
		if ( *src == '<' ) {
			if ( dst + 4 >= dstEnd ) {
				break;
			}

			gbmemcpy ( dst , "&lt;" , 4 );
			dst += 4 - cs;
			continue;
		}

		// encode it as an html entity if asked to
		if ( *src == '>' ) {
			if ( dst + 4 >= dstEnd ) {
				break;
			}

			gbmemcpy ( dst , "&gt;" , 4 );
			dst += 4 - cs;
			continue;
		}

		// if more than 1 byte in char, use gbmemcpy
		if ( cs == 1 ) {
			*dst = *src;
		} else {
			gbmemcpy ( dst , src , cs );
		}
	}

	// null term always
	*dst = '\0';
	
	// do not split a word in the middle!
	if ( src < srcEnd ) { 
		if ( lastp ) {
			gbmemcpy ( lastp , "...\0" , 4 );
			dst = lastp + 3;
		} else {
			gbmemcpy ( dst   , "...\0" , 4 );
			dst += 3;
		}
	}

	// set size. does not include the terminating \0
	m_titleLen = dst - m_title;

	return true;
}
