//#include "gb-include.h"

#include "Words.h"
#include "Xml.h"
#include "unicode/UCEnums.h"
#include "StopWords.h"
#include "Speller.h"
#include "HashTableX.h"
#include "Sections.h"
#include "XmlNode.h" // getTagLen()
#include "Mem.h"
#include "Sanity.h"


Words::Words ( ) {
	m_buf = NULL;
	m_bufSize = 0;
	memset(m_localBuf, 0, sizeof(m_localBuf));
	reset();
}
Words::~Words ( ) {
	reset();
}
void Words::reset ( ) {
	m_numWords = 0;
	m_numAlnumWords = 0;
	m_preCount = 0;
	if ( m_buf && m_buf != m_localBuf && m_buf != m_localBuf2 )
		mfree ( m_buf , m_bufSize , "Words" );
	m_buf = NULL;
	m_bufSize = 0;
	m_nodes = NULL;
	m_tagIds = NULL;
	m_numTags = 0;
	m_localBuf2 = NULL;
	m_localBufSize2 = 0;

	// Coverity
	m_words = NULL;
	m_wordLens = 0;
	m_wordIds = NULL;
}

// a quickie
// this url gives a m_preCount that is too low. why?
// http://go.tfol.com/163/speed.asp
static int32_t countWords ( const char *p, int32_t plen ) {
	const char *pend  = p + plen;
	int32_t  count = 1;

	while ( p < pend && *p) {

		// sequence of punct
		for  ( ; p < pend && *p && ! is_alnum_utf8 (p) ; p += getUtf8CharSize(p) ) {
			// in case being set from xml tags, count as words now
			if ( *p == '<' ) {
				count++;
			}
		}
		count++;

		// sequence of alnum
		for  ( ; p < pend && *p && is_alnum_utf8 (p) ; p += getUtf8CharSize(p) )
			;

		count++;

	};
	// some extra for good meaure
	return count+10;
}

bool Words::set( Xml *xml, int32_t node1, int32_t node2 ) {
	reset();

	// if xml is empty, bail
	if ( !xml->getContent() ) {
		return true;
	}

	int32_t numNodes = xml->getNumNodes();
	if ( numNodes <= 0 ) {
		return true;
	}

	// . can be given a range, if node2 is -1 that means all!
	// . range is half-open: [node1, node2)
	if ( node2 < 0 ) {
		node2 = numNodes;
	}

	// sanity check
	if ( node1 > node2 ) gbshutdownLogicError();

	char *start = xml->getNode(node1);
	char *end = xml->getNode( node2 - 1 ) + xml->getNodeLen( node2 - 1 );
	int32_t  size  = end - start;

	m_preCount = countWords( start , size );

	// allocate based on the approximate count
	if ( !allocateWordBuffers( m_preCount, true ) ) {
		return false;
	}

	// are we done?
	for ( int32_t k = node1; k < node2 && m_numWords < m_preCount; ++k ) {
		// get the kth node
		char *node = xml->getNode( k );
		int32_t nodeLen = xml->getNodeLen( k );

		// is the kth node a tag?
		if ( !xml->isTag( k ) ) {
			addWords(node, nodeLen);
			continue;
		}

		// it is a tag
		m_words    [m_numWords] = node;
		m_wordLens [m_numWords] = nodeLen;
		m_tagIds   [m_numWords] = xml->getNodeId(k);
		m_wordIds  [m_numWords] = 0LL;
		m_nodes    [m_numWords] = k;

		// If it is an end-tag then set the bit
		if ( xml->isBackTag(k)) {
			m_tagIds[m_numWords] |= BACKBIT;
		}

		m_numWords++;

		// used by XmlDoc.cpp
		m_numTags++;
	}

	return true;
}


