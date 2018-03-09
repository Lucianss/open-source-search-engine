#include "Highlight.h"
#include "tokenizer.h"
#include "Query.h"
#include "Matches.h"
#include "Xml.h"
#include "Url.h"
#include "gb-include.h"

#include "Phrases.h"

// use different front tags for matching different term #'s
static const char *s_frontTags[] = {
	"<span class='gbcnst00'>" ,
	"<span class='gbcnst01'>" ,
	"<span class='gbcnst02'>" ,
	"<span class='gbcnst03'>" ,
	"<span class='gbcnst04'>" ,
	"<span class='gbcnst05'>" ,
	"<span class='gbcnst06'>" ,
	"<span class='gbcnst07'>" ,
	"<span class='gbcnst08'>" ,
	"<span class='gbcnst09'>"
};

static const char s_styleSheet[] =
"<style type='text/css'>"
	"span.gbcnst00{color:black;background-color:#ffff66}"
	"span.gbcnst01{color:black;background-color:#a0ffff}"
	"span.gbcnst02{color:black;background-color:#99ff99}"
	"span.gbcnst03{color:black;background-color:#ff9999}"
	"span.gbcnst04{color:black;background-color:#ff66ff}"
	"span.gbcnst05{color:white;background-color:#880000}"
	"span.gbcnst06{color:white;background-color:#00aa00}"
	"span.gbcnst07{color:white;background-color:#886800}"
	"span.gbcnst08{color:white;background-color:#004699}"
	"span.gbcnst09{color:white;background-color:#990099}"
"</style>";

// . return length stored into "buf"
// . content must be NULL terminated
// . if "useAnchors" is true we do click and scroll
// . if "isQueryTerms" is true, we do typical anchors in a special way
int32_t Highlight::set( SafeBuf *sb, const char *content, int32_t contentLen, Query *q, const char *frontTag,
			const char *backTag ) {
	TokenizerResult tr;
	plain_tokenizer_phase_1(content, contentLen, &tr);
	calculate_tokens_hashes(&tr);

	Bits bits;
	if ( !bits.set(&tr)) {
		return -1;
	}

	Phrases phrases;
	if ( !phrases.set( &tr, &bits ) ) {
		return -1;
	}

	Matches matches;
	matches.setQuery ( q );

	if ( !matches.addMatches( &tr, &phrases ) ) {
		return -1;
	}

	// store
	m_numMatches = matches.getNumMatches();

	return set ( sb, &tr, &matches, frontTag, backTag, q);
}

// New version
int32_t Highlight::set( SafeBuf *sb, const TokenizerResult *tr, const Matches *matches, const char *frontTag,
			const char *backTag, const Query *q ) {
	// save stuff
	m_frontTag    = frontTag;
	m_backTag     = backTag;

	// set lengths of provided front/back highlight tags
	if ( m_frontTag ) {
		m_frontTagLen = strlen ( frontTag );
	}
	if ( m_backTag  ) {
		m_backTagLen  = strlen ( backTag  );
	}

	m_sb = sb;

	// label it
	m_sb->setLabel ("highw");

	if ( ! highlightWords ( tr, matches, q ) ) {
		return -1;
	}

	// null terminate
	m_sb->nullTerm();

	// return the length
	return m_sb->length();
}

bool Highlight::highlightWords ( const TokenizerResult *tr, const Matches *m, const Query *q ) {
	// set nexti to the word # of the first word that matches a query word
	int32_t nextm = -1;
	int32_t nexti = -1;
	if ( m->getNumMatches() > 0 ) {
		nextm = 0;
		nexti = m->getMatch(0).m_wordNum;
	}

	int32_t backTagi = -1;
	bool inTitle  = false;

	for ( int32_t i = 0 ; i < tr->size(); i++ ) {
		const auto &token = (*tr)[i];
		// set word's info
		bool endHead = false;
		bool endHtml = false;

		nodeid_t base_nodeid = token.nodeid&BACKBITCOMP;
		if ( base_nodeid == TAG_TITLE ) {
			inTitle = (token.nodeid&BACKBIT)==0;
		} else if ( base_nodeid == TAG_HTML ) {
			endHtml = (token.nodeid&BACKBIT)!=0;
		} else if ( base_nodeid == TAG_HEAD ) {
			endHead = (token.nodeid&BACKBIT)!=0;
		}

		// This word is a match...see if we're gonna tag it
		// dont put the same tags around consecutive matches
		if ( i == nexti && ! inTitle && ! endHead && ! endHtml) {
			// get the match class for the match
			const Match *mat = &m->getMatch(nextm);
			// discontinue any current font tag we are in
			if ( i < backTagi ) {
				// push backtag ahead if needed
				if ( i + mat->m_numWords > backTagi ) {
					backTagi = i + mat->m_numWords;
				}
			}
			else {
				// now each match is the entire quote, so write the front tag right now
				if ( m_frontTag ) {
					m_sb->safeStrcpy ( m_frontTag );
				} else {
					m_sb->safeStrcpy( s_frontTags[(mat->m_qwordNum % 10)] );
				}

				// when to write the back tag? add the number of
				// words in the match to i.
				backTagi = i + mat->m_numWords;
			}
		} else if ( endHead ) {
			// include the tags style sheet immediately before the closing </TITLE> tag
			m_sb->safeStrcpy( s_styleSheet );
		}

		if ( i == nexti ) {
			// advance match
			nextm++;
			// set nexti to the word # of the next match
			if ( nextm < m->getNumMatches() ) 
				nexti = m->getMatch(nextm).m_wordNum;
			else
				nexti = -1;
		}

		m_sb->safeMemcpy ( token.token_start, token.token_len );

		// back tag
		if ( i == backTagi-1 ) {
			// store the back tag
			if ( m_backTag ) {
				m_sb->safeMemcpy( m_backTag, m_backTagLen );
			} else {
				m_sb->safeStrcpy("</span>");
			}
		}
	}

	return true;
}
