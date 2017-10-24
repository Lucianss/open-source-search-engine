#include "gb-include.h"

#include "SearchInput.h"
#include "Parms.h"         // g_parms
#include "Pages.h"         // g_msg
#include "CountryCode.h"
#include "PageResults.h"
#include "GbUtil.h"
#include "Collectiondb.h"
#include "Conf.h"

#include "third-party/cld2/public/compact_lang_det.h"
#include "third-party/cld2/public/encodings.h"

SearchInput::SearchInput() {
	// Coverity
	m_niceness = 0;
	m_displayQuery = NULL;
	m_cr = NULL;
	m_isMasterAdmin = false;
	m_isCollAdmin = false;
	m_queryLangId = 0;
	m_format = 0;
	m_START = 0;
	m_coll = NULL;
	m_query = NULL;
	m_prepend = NULL;
	m_showImages = false;
	m_useCache = -1;	// default from Param.cc
	m_rcache = false;
	m_wcache = -1;		// default from Param.cc
	m_debug = false;
	m_displayMetas = NULL;
	m_queryCharset = NULL;
	m_url = NULL;
	m_sites = NULL;
	m_plus = NULL;
	m_minus = NULL;
	m_link = NULL;
	m_quote1 = NULL;
	m_quote2 = NULL;
	m_imgUrl = NULL;
	m_imgLink = NULL;
	m_imgWidth = 0;
	m_imgHeight = 0;
	m_titleMaxLen = 0;
	m_maxSerpScore = 0.0;
	m_minSerpDocId = 0;
	m_sameLangWeight = 0.0;
	m_unknownLangWeight = 0.0;
	m_fx_qlang = nullptr;
	m_fx_blang = nullptr;
	m_fx_fetld = nullptr;
	m_fx_country = nullptr;
	m_defaultSortLang = NULL;
	m_dedupURL = 0;
	m_percentSimilarSummary = 0;
	m_showBanned = false;
	m_includeCachedCopy = 0;
	m_familyFilter = false;
	m_allowHighFrequencyTermCache = false;
	m_minMsg3aTimeout = 0;
	m_showErrors = false;
	m_doSiteClustering = false;
	m_doDupContentRemoval = false;
	m_getDocIdScoringInfo = false;
	m_hideAllClustered = false;
	m_askOtherShards = false;
	memset(m_queryId, 0, sizeof(m_queryId));
	m_doMaxScoreAlgo = false;

	m_termFreqWeightFreqMin = 0.0;
	m_termFreqWeightFreqMax = 0.5;
	m_termFreqWeightMin = 0.5;
	m_termFreqWeightMax = 1.0;

	m_synonymWeight = 0.9;
	m_bigramWeight  = 5.0;
	m_pageTemperatureWeightMin = 1.0;
	m_pageTemperatureWeightMax = 20.0;
	m_usePageTemperatureForRanking = true;
	m_numFlagScoreMultipliers=26;
	for(int i=0; i<26; i++)
		m_flagScoreMultiplier[i] = 1.0;
	m_numFlagRankAdjustments=26;
	for(int i=0; i<26; i++)
		m_flagRankAdjustment[i] = 0;
	m_streamResults = false;
	m_secsBack = 0;
	m_sortBy = 0;
	m_filetype = NULL;
	m_realMaxTop = 0;
	m_numLinesInSummary = 0;
	m_summaryMaxWidth = 0;
	m_summaryMaxNumCharsPerLine = 0;
	m_docsWanted = 0;
	m_firstResultNum = 0;
	m_doQueryHighlighting = false;
	m_highlightQuery = NULL;
	m_displayInlinks = 0;
	m_displayOutlinks = 0;
	m_docIdsOnly = 0;
	m_formatStr = NULL;
	m_queryExpansion = false;
	m_END = 0;
}

SearchInput::~SearchInput() {
}