// . set words from a string
// . assume no HTML entities in the string "s"
// . s must be NULL terminated
// . NOTE: do not free "s" from under us cuz we reference it
// . break up the string ,"s", into "words".
// . doesn't do tags, only text nodes in "xml"
// . our definition of a word is as close to English as we can get it
// . BUT we also consider a string of punctuation characters to be a word
bool Words::set(const char *s) {
	return set(s,0x7fffffff);
}

bool Words::set(const char *s, int32_t slen) {
	reset();
	// bail if nothing
	if ( ! s || slen == 0 ) {
		return true;
	}

	// determine rough upper bound on number of words by counting
	// punct/alnum boundaries
	m_preCount = countWords(s,slen);
	if ( !allocateWordBuffers( m_preCount ) ) {
		return false;
	}

	return addWords(s, slen);
}

bool Words::addWords(const char *s, int32_t nodeLen) {
	int32_t  i = 0;
	int32_t  j;
	int32_t  wlen;

	bool hadApostrophe = false;

	Unicode::script_t oldScript = Unicode::script_t::Common;
	Unicode::script_t saved;

 uptop:

	// bad utf8 can cause a breach
	if ( i >= nodeLen ) {
		goto done;
	}

	if ( ! s[i] ) {
		goto done;
	}

	if ( !is_alnum_utf8( s + i ) ) {
		if ( m_numWords >= m_preCount ) {
			goto done;
		}

		// it is a punct word, find end of it
		const char *start = s+i;
		for ( ; i<nodeLen && s[i] ; i += getUtf8CharSize(s+i)) {
			// if we are simple ascii, skip quickly
			if ( is_ascii(s[i]) ) {
				// accumulate NON-alnum chars
				if ( ! is_alnum_a(s[i]) ) {
					continue;
				}

				// update
				oldScript = Unicode::script_t::Common;

				// otherwise, stop we got alnum
				break;
			}

			// if we are utf8 we stop on special props
			UChar32 c = utf8Decode ( s+i );

			// stop if word char
			if ( ! ucIsWordChar_fast( c ) ) {
				continue;
			}

			// update first though
			oldScript = UnicodeMaps::query_script(c);
			if ( oldScript == Unicode::script_t::Latin ) oldScript = Unicode::script_t::Common;

			// then stop
			break;
		}
		m_words        [ m_numWords  ] = start;
		m_wordLens     [ m_numWords  ] = s+i - start;
		m_wordIds      [ m_numWords  ] = 0LL;
		m_nodes        [ m_numWords  ] = 0;

		if (m_tagIds) {
			m_tagIds[m_numWords] = 0;
		}

		m_numWords++;
		goto uptop;
	}

	// get an alnum word
	j = i;
 again:
	for ( ; i<nodeLen && s[i] ; i += getUtf8CharSize(s+i) ) {
		// simple ascii?
		if ( is_ascii(s[i]) ) {
			// accumulate alnum chars
			if ( is_alnum_a(s[i]) ) continue;
			// update
			oldScript = Unicode::script_t::Common;
			// otherwise, stop we got punct
			break;
		}
		// get the code point of the utf8 char
		UChar32 c = utf8Decode ( s+i );
		// get props
		uint32_t props = UnicodeMaps::query_properties(c);
		// good stuff?
		if(props&(Unicode::Extender)) continue;
		//(todo): props&ignorable (which is quite complicated)
		//something abotu ignorable
		// stop? if UC_WORCHAR is set, that means its an alnum
		if(!UnicodeMaps::is_wordchar(c)) {
			// reset script between words
			oldScript = Unicode::script_t::Common;
			break;
		}
		// save it
		saved = oldScript;
		// update here
		oldScript = UnicodeMaps::query_script(c);
		// treat ucScriptLatin (30) as common so we can have latin1
		// like char without breaking the word!
		if ( oldScript == Unicode::script_t::Latin ) oldScript = Unicode::script_t::Common;
		// stop on this crap too i guess. like japanes chars?
		if((props&Unicode::Ideographic) || oldScript==Unicode::script_t::Hiragana || oldScript==Unicode::script_t::Thai) {
			// include it
			i += getUtf8CharSize(s+i);
			// but stop
			break;
		}
		// script change?
		if ( saved != oldScript ) break;
	}
	
	// . java++, A++, C++ exception
	// . A+, C+, exception
	// . TODO: consider putting in Bits.cpp w/ D_CAN_BE_IN_PHRASE
	if ( s[i]=='+' ) {
		if ( s[i+1]=='+' && !is_alnum_utf8(&s[i+2]) ) i += 2;
		else if ( !is_alnum_utf8(&s[i+1]) ) i++;
	}
	// . c#, j#, ...
	if ( s[i]=='#' && !is_alnum_utf8(&s[i+1]) ) i++;

	// comma is ok if like ,ddd!d
	if ( s[i]==',' && 
	     i-j <= 3 &&
	     is_digit(s[i-1]) ) {
		// if word so far is 2 or 3 chars, make sure digits
		if ( i-j >= 2 && ! is_digit(s[i-2]) ) goto nogo;
		if ( i-j >= 3 && ! is_digit(s[i-3]) ) goto nogo;
		// scan forward
		while ( s[i] == ',' &&
		        is_digit(s[i+1]) &&
		        is_digit(s[i+2]) &&
		        is_digit(s[i+3]) &&
		        ! is_digit(s[i+4]) ) {
			i += 4;
		}
	}

	// decimal point?
	if ( s[i] == '.' &&
	     is_digit(s[i-1]) &&
	     is_digit(s[i+1]) ) {
		// allow the decimal point
		i++;
		// skip over string of digits
		while ( is_digit(s[i]) ) i++;
	}
	
 nogo:

	// allow for words like we're dave's and i'm
	if ( s[i] == '\'' && s[i + 1] && is_alnum_utf8( &s[i + 1] ) && !hadApostrophe ) {
		i++;
		hadApostrophe = true;
		goto again;
	}
	hadApostrophe = false;
	
	// get word length
	wlen = i - j;
	if ( m_numWords >= m_preCount ) goto done;
	m_words   [ m_numWords  ] = &s[j];
	m_wordLens[ m_numWords  ] = wlen;

	{
		int64_t h = hash64Lower_utf8(&s[j],wlen);
		m_wordIds [m_numWords] = h;
	}

	m_nodes[m_numWords] = 0;
	if (m_tagIds) m_tagIds[m_numWords] = 0;
	m_numWords++;
	m_numAlnumWords++;
	// get a punct word
	goto uptop;

 done:
	// bad programming warning
	if ( m_numWords > m_preCount ) {
		log(LOG_LOGIC, "build: words: set: Fix counting routine.");
		gbshutdownLogicError();
	}

	return true;
}

