#include "Pos.h"
#include "tokenizer.h"
#include "XmlNode.h"
#include "Sections.h"
#include "TitleSummaryCodepointFilter.h"
#include "Conf.h"
#include "Mem.h"
#include "Errno.h"
#include "Log.h"
#include "utf8_fast.h"


Pos::Pos() {
	m_buf = NULL;
	m_needsFree = false;
	m_pos = NULL;
	m_bufSize = 0;
	memset(m_localBuf, 0, sizeof(m_localBuf));
}

Pos::~Pos () {
	reset();
}

void Pos::reset() {
	if ( m_buf && m_needsFree )
		mfree ( m_buf , m_bufSize , "Pos" );
	m_buf = NULL;
}

static bool inTag( nodeid_t tagId, nodeid_t expectedTagId, int *count ) {
	if ( !count ) {
		return false;
	}

	if ( tagId == expectedTagId ) {
		++( *count );
	}

	if ( *count ) {
		// back tag
		if ( ( tagId & BACKBITCOMP ) == expectedTagId ) {
			--( *count );
		}
	}

	return ( *count > 0 );
}

unsigned Pos::filter( const TokenizerResult *tr, int32_t a, int32_t b, bool addEllipsis, char *f, char *fend, int32_t version ) {
	logTrace(g_conf.m_logTracePos, "BEGIN");

	// save start point for filtering
	char *fstart = f;

	// -1 is the default value
	if ( b == -1 ) {
		b = tr->size();
	}

	bool trunc = false;

	static const int32_t maxCharSize = 4; // we are utf8

	char* prevChar = NULL;

	char* lastBreak = NULL;
	char* lastBreakPrevChar = NULL; // store char before space

	// flag for stopping back-to-back spaces. only count those as one char.
	bool lastSpace = false;

	int inBadTags = 0;
	int capCount = 0;

	const char *lastPunct = NULL;
	unsigned char lastPunctSize = 0;
	int samePunctCount = 0;

	int dotCount = 0; // store last encountered total consecutive dots
	char* dotPrevChar = NULL; // store char before dot which is not a space

	const char* entityPos[32];
	int32_t entityLen[32];
	char entityChar[32];
	int32_t entityCount = 0;

	// we need to decode HTML entities for version above 122 because we stop decoding
	// &amp; &gt; &lt; to avoid losing information
	if (version >= 122) { // TITLEREC_CURRENT_VERSION
		int32_t maxWord = b;

		if ((unsigned)maxWord == tr->size()) {
			maxWord -= 1;
		}

		const char *pos = (*tr)[a].token_start;
		const char *endPos = (*tr)[maxWord].token_end();

		for ( ; ( pos + 3 ) < endPos; ++pos ) {
			if (*pos == '&') {
				if (*(pos + 3) == ';') {
					if (*(pos + 2) == 't') {
						char c = *(pos + 1);
						if ( c == 'g' || c == 'l' ) {
							// &gt; / &lt;
							entityPos[entityCount] = pos;
							entityLen[entityCount] = 4;
							if ( c == 'g' ) {
								entityChar[entityCount] = '>';
							} else {
								entityChar[entityCount] = '<';
							}
							++entityCount;
						}
					}
				} else if ((pos + 4 < endPos) && *(pos + 4) == ';') {
					if (*(pos + 1) == 'a' && *(pos + 2) == 'm' && *(pos + 3) == 'p') {
						// &amp;
						entityPos[entityCount] = pos;
						entityLen[entityCount] = 5;
						entityChar[entityCount] = '&';
						++entityCount;
					}
				}
			}

			// make sure we don't overflow
			if (entityCount >= 32) {
				break;
			}
		}
	}

	int32_t currentEntityPos = 0;

	for ( int32_t i = a ; i < b ; ++i ) {
		if (trunc) {
			break;
		}

		// is tag?
		nodeid_t tid = (*tr)[i].nodeid;
		if ( tid ) {
			logTrace(g_conf.m_logTracePos, "tags");

			// let's not get from bad tags
			if ( inTag( tid, TAG_STYLE, &inBadTags ) ) {
				continue;
			}

			if ( inTag( tid, TAG_SCRIPT, &inBadTags ) ) {
				continue;
			}

			// if not breaking, does nothing
			if ( !g_nodes[tid & 0x7f].m_isBreaking ) {
				continue;
			}

			// list tag? <li>
			if ( tid == TAG_LI ) {
				if ( ( fend - f > maxCharSize ) ) {
					*f++ = '*';

					// counted as caps because we're detecting all caps for a sentence
					++capCount;
				} else {
					trunc = true;
				}

				lastSpace = false;
				continue;
			}

			// if had a previous breaking tag and no non-tag
			// word after it, do not count back-to-back spaces
			if ( lastSpace ) {
				continue;
			}

			// if had a br tag count it as a '.'
			if ( tid ) { // <br>
				if ( f != fstart ) {
					if ( ( fend - f > 2 * maxCharSize ) ) {
						if ( prevChar && is_ascii(*prevChar) && (*prevChar != '.') ) {
							*f++ = '.';

							// counted as caps because we're detecting all caps for a sentence
							++capCount;
						}

						*f++ = ' ';
						++capCount;
					} else {
						trunc = true;
					}
				}

				lastSpace = true;

				continue;
			}

			if ( ( fend - f > maxCharSize ) ) {
				*f++ = ' ';
			} else {
				trunc = true;
			}

			// do not allow back-to-back spaces
			lastSpace = true;

			continue;
		}

		// scan through all chars discounting back-to-back spaces
		unsigned char cs = 0;
		const char *p    = (*tr)[i].token_start;
		const char *pend = (*tr)[i].token_end();


		const char *currentEntity = NULL;
		int32_t currentEntityLen = 0;
		char currentEntityChar = '\0';
		const char *nextEntity = NULL;
		int32_t nextEntityLen = 0;
		char nextEntityChar = '\0';

		bool hasEntity = false;
		while (currentEntityPos < entityCount) {
			currentEntity = entityPos[currentEntityPos];
			currentEntityLen = entityLen[currentEntityPos];
			currentEntityChar = entityChar[currentEntityPos];

			if ( currentEntityPos + 1 < entityCount ) {
				nextEntity = entityPos[currentEntityPos + 1];
				nextEntityLen = entityLen[currentEntityPos + 1];
				nextEntityChar = entityChar[currentEntityPos + 1];
			}

			if ( p <= currentEntity || p <= (currentEntity + currentEntityLen) ) {
				hasEntity = true;
				break;
			} else {
				if (p > currentEntity) {
					++currentEntityPos;
				} else {
					break;
				}
			}
		}

		/// @todo ALC configurable maxSamePunctCount so we can tweak this as needed
		const int maxSamePunctCount = 5;
		char *lastEllipsis = NULL;

		// assume filters out to the same # of chars
		for ( ; p < pend; p += cs ) {
			// get size
			cs = getUtf8CharSize(p);

			// skip entity
			if ( hasEntity ) {
				if (p >= currentEntity && p < (currentEntity + currentEntityLen)) {
					if (p == currentEntity) {
						*f++ = currentEntityChar;
						lastSpace = false;
					}
					continue;
				}

				if (nextEntity && p >= nextEntity && p < (nextEntity + nextEntityLen)) {
					if (p == nextEntity) {
						*f++ = nextEntityChar;
						lastSpace = false;
					}
					continue;
				}
			}

			// skip unwanted character
			if ( isUtf8UnwantedSymbols( p ) ) {
				continue;
			}


			bool resetPunctCount = true;
			if (is_punct_utf8(p) && !is_wspace_utf8(p)) {
				if ( ( cs == lastPunctSize) && ( memcmp(lastPunct, p, cs) == 0 ) ) {
					resetPunctCount = false;
					++samePunctCount;
				}
			}

			if ( resetPunctCount ) {
				if (samePunctCount >= maxSamePunctCount) {
					f -= (maxSamePunctCount);

					bool addEllipsis = false;
					if ( lastEllipsis ) {
						// if all from f to last ellipsis are punctuation, skip to last ellipsis
						for ( char *c = lastEllipsis + 1; c < f; ++c) {
							if ( is_alnum_utf8( c ) ) {
								logTrace(g_conf.m_logTracePos, "addEllipsis=true");
								addEllipsis = true;
								break;
							}
						}

						if ( !addEllipsis ) {
							f = lastEllipsis;
						}
					} else {
						logTrace(g_conf.m_logTracePos, "addEllipsis=true");
						addEllipsis = true;
					}

					if (addEllipsis) {
						logTrace(g_conf.m_logTracePos, "addEllipsis");

						if ( f != fstart && *(f - 1) != ' ' ) {
							*f++ = ' ';
						}

						lastSpace = true;
						memcpy ( f, "\342\200\246 ", 4 ); //horizontal ellipsis, code point 0x2026
						f += 4;

						lastEllipsis = f;
					}
				}

				lastPunct = p;
				lastPunctSize = cs;
				samePunctCount = 0;
			}

			if ( samePunctCount >= maxSamePunctCount ) {
				continue;
			}

			// do not count space if one before
			if ( is_wspace_utf8 (p) ) {
				if ( lastSpace ) {
					continue;
				}

				lastSpace = true;

				if ( fend - f > 1 ) {
					lastBreakPrevChar = prevChar;

					// don't store lastBreak if we have less than ellipsis length ' ...'
					if ( fend - f > 4 ) {
						lastBreak = f;
					}

					*f++ = ' ';

					// counted as caps because we're detecting all caps for a sentence
					++capCount;

					dotCount = 0;

					// we don't store space as dotPreviousChar because we want to strip ' ...' as well
				} else {
					trunc = true;
				}

				continue;
			}

			if ( fend - f > cs ) {
				prevChar = f;

				if ( cs == 1 ) {
					// we only do it for ascii to avoid catering for different rules in different languages
					// https://en.wikipedia.org/wiki/Letter_case#Exceptional_letters_and_digraphs
					// eg:
					//   The Greek upper-case letter "Σ" has two different lower-case forms:
					//     "ς" in word-final position and "σ" elsewhere
					if ( !is_alpha_a( *p ) || is_upper_a( *p ) ) {
						// non-alpha is counted as caps as well because we're detecting all caps for a sentence
						// and comma/quotes/etc. is included
						++capCount;
					}

					// some sites try to be smart and truncate for us, let's remove that
					// if if there are no space between dots and letter
					if ( *p == '.' ) {
						++dotCount;
					} else {
						dotCount = 0;
						dotPrevChar = f;
					}

					*f++ = *p;
				} else {
					dotCount = 0;
					dotPrevChar = f;

					memcpy( f, p, cs );
					f += cs;
				}
			} else {
				trunc = true;
			}

			lastSpace = false;
		}
	}

	/// @todo ALC simplify logic/break into smaller functions

	/// @todo ALC configurable minCapCount so we can tweak this as needed
	const int minCapCount = 5;

	// only capitalize first letter in a word for a sentence with all caps
	//TODO: assumes we want a us-centric title capitilization. There are other styles.
	//FIXME: Assumes lowercasing a codepoint doesn't change its utf8-encoding length. This is not true (eg. Turkish U+0130 İ -> U+0069 i)
	if ( capCount > minCapCount && capCount == ( f - fstart ) ) {
		logTrace(g_conf.m_logTracePos, "all caps");

		bool isFirstLetter = true;

		unsigned char cs = 0;
		for ( char *c = fstart; c < f; c += cs ) {
			cs = getUtf8CharSize(c);

			bool isAlpha = is_alpha_utf8( c );

			if ( isAlpha ) {
				if (isFirstLetter) {
					isFirstLetter = false;
					continue;
				}
			} else {
				// some hard coded punctuation that we don't want to treat as first letter
				// eg: Program's instead of Program'S
				if ( cs == 1 && *c == '\'' ) {
					isFirstLetter = false;
				} else {
					isFirstLetter = true;
				}
				continue;
			}

			if ( !isFirstLetter ) {
				to_lower_utf8(c, c);
				//TODO: do titlecase on the first letter - don't leave it as uppercase
			}
		}
	}

	/// @todo ALC configurable minRemoveEllipsisLen so we can tweak this as needed
	const int minRemoveEllipsisLen = 90;

	logTrace(g_conf.m_logTracePos, "len=%ld", (f - fstart));

	// let's remove ellipsis (...) at the end
	if ( (f - fstart) >= minRemoveEllipsisLen && dotCount == 3 ) {
		logTrace(g_conf.m_logTracePos, "remove ellipsis");
		if ( dotPrevChar ) {
			if ( is_ascii3( *dotPrevChar ) ) {
				logTrace(g_conf.m_logTracePos, "dotPrevChar=%c", *dotPrevChar);
				switch ( *dotPrevChar ) {
					case ',':
						trunc = true;
						lastBreak = dotPrevChar + 1;
						break;
					case '!':
					case '.':
						trunc = false;
						f = dotPrevChar + 1;
						break;
					case ' ':
						trunc = false;

						if ( lastBreak ) {
							f = lastBreak;
						}
						break;
					default:
						trunc = true;

						if ( lastBreakPrevChar ) {
							logTrace(g_conf.m_logTracePos, "lastBreakPrevChar=%c", *lastBreakPrevChar);
							if ( is_ascii( *( lastBreakPrevChar ) ) ) {
								switch ( *( lastBreakPrevChar ) ) {
									case '!':
									case '.':
										trunc = false;

										if (lastBreak) {
											f = lastBreak;
										}
										break;
									default:
										break;
								}
							}
						}
						break;
				}
			}
		} else {
			trunc = true;
			lastBreak = nullptr;
		}
	}
	if ( trunc ) {
		logTrace(g_conf.m_logTracePos, "trunc");

		if ( lastBreak == NULL ) {
			logTrace(g_conf.m_logTracePos, "END. Return 0");
			return 0;
		}

		f = lastBreak;

		/// @todo ALC we should cater ellipsis for different languages
		if ( addEllipsis ) {
			logTrace(g_conf.m_logTracePos, "addEllipsis");
			if ( (fend - f) > 4 ) {
				memcpy ( f, " \342\200\246", 4 ); //horizontal ellipsis, code point 0x2026
				f += 4;
			}
		}
	}

	// NULL terminate f
	*f = '\0';

	int bytesStored = static_cast<int>(f - fstart);

	logTrace(g_conf.m_logTracePos, "END. Return %d", bytesStored);

	return bytesStored;
}

