// Copyright (C) 2017 Privacore ApS - https://www.privacore.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// License TL;DR: If you change this file, you must publish your changes.
//
#include "FxCheckSpam.h"
#include "Log.h"
#include "Conf.h"
#include "Mem.h"
#include "termid_mask.h"
#include "Phrases.h"
#include "XmlDoc.h"

TermCheckList g_checkSpamList;


CheckSpam::CheckSpam(XmlDoc *xd, bool debug) :
	m_debbuf(NULL), m_debbufUsed(0), m_debbufSize(0), m_docMatchScore(-1),
	m_numUniqueMatchedWords(0), m_numUniqueMatchedPhrases(0), m_numWordsChecked(0),
	m_emptyDocumentBody(false), m_resultValid(false), m_result(false) {

	if( !xd ) {
		log(LOG_ERROR, "CheckSpam::CheckSpam passed NULL-pointer");
		gbshutdownLogicError();
	}

	m_url = xd->getFirstUrl();
	if( m_url == (Url *)-1 ) {
		m_url = NULL;
	}

	m_xml = xd->getXml();
	if( m_xml == (Xml *)-1 ) {
		m_xml = NULL;
	}

	m_tokenizerResult = xd->getTokenizerResult();
	if( m_tokenizerResult == (TokenizerResult*)-1 ) {
		m_tokenizerResult = NULL;
	}

	m_phrases = xd->getPhrases();
	if( m_phrases == (Phrases *)-1 ) {
		m_phrases = NULL;
	}
	
	if( debug ) {
		m_debbufSize = 2000;
		m_debbuf = (char *)mmalloc(m_debbufSize, "CheckSpam");
		if( m_debbuf ) {
			// zero-terminate now as we may not need it, but may try logging it later
			m_debbuf[0] = '\0';
		}
		else {
			m_debbufSize = 0;
		}
	}
}


CheckSpam::~CheckSpam() {
	if( m_debbuf ) {
		mfree(m_debbuf, m_debbufSize, "CheckSpam");
	}
}


int32_t CheckSpam::getScore() {
	return m_docMatchScore;
}

int32_t CheckSpam::getNumUniqueMatchedWords() {
	return m_numUniqueMatchedWords;
}

int32_t CheckSpam::getNumUniqueMatchedPhrases() {
	return m_numUniqueMatchedPhrases;
}

int32_t CheckSpam::getNumWordsChecked() {
	return m_numWordsChecked;
}

bool CheckSpam::hasEmptyDocumentBody() {
	return m_emptyDocumentBody;
}

const char *CheckSpam::getReason() {
	return m_reason.c_str();
}

const char *CheckSpam::getDebugInfo() {
	if( m_debbuf ) {
		return m_debbuf;
	}
	return "";
}