// common to Unicode and ISO-8859-1
bool Words::allocateWordBuffers(int32_t count, bool tagIds) {
	// alloc if we need to (added 4 more for m_nodes[])
	int32_t wordSize = 0;
	wordSize += sizeof(char *);
	wordSize += sizeof(int32_t);
	wordSize += sizeof(int64_t);
	wordSize += sizeof(int32_t);
	if ( tagIds ) wordSize += sizeof(nodeid_t);
	m_bufSize = wordSize * count;
	if(m_bufSize < 0) {
		log(LOG_WARN, "build: word count overflow %" PRId32" bytes wordSize=%" PRId32" count=%" PRId32".",
		    m_bufSize, wordSize, count);
		return false;
	}
	if ( m_bufSize <= m_localBufSize2 && m_localBuf2 ) {
		m_buf = m_localBuf2;
	}
	else if ( m_bufSize <= WORDS_LOCALBUFSIZE ) {
		m_buf = m_localBuf;
	}
	else {
		m_buf = (char *)mmalloc ( m_bufSize , "Words" );
		if ( ! m_buf ) {
			log(LOG_WARN, "build: Could not allocate %" PRId32" bytes for parsing document.", m_bufSize);
			return false;
		}
	}

	// set ptrs
	char *p = m_buf;
	m_words    = (const char **)p ;
	p += sizeof(char*) * count;
	m_wordLens = (int32_t      *)p ;
	p += sizeof(int32_t)* count;
	m_wordIds  = (int64_t *)p ;
	p += sizeof (int64_t) * count;
	m_nodes = (int32_t *)p;
	p += sizeof(int32_t) * count;

	if (tagIds) {
		m_tagIds = (nodeid_t*) p;
		p += sizeof(nodeid_t) * count;
	}

	if ( p > m_buf + m_bufSize ) gbshutdownLogicError();

	return true;
}