void SearchInput::clear () {
	// set all to 0 just to avoid any inconsistencies
	int32_t size = (char *)&m_END - (char *)&m_START;
	memset ( &m_START , 0x00 , size );
	m_sbuf1.reset();
	m_sbuf2.reset();

	// set these
	m_numLinesInSummary  = 2;
	m_docsWanted         = 10;
	m_niceness           = 0;
}

void SearchInput::copy ( class SearchInput *si ) {
	gbmemcpy ( (char *)this , (char *)si , sizeof(SearchInput) );
}

bool SearchInput::set ( TcpSocket *sock , HttpRequest *r ) {
	// store list of collection #'s to search here. usually just one.
	m_collnumBuf.reset();

	m_q.reset();

	// zero out everything
	clear();

	m_hr.copy(r);

	const char *coll = g_collectiondb.getDefaultColl(r->getString("c"));

	//////
	//
	// build "m_collnumBuf" to consist of all the collnums we should
	// be searching.
	//
	///////

	// set this to the collrec of the first valid collnum we encounter
	CollectionRec *cr = NULL;
	// now convert list of space-separated coll names into list of collnums
	const char *p = r->getString("c",NULL);

	// if we had a "&c=..." in the GET request process that
	if ( p ) {
		do {
			const char *end = p;
			for ( ; *end && ! is_wspace_a(*end) ; end++ );
			CollectionRec *tmpcr = g_collectiondb.getRec ( p, end-p );
			// set defaults from the FIRST one
			if ( tmpcr && ! cr ) {
				cr = tmpcr;
			}
			if ( ! tmpcr ) {
				g_errno = ENOCOLLREC;
				log("query: missing collection %*.*s",(int)(end-p),(int)(end-p),p);
				g_msg = " (error: no such collection)";		
				return false;
			}
			// add to our list
			if (!m_collnumBuf.safeMemcpy(&tmpcr->m_collnum,
						     sizeof(collnum_t)))
				return false;
			// advance
			p = end;
			// skip to next collection name if there is one
			while ( *p && is_wspace_a(*p) ) p++;
			// now add it's collection # to m_collnumBuf if there
		} while(*p);
	}

	// use default collection if none provided
	if ( ! p && m_collnumBuf.length() <= 0 ) {
		// get default collection rec
		cr = g_collectiondb.getRec (coll);
		// add to our list
		if ( cr &&
		     !m_collnumBuf.safeMemcpy(&cr->m_collnum,
					      sizeof(collnum_t)))
			return false;
	}
		


	/////
	//
	// END BUILDING m_collnumBuf
	//
	/////


	// save the collrec
	m_cr = cr;

	// must have had one
	if ( ! cr ) {
		log("si: si. collection does not exist");
		// if we comment the below out then it cores in setToDefault!
		g_errno = ENOCOLLREC;
		return false;
	}

	// and set from the http request. will set m_coll, etc.
	g_parms.setToDefault ( (char *)this , OBJ_SI , cr );


	///////
	//
	// set defaults of some things based on format language
	//
	//////

	// get the format. "html" "json" --> FORMAT_HTML, FORMAT_JSON ...
	char tmpFormat = m_hr.getReplyFormat();
	// now override automatic defaults for special cases
	if ( tmpFormat != FORMAT_HTML ) {
		m_doQueryHighlighting = false;
		m_getDocIdScoringInfo = false;
	}

	// if they have a list of sites...
	if ( m_sites && m_sites[0] ) {
		m_doSiteClustering        = false;
	}

	// and set from the http request. will set m_coll, etc.
	g_parms.setFromRequest ( &m_hr , sock , cr , (char *)this , OBJ_SI );

	if ( m_streamResults &&
	     tmpFormat != FORMAT_XML &&
	     tmpFormat != FORMAT_JSON ) {
		log("si: streamResults only supported for "
		    "xml/csv/json. disabling");
		m_streamResults = false;
	}

	m_coll = coll;

	// it sets m_formatStr above, but we gotta set this...
	m_format = tmpFormat;


	//////
	//
	// fix some parms
	//
	//////

	// set m_isMasterAdmin to zero if no correct ip or password
	if ( ! g_conf.isMasterAdmin ( sock , &m_hr ) ) {
		m_isMasterAdmin = false;
	}

	// collection admin?
	m_isCollAdmin = g_conf.isCollAdmin ( sock , &m_hr );

	//////////////////////////////////////
	//
	// transform input into classes
	//
	//////////////////////////////////////

	// . returns false and sets g_errno on error
	// . sets m_qbuf1 and m_qbuf2
	// . sets:
	//   m_sbuf1
	//   m_sbuf2
	//   m_sbuf3
	//   m_displayQuery
	//   m_qe (encoded query)
	//   m_rtl (right to left like hebrew)
	//   m_highlightQuery
	if ( ! setQueryBuffers (r) ) {
		log(LOG_WARN, "query: setQueryBuffers: %s",mstrerror(g_errno));
		return false;
	}

	// this parm is in Parms.cpp and should be set
	const char *langAbbr = m_defaultSortLang;

	// Parms.cpp sets it to an empty string, so make that null
	// if Parms.cpp set it to NULL it seems it comes out as "(null)"
	// i guess because we sprintf it or something.
	if ( langAbbr && langAbbr[0] == '\0' ) {
		langAbbr = NULL;
	}

	// detect language
	if ( !langAbbr ) {
		// detect language hints

		// language tag format:
		//   Language-Tag = Primary-tag *( "-" Subtag )
		//   Primary-tag = 1*8ALPHA
		//   Subtag = 1*8ALPHA
		char content_language_hint[64] = {}; // HTTP header Content-Language: field
		const char *tld_hint = NULL; // hostname of a URL

		bool valid_qlang = false;
		{
			if (m_fx_qlang) {
				// validate lang
				if (strlen(m_fx_qlang) == 2) {
					valid_qlang = true;
					strcat(content_language_hint, m_fx_qlang);
				}
			}
		}

		// only use other hints if fx_qlang is not set
		if (!valid_qlang) {
			if (m_fx_blang) {
				// validate lang
				size_t len = strlen(m_fx_blang);
				if (len > 0 && len <= 17) {
					strcat(content_language_hint, m_fx_blang);
				}
			}

			// use fx_fetld if available; if not, try with fx_country
			tld_hint = m_fx_fetld;
			if (!tld_hint || strlen(tld_hint) == 0) {
				tld_hint = m_fx_country;
			}
		}

		int encoding_hint = CLD2::UNKNOWN_ENCODING; // encoding detector applied to the input document
		CLD2::Language language_hint = CLD2::UNKNOWN_LANGUAGE; // any other context

		//log("query: cld2: using content_language_hint='%s' tld_hint='%s'", content_language_hint, tld_hint);

		CLD2::CLDHints cldhints = {content_language_hint, tld_hint, encoding_hint, language_hint};

		int flags = 0;
		flags |= CLD2::kCLDFlagBestEffort;

		// this is initialized by CLD2 library
		CLD2::Language language3[3];
		int percent3[3];
		double normalized_score3[3];

		CLD2::ResultChunkVector *resultchunkvector = NULL;

		int text_bytes = 0;
		bool is_reliable = false;
		int valid_prefix_bytes = 0;

		CLD2::Language language = CLD2::ExtDetectLanguageSummaryCheckUTF8(m_sbuf1.getBufStart(),
		                                                                  m_sbuf1.length(),
		                                                                  true,
		                                                                  &cldhints,
		                                                                  flags,
		                                                                  language3,
		                                                                  percent3,
		                                                                  normalized_score3,
		                                                                  resultchunkvector,
		                                                                  &text_bytes,
		                                                                  &is_reliable,
		                                                                  &valid_prefix_bytes);

		//log("query: cld2: lang0: %s(%d%% %3.0fp)", CLD2::LanguageCode(language3[0]), percent3[0], normalized_score3[0]);
		//log("query: cld2: lang1: %s(%d%% %3.0fp)", CLD2::LanguageCode(language3[1]), percent3[1], normalized_score3[1]);
		//log("query: cld2: lang2: %s(%d%% %3.0fp)", CLD2::LanguageCode(language3[2]), percent3[2], normalized_score3[2]);

		if (language != CLD2::UNKNOWN_LANGUAGE) {
			langAbbr = CLD2::LanguageCode(language);
		}
	}

	// if &qlang was not given explicitly fall back to coll rec
	if (!langAbbr) {
		langAbbr = cr->m_defaultSortLanguage2;
	}

	log(LOG_INFO,"query: using default lang of %s", langAbbr );

	// get code
	m_queryLangId = getLangIdFromAbbr ( langAbbr );

	// allow for 'xx', which means langUnknown
	if ( m_queryLangId == langUnknown &&
	     langAbbr[0] &&
	     langAbbr[0]!='x' ) {
		log("query: langAbbr of '%s' is NOT SUPPORTED. using langUnknown, 'xx'.", langAbbr);
	}

	int32_t maxQueryTerms = cr->m_maxQueryTerms;

	// . the query to use for highlighting... can be overriden with "hq"
	// . we need the language id for doing synonyms
	if ( m_prepend && m_prepend[0] )
		m_hqq.set2 ( m_prepend , m_queryLangId , m_queryExpansion , true, m_allowHighFrequencyTermCache, maxQueryTerms);
	else if ( m_highlightQuery && m_highlightQuery[0] )
		m_hqq.set2 (m_highlightQuery,m_queryLangId,m_queryExpansion, true, m_allowHighFrequencyTermCache, maxQueryTerms);
	else if ( m_query && m_query[0] )
		m_hqq.set2 ( m_query , m_queryLangId , m_queryExpansion, true, m_allowHighFrequencyTermCache, maxQueryTerms);

	// log it here
	log(LOG_INFO, "query: got query %s (len=%i)" ,m_sbuf1.getBufStart() ,m_sbuf1.length());

	// . now set from m_qbuf1, the advanced/composite query buffer
	// . returns false and sets g_errno on error (?)
	if ( ! m_q.set2 ( m_sbuf1.getBufStart(),
			  m_queryLangId ,
			  m_queryExpansion ,
			  true , // use QUERY stopwords?
			  m_allowHighFrequencyTermCache,
			  maxQueryTerms ) ) {
		g_msg = " (error: query has too many operands)";
		return false;
	}

	if ( m_q.m_truncated && m_q.m_isBoolean ) {
		g_errno = EQUERYTOOBIG;
		g_msg = " (error: query is too long)";
		return false;
	}


	if ( m_hideAllClustered )
		m_doSiteClustering = true;

	// turn off some parms
	if ( m_q.m_hasPositiveSiteField ) {
		m_doSiteClustering    = false;
	}

	if ( ! m_doSiteClustering )
		m_hideAllClustered = false;

	// sanity check
	if(m_firstResultNum < 0) {
		m_firstResultNum = 0;
	}

	// . if query has url: or site: term do NOT use cache by def.
	// . however, if spider is off then use the cache by default
	if ( m_useCache == -1 && g_conf.m_spideringEnabled ) {
		if      ( m_q.m_hasPositiveSiteField ) m_useCache = 0;
		else if ( m_q.m_hasIpField   ) m_useCache = 0;
		else if ( m_q.m_hasUrlField  ) m_useCache = 0;
		else if ( m_sites && m_sites[0] ) m_useCache = 0;
		//else if ( m_whiteListBuf.length() ) m_useCache = 0;
		else if ( m_url && m_url[0]   ) m_useCache = 0;
	}

	// if useCache is still -1 then turn it on
	if ( m_useCache == -1 ) m_useCache = 1;

	bool readFromCache = false;
	if ( m_useCache ==  1  ) readFromCache = true;
	if ( !m_rcache ) readFromCache = false;
	if ( m_useCache ==  0  ) readFromCache = false;

	// if useCache is false, don't write to cache if it was not specified
	if ( m_wcache == -1 ) {
		if ( m_useCache ==  0 ) m_wcache = 0;
		else                    m_wcache = 1;
	}

	// save it
	m_rcache = readFromCache;

	return true;
}

