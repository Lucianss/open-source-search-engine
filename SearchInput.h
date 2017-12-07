// Copyright Apr 2005 Matt Wells

// . parameters from CollectionRec that can be overriden on a per query basis

// . use m_qbuf1 for the query. it has all the min/plus/quote1/quote2 advanced
//   search cgi parms appended to it. it is the complete query, which is 
//   probably what you are looking for. m_q should also be set to that, too.
//   m_query is simply the "query=" string in the url request.
// . use m_qbuf2 for the spell checker. it is missing url: link: etc fields.
// . use m_displayQuery for the query displayed in the text box. it is missing
//   values from the "sites=" and "site=" cgi parms, since they show up with
//   radio buttons below the search text box on the web page.

#ifndef GB_SEARCHINPUT_H
#define GB_SEARCHINPUT_H

#include "Query.h" // MAX_QUERY_LEN
#include "HttpRequest.h"
#include <string>

class CollectionRec;

class SearchInput {
public:

	// why provide query here, it is in "hr"
	bool set ( class TcpSocket *s , class HttpRequest *hr );

	bool setQueryBuffers ( class HttpRequest *hr ) ;

	void clear();

	// Msg40 likes to use this to pass the parms to a remote host
	SearchInput      ( );
	~SearchInput     ( );

	void  copy                  ( class SearchInput *si ) ;

	std::string getPreferredResultLanguage();

	///////////
	//
	// BEGIN COMPUTED THINGS
	//
	///////////

	// we basically steal the original HttpRequest buffer and keep
	// here since the original one is on the stack
	HttpRequest  m_hr;

	int32_t   m_niceness;                   // msg40

	// array of collnum_t's to search... usually just one
	SafeBuf m_collnumBuf;

	const char          *m_displayQuery;     // pts into m_qbuf1

	// urlencoded display query. everything is compiled into this.
	SafeBuf m_qe;

	CollectionRec *m_cr;

	// the final compiled query goes here
	Query          m_q;

	bool           m_isMasterAdmin;
	bool           m_isCollAdmin;

	// these are set from things above
	SafeBuf m_sbuf1;
	SafeBuf m_sbuf2;


	// we convert m_defaultSortLang to this number, like langEnglish
	// or langFrench or langUnknown...
	lang_t m_queryLangId;

	// can be 1 for FORMAT_HTML, 2 = FORMAT_XML, 3=FORMAT_JSON, 4=csv
	int32_t m_format;

	// used as indicator by SearchInput::makeKey() for generating a
	// key by hashing the parms between m_START and m_END
	int32_t   m_START;


	//////
	//
	// BEGIN USER PARMS set from HttpRequest, m_hr
	//
	//////

	const char *m_coll;
	char *m_query;
	
	char *m_prepend;

	bool  m_showImages;

	// general parms, not part of makeKey(), but should be serialized
	char   m_useCache;                   // msg40. Can be -1
	bool   m_rcache;                     // msg40
	char   m_wcache;                     // msg40. Can be -1

	bool   m_debug;                      // msg40

	char  *m_displayMetas;               // msg40

	// do not include these in makeKey()

	char  *m_queryCharset;

	// advanced query parms
	char  *m_url; // for url: search
	char  *m_sites;
	char  *m_plus;
	char  *m_minus;
	char  *m_link;
	char  *m_quote1;
	char  *m_quote2;

	int32_t m_titleMaxLen;

	// for limiting results by score in the widget
	double    m_maxSerpScore;
	int64_t m_minSerpDocId;

	float m_sameLangWeight;
	float m_unknownLangWeight;
	float m_siteRankMultiplier;

	char *m_fx_qlang;
	char *m_fx_blang;
	char *m_fx_fetld;
	char *m_fx_country;

	// prefer what lang in the results. it gets a 20x boost. "en" "xx" "fr"
	char 	      *m_defaultSortLang;

	// general parameters
        char   m_dedupURL;
	int32_t   m_percentSimilarSummary;   // msg40
	bool   m_showBanned;
	int32_t   m_includeCachedCopy;
	bool   m_familyFilter;            // msg40
	bool   m_allowHighFrequencyTermCache;
	int64_t m_minMsg3aTimeout;
	bool	m_showErrors;
	bool   m_doSiteClustering;        // msg40
	bool   m_doDupContentRemoval;     // msg40
	bool   m_getDocIdScoringInfo;

	float m_termFreqWeightFreqMin;
	float m_termFreqWeightFreqMax;
	float m_termFreqWeightMin;
	float m_termFreqWeightMax;

	float m_diversityWeightMin;
	float m_diversityWeightMax;
	float m_densityWeightMin;
	float m_densityWeightMax;
	float m_hashGroupWeightBody;
	float m_hashGroupWeightTitle;
	float m_hashGroupWeightHeading;
	float m_hashGroupWeightInlist;
	float m_hashGroupWeightInMetaTag;
	float m_hashGroupWeightInLinkText;
	float m_hashGroupWeightInTag;
	float m_hashGroupWeightNeighborhood;
	float m_hashGroupWeightInternalLinkText;
	float m_hashGroupWeightInUrl;
	float m_hashGroupWeightInMenu;
	float m_synonymWeight;
	float m_bigramWeight;
	float m_pageTemperatureWeightMin;
	float m_pageTemperatureWeightMax;
	bool m_usePageTemperatureForRanking;

	int32_t m_numFlagScoreMultipliers;
	float m_flagScoreMultiplier[26];
	int32_t m_numFlagRankAdjustments;
	int m_flagRankAdjustment[26];
	
	bool   m_hideAllClustered;

	bool   m_askOtherShards;

	char   m_queryId[32];             //for log and correlation

	// ranking algos
	bool   m_doMaxScoreAlgo;

	// stream results back on socket in streaming mode, usefule when 
	// thousands of results are requested
	bool   m_streamResults;

	// limit search results to pages spidered this many seconds ago
	int32_t   m_secsBack;

	// 0 relevance, 1 date, 2 reverse date
	char   m_sortBy;

	char *m_filetype;

	// search result knobs
	int32_t   m_realMaxTop;

	// general parameters
	int32_t   m_numLinesInSummary;           // msg40
	int32_t   m_summaryMaxWidth;             // msg40
	int32_t   m_summaryMaxNumCharsPerLine;

	int32_t   m_docsWanted;                  // msg40
	int32_t   m_firstResultNum;              // msg40
	bool      m_doQueryHighlighting;         // msg40
	char  *m_highlightQuery;
	Query  m_hqq;

	// . buzz stuff (buzz)
	// . these controls the set of results, so should be in the makeKey()
	//   as it is, in between the start and end hash vars
	int32_t   m_displayInlinks;

	// we do not do summary deduping, and other filtering with docids
	// only, so can change the result and should be part of the key
	char   m_docIdsOnly;                 // msg40

	char  *m_formatStr;

	// this should be part of the key because it will affect the results!
	bool   m_queryExpansion;

	////////
	//
	// END USER PARMS
	//
	////////

	int32_t   m_END;
};

#endif // GB_SEARCHINPUT_H