unsigned char getCharacterLanguage ( const char *utf8Char ) {
	// romantic?
	char cs = getUtf8CharSize ( utf8Char );
	// can't say what language it is
	if ( cs == 1 ) return langUnknown;
	// convert to 32 bit unicode
	UChar32 c = utf8Decode ( utf8Char );
	Unicode::script_t us = UnicodeMaps::query_script(c);
	// arabic? this also returns for persian!! fix?
	if ( us == Unicode::script_t::Arabic ) 
		return langArabic;
	if ( us == Unicode::script_t::Cyrillic )
		return langRussian;
	if ( us == Unicode::script_t::Hebrew )
		return langHebrew;
	if ( us == Unicode::script_t::Greek )
		return langGreek;

	return langUnknown;
}

// . return the value of the specified "field" within this html tag, "s"
// . the case of "field" does not matter
char *getFieldValue( char *s, int32_t slen, const char *field, int32_t *valueLen ) {
	// reset this to 0
	*valueLen = 0;
	// scan for the field name in our node
	int32_t flen = strlen(field);
	char inQuotes = '\0';
	int32_t i;

	// make it sane
	if ( slen > 2000 ) slen = 2000;

	for ( i = 1; i + flen < slen ; i++ ) {
		// skip the field if it's quoted
		if ( inQuotes) {
			if (s[i] == inQuotes ) inQuotes = 0;
			continue;
		}
		// set inQuotes to the quote if we're in quotes
		if ( (s[i]=='\"' || s[i]=='\'')){ 
			inQuotes = s[i];
			continue;
		} 
		// if not in quote tag might end
		if ( s[i] == '>' && ! inQuotes ) return NULL;
		// a field name must be preceeded by non-alnum
		if ( is_alnum_a ( s[i-1] ) ) continue;
		// the first character of this field shout match field[0]
		if ( to_lower_a (s[i]) != to_lower_a(field[0] )) continue;
		// field just be immediately followed by an = or space
		if (s[i+flen]!='='&&!is_wspace_a(s[i+flen]))continue;
		// field names must match
		if ( strncasecmp ( &s[i], field, flen ) != 0 ) continue;
		// break cuz we got a match for our field name
		break;
	}

	// return NULL if no matching field
	if ( i + flen >= slen ) return NULL;

	// advance i over the fieldname so it pts to = or space
	i += flen;

	// advance i over spaces
	while ( i < slen && is_wspace_a ( s[i] ) ) i++;

	// advance over the equal sign, return NULL if does not exist
	if ( i < slen && s[i++] != '=' ) return NULL;

	// advance i over spaces after the equal sign
	while ( i < slen && is_wspace_a ( s[i] ) ) i++;
	
	// now parse out the value of this field (could be in quotes)
	inQuotes = '\0';

	// set inQuotes to the quote if we're in quotes
	if ( s[i]=='\"' || s[i]=='\'') inQuotes = s[i++]; 

	// mark this as the start of the value
	int start=i;

	// advance i until we hit a space, or we hit a that quote if inQuotes
	if (inQuotes) while (i<slen && s[i] != inQuotes ) i++;
	else while ( i<slen &&!is_wspace_a(s[i])&&s[i]!='>')i++;

	// set the length of the value
	*valueLen = i - start;

	// return a ptr to the value
	return s + start;
}