bool Pos::set(const TokenizerResult *tr, int32_t a, int32_t b) {
	// free m_buf in case this is a second call
	reset();

	int32_t nw = tr->size();

	// -1 is the default value
	if ( b == -1 ) {
		b = nw;
	}

	// alloc array if need to
	int32_t need = (nw+1) * 4;

	// do not destroy m_pos/m_numWords if only filtering into a buffer
	m_needsFree = false;

	m_buf = m_localBuf;
	if ( need > POS_LOCALBUFSIZE ) {
		m_buf = (char *)mmalloc(need,"Pos");
		m_needsFree = true;
	}

	// bail on error
	if ( ! m_buf ) {
		return false;
	}

	m_bufSize = need;
	m_pos      = (int32_t *)m_buf;

	// this is the CHARACTER count.
	int32_t pos = 0;

	// flag for stopping back-to-back spaces. only count those as one char.
	bool lastSpace = false;

	for ( int32_t i = a ; i < b ; i++ ) {
		// set pos for the ith word to "pos"
		m_pos[i] = pos;

		nodeid_t tid = (*tr)[i].nodeid;
		// is tag?
		if ( tid ) {
			// if not breaking, does nothing
			if ( !g_nodes[tid & 0x7f].m_isBreaking ) {
				continue;
			}

			// list tag? <li>
			if ( tid == TAG_LI ) {
				++pos;
				lastSpace = false;
				continue;
			}

			// if had a previous breaking tag and no non-tag
			// word after it, do not count back-to-back spaces
			if ( lastSpace ) {
				continue;
			}

			// if had a br tag count it as a '. '
			if ( tid ) { // <br>
				pos += 2;
				lastSpace = true;

				continue;
			}

			// count as a single space
			pos++;

			// do not allow back-to-back spaces
			lastSpace = true;

			continue;
		}

		// scan through all chars discounting back-to-back spaces
		const char *wp = (*tr)[i].token_start;
		const char *pend = wp + (*tr)[i].token_len;
		unsigned char cs = 0;

		// assume filters out to the same # of chars
		for ( const char *p = wp; p < pend; p += cs ) {
			// get size
			cs = getUtf8CharSize(p);

			// do not count space if one before
			if ( is_wspace_utf8 (p) ) {
				if ( lastSpace ) {
					continue;
				}

				lastSpace = true;

				++pos;
				continue;
			}

			++pos;
			lastSpace = false;
		}
	}

	// set pos for the END of the last word here
	m_pos[nw] = pos;

	return true;
}