bool CheckSpam::isDocSpam() {
	// Hash table used to hold unique termIds to make sure we only count each unique word once
	HashTableX uniqueTermIds;

	if( m_resultValid ) {
		return m_result;
	}

	m_docMatchScore = 0;

	//
	// If not blocked by the cheaper checks, do the hard work and check document content
	//
	if( !m_docMatchScore ) {
		//
		// Score words and phrases from the document body text
		//

		if( m_tokenizerResult ) {
			if (!uniqueTermIds.set(sizeof(int64_t), 0, m_tokenizerResult->size()+5000, NULL, 0, false, "uniquetermids", false, 0)) {
				log(LOG_ERROR,"isDocSpam: Could not initialize uniqueTermIds hash table");
			}

			if( m_tokenizerResult->empty() ) {
				// No words in document body
				m_emptyDocumentBody = true;
			}
			else {
				g_checkSpamList.getScore(*m_tokenizerResult, m_phrases, &uniqueTermIds, &m_docMatchScore, &m_numUniqueMatchedWords, &m_numUniqueMatchedPhrases, m_debbuf, m_debbufUsed, m_debbufSize);
				m_numWordsChecked += m_tokenizerResult->size();
			}
			logTrace(g_conf.m_logTraceTermCheckList, "%zu words checked (%" PRId32 " unique) in body: %s. %" PRId32 " unique matched words, %" PRId32 " unique matched phrases. Score: %" PRId32 "",
				m_tokenizerResult->size(), uniqueTermIds.getNumUsedSlots(), m_url->getUrl(), m_numUniqueMatchedWords, m_numUniqueMatchedPhrases, m_docMatchScore);
		}
		else {
			// No words in document body
			m_emptyDocumentBody = true;
			logTrace(g_conf.m_logTraceTermCheckList, "Document body is empty in %s", m_url->getUrl());
		}

		//
		// Score words and phrases from the document meta tags
		//
		if( m_xml ) {
			TokenizerResult metatr;
			Bits metab;
			Phrases metap;
			int32_t mtlen;

			const char *mtag = m_xml->getMetaContentPointer( "keywords", 8, "name", &mtlen );
			if( mtlen > 0 ) {
				plain_tokenizer_phase_1(mtag,mtlen, &metatr);
			}
			mtag = m_xml->getMetaContentPointer( "description", 11, "name", &mtlen );
			if( mtlen > 0 ) {
				plain_tokenizer_phase_1(mtag,mtlen, &metatr);
			}
			if( !metatr.empty() ) {
				plain_tokenizer_phase_2(langUnknown, nullptr, &metatr);
				if( !metab.set(&metatr) ) {
					log(LOG_ERROR,"isDocSpam: Could not set bits for meta words");
				}
				if( !metap.set(&metatr, &metab) ) {
					log(LOG_ERROR,"isDocSpam: Could not set phrases for meta words");
				}
				g_checkSpamList.getScore(metatr, &metap, &uniqueTermIds, &m_docMatchScore, &m_numUniqueMatchedWords, &m_numUniqueMatchedPhrases, m_debbuf, m_debbufUsed, m_debbufSize);
				m_numWordsChecked += metatr.size();

				logTrace(g_conf.m_logTraceTermCheckList, "%zu words checked (%" PRId32 " unique) in meta tags: %s. %" PRId32 " unique matched words, %" PRId32 " unique matched phrases. Score: %" PRId32 "",
					metatr.size(), uniqueTermIds.getNumUsedSlots(), m_url->getUrl(), m_numUniqueMatchedWords, m_numUniqueMatchedPhrases, m_docMatchScore);
			}
		}

		//
		// Score words and phrases from URL
		//
		if( m_url ) {
			TokenizerResult urltr;
			Bits urlb;
			Phrases urlp;

			plain_tokenizer_phase_1(m_url->getUrl(), m_url->getUrlLen(), &urltr);
			if( !urlb.set(&urltr) ) {
				log(LOG_ERROR,"isDocSpam: Could not set bits for URL words");
			}
			if( !urlp.set(&urltr, &urlb) ) {
				log(LOG_ERROR,"isDocSpam: Could not set phrases for URL words");
			}
			g_checkSpamList.getScore(urltr, &urlp, &uniqueTermIds, &m_docMatchScore, &m_numUniqueMatchedWords, &m_numUniqueMatchedPhrases, m_debbuf, m_debbufUsed, m_debbufSize);
			m_numWordsChecked += urltr.size();

			logTrace(g_conf.m_logTraceTermCheckList, "%zu words checked (%" PRId32 " unique) in URL: %s. %" PRId32 " unique matched words, %" PRId32 " unique matched phrases. Score: %" PRId32 "", 
				urltr.size(), uniqueTermIds.getNumUsedSlots(), m_url->getUrl(), m_numUniqueMatchedWords, m_numUniqueMatchedPhrases, m_docMatchScore);
		}


		if( m_docMatchScore > 0 ) {
			m_reason = "spamTerms";
		}
	}

	logTrace(g_conf.m_logTraceTermCheckList, "Final score %" PRId32 " for: %s. %" PRId32 " unique matched words, %" PRId32 " unique matched phrases", 
		m_docMatchScore, m_url->getUrl(), m_numUniqueMatchedWords, m_numUniqueMatchedPhrases);

	m_result = false;
	if( ( m_docMatchScore >= 20 || m_numUniqueMatchedWords > 7) ||
		( m_docMatchScore >= 20 || m_numUniqueMatchedPhrases >= 3) ) {
		m_result = true;
	}
	m_resultValid = true;
	
	return m_result;
}