// . sets m_qbuf1[] and m_qbuf2[]
// . m_qbuf1[] is the advanced query
// . m_qbuf2[] is the query to be used for spell checking
// . returns false and set g_errno on error
bool SearchInput::setQueryBuffers ( HttpRequest *hr ) {

	m_sbuf1.reset();
	m_sbuf2.reset();

	int16_t qcs = csUTF8;
	if (m_queryCharset && m_queryCharset[0]){
		// we need to convert the query string to utf-8
		int32_t qclen = strlen(m_queryCharset);
		qcs = get_iana_charset(m_queryCharset, qclen );
		if (qcs == csUnknown) {
			qcs = csUTF8;
		}
	}

	// prepend
	const char *qp = hr->getString("prepend",NULL,NULL);
	if( qp && qp[0] ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf( "%s", qp );
	}

	// boolean OR terms
	bool boolq = false;
	const char *any = hr->getString("any",NULL);
	bool first = true;
	if ( any ) {
		const char *s = any;
		const char *send = any + strlen(any);
	 	if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
	 	if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		while (s < send) {
			while (isspace(*s) && s < send) s++;
			const char *s2 = s+1;
			if (*s == '\"') {
				// if there's no closing quote just treat
				// the end of the line as such
				while (*s2 != '\"' && s2 < send) s2++;
				if (s2 < send) s2++;
			} else {
				while (!isspace(*s2) && s2 < send) s2++;
			}
			if ( first ) {
				m_sbuf1.safeStrcpy("(");
				m_sbuf2.safeStrcpy("(");
			}
			if ( ! first ) {
				m_sbuf1.safeStrcpy(" OR ");
				m_sbuf2.safeStrcpy(" OR ");
			}
			first = false;
			m_sbuf1.safeMemcpy ( s , s2 - s );
			m_sbuf2.safeMemcpy ( s , s2 - s );
			s = s2 + 1;
		}
	}
	if ( ! first ) {
		m_sbuf1.safeStrcpy(") AND ");
		m_sbuf2.safeStrcpy(") AND ");
		boolq = true;
	}

	// and this
	if ( m_secsBack > 0 ) {
		int32_t timestamp = getTimeGlobalNoCore();
		timestamp -= m_secsBack;
		if ( timestamp <= 0 ) timestamp = 0;
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("gbminint:gbspiderdate:%" PRIu32,timestamp);
	}

	if ( m_sortBy == 1 ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("gbsortbyint:gbspiderdate");
	}

	if ( m_sortBy == 2 ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("gbrevsortbyint:gbspiderdate");
	}

	char *ft = m_filetype;
	if ( ft && strcasecmp(ft,"any")==0 ) ft = NULL;
	if ( ft && ! ft[0] ) ft = NULL;
	if ( ft ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("filetype:%s",ft);
	}

	// PRE-pend gblang: term
	int32_t gblang = hr->getLong("gblang",-1);
	if( gblang >= 0 ) {
	 	if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
	 	if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		m_sbuf1.safePrintf( "+gblang:%" PRId32, gblang );
		m_sbuf2.safePrintf( "+gblang:%" PRId32, gblang );
		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" |");
			m_sbuf2.safeStrcpy(" |");
		}
		else {
			m_sbuf1.safeStrcpy(" AND ");
			m_sbuf2.safeStrcpy(" AND ");
		}
	}

	// append url: term
	if ( m_link && m_link[0] ) {
	 	if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
	 	if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		m_sbuf1.safeStrcpy ( "+link:");
		m_sbuf2.safeStrcpy ( "+link:");
		m_sbuf1.safeStrcpy ( m_link );
		m_sbuf2.safeStrcpy ( m_link );
		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" |");
			m_sbuf2.safeStrcpy(" |");
		}
		else {
			m_sbuf1.safeStrcpy(" AND ");
			m_sbuf2.safeStrcpy(" AND ");
		}
	}
	m_sbuf1.setLabel("sisbuf1");
	m_sbuf2.setLabel("sisbuf2");
	// append the natural query
	if ( m_query && m_query[0] ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safeStrcpy ( m_query );
		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		m_sbuf2.safeStrcpy ( m_query );
	}

	// append quoted phrases to query
	if ( m_quote1 && m_quote1[0] ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" +\"");
			m_sbuf2.safeStrcpy(" +\"");
		}
		else {
			m_sbuf1.safeStrcpy(" AND \"");
			m_sbuf2.safeStrcpy(" AND \"");
		}
		m_sbuf1.safeStrcpy ( m_quote1 );
		m_sbuf1.safeStrcpy("\"");

		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');

		m_sbuf2.safeStrcpy ( m_quote1 );
		m_sbuf2.safeStrcpy("\"");
	}

	if ( m_quote2 && m_quote2[0] ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');

		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" +\"");
			m_sbuf2.safeStrcpy(" +\"");
		}
		else {
			m_sbuf1.safeStrcpy(" AND \"");
			m_sbuf2.safeStrcpy(" AND \"");
		}

		m_sbuf1.safeStrcpy ( m_quote2 );
		m_sbuf1.safeStrcpy("\"");

		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');

		m_sbuf2.safeStrcpy ( m_quote2 );
		m_sbuf2.safeStrcpy("\"");
	}

	// append plus terms
	if ( m_plus && m_plus[0] ) {
		char *s = m_plus;
		char *send = m_plus + strlen(m_plus);

		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		while (s < send) {
			while (isspace(*s) && s < send) s++;
			char *s2 = s+1;
			if (*s == '\"') {
				// if there's no closing quote just treat
				// the end of the line as such
				while (*s2 != '\"' && s2 < send) s2++;
				if (s2 < send) s2++;
			} else {
				while (!isspace(*s2) && s2 < send) s2++;
			}

			if ( ! boolq ) {
				m_sbuf1.safeStrcpy("+");
				m_sbuf2.safeStrcpy("+");
			}
			else {
				m_sbuf1.safeStrcpy(" AND ");
				m_sbuf2.safeStrcpy(" AND ");
			}

			m_sbuf1.safeMemcpy ( s , s2 - s );
			m_sbuf2.safeMemcpy ( s , s2 - s );

			s = s2 + 1;
			if (s < send) {
				if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
				if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
			}
		}

	}  
	// append minus terms
	if ( m_minus && m_minus[0] ) {
		char *s = m_minus;
		char *send = m_minus + strlen(m_minus);
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		while (s < send) {
			while (isspace(*s) && s < send) s++;
			char *s2 = s+1;
			if (*s == '\"') {
				// if there's no closing quote just treat
				// the end of the line as such
				while (*s2 != '\"' && s2 < send) s2++;
				if (s2 < send) s2++;
			} else {
				while (!isspace(*s2) && s2 < send) s2++;
			}
			if (s2 < send) break;

			if ( ! boolq ) {
				m_sbuf1.safeStrcpy("-");
				m_sbuf2.safeStrcpy("-");
			}
			else {
				m_sbuf1.safeStrcpy(" AND NOT ");
				m_sbuf2.safeStrcpy(" AND NOT ");
			}

			m_sbuf1.safeMemcpy ( s , s2 - s );
			m_sbuf2.safeMemcpy ( s , s2 - s );

			s = s2 + 1;
			if (s < send) {
				if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
				if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
			}
		}
	}
	// append gbkeyword:numinlinks if they have &mininlinks=X, X>0
	int32_t minInlinks = m_hr.getLong("mininlinks",0);
	if ( minInlinks > 0 ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf ( "gbkeyword:numinlinks");
	}

	// null terms
	if ( ! m_sbuf1.nullTerm() ) return false;
	if ( ! m_sbuf2.nullTerm() ) return false;

	// the natural query
	m_displayQuery = m_sbuf2.getBufStart();

	if ( ! m_displayQuery ) m_displayQuery = "";

	while ( *m_displayQuery == ' ' ) m_displayQuery++;

	// urlencoded display query
	urlEncode(&m_qe, m_displayQuery);

	return true;
}
