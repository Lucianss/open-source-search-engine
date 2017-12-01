// Matt Wells, copyright Aug 2003

// Query is a class for parsing queries

#ifndef GB_QUERY_H
#define GB_QUERY_H

#include "SafeBuf.h"
#include "Lang.h"
#include "WordVariationsConfig.h"
class CollectionRec;


// support big OR queries for image shingles
#define ABS_MAX_QUERY_LEN 62000

// raise for crazy bool query on diffbot
// seems like we alloc just enough to hold our words now so that this
// is really a performance capper but it is used in Summary.cpp
// and Matches.h so don't go too big just yet
#define ABS_MAX_QUERY_WORDS 99000

// . how many IndexLists might we get/intersect
// . we now use a int64_t to hold the query term bits for non-boolean queries
#define ABS_MAX_QUERY_TERMS 9000

#define GBUF_SIZE (16*1024)

// let's support up to 64 query terms for now
typedef uint64_t qvec_t;

#define MAX_OVEC_SIZE 256

// field codes
enum field_code_t {
	FIELD_UNSET             =  0,
	FIELD_URL               =  1,
	FIELD_LINK              =  2,
	FIELD_SITE              =  3,
	FIELD_IP                =  4,
	FIELD_SUBURL            =  5,
	FIELD_TITLE             =  6,
	FIELD_TYPE              =  7,
	FIELD_EXT               = 21,
	//FIELD_COLL            = 22,
	//FIELD_UNUSED          = 23,
	FIELD_LINKS             = 24,
	FIELD_SITELINK          = 25,
	// non-standard field codes
	//FIELD_UNUSED          =  8,
	//FIELD_UNUSED          =  9,
	//FIELD_UNUSED          = 10,
	//FIELD_UNUSED          = 11,
	//FIELD_UNUSED          = 12,
	//FIELD_UNUSED          = 13,
	//FIELD_UNUSED          = 14,
	//FIELD_UNUSED          = 15,
	//FIELD_UNUSED          = 16,
	//FIELD_UNUSED          = 17,
	FIELD_GENERIC           = 18,
	//FIELD_UNUSED          = 19,
	//FIELD_UNUSED          = 20,
	//FIELD_UNUSED          = 30,
	//FIELD_UNUSED          = 31,
	//FIELD_UNUSED          = 32,
	//FIELD_UNUSED          = 33,
	//FIELD_UNUSED          = 34,
	//FIELD_UNUSED          = 35,
	FIELD_GBLANG            = 36,
	//FIELD_UNUSED          = 37,
	//FIELD_UNUSED          = 38,
	//FIELD_UNUSED          = 39,
	//FIELD_UNUSED          = 40,
	//FIELD_UNUSED          = 41,
	//FIELD_UNUSED          = 42,
	//FIELD_UNUSED          = 43,
	//FIELD_UNUSED          = 44,
	//FIELD_UNUSED          = 45,
	FIELD_GBCOUNTRY         = 46,
	//FIELD_UNUSED          = 47,
	//FIELD_UNUSED          = 48,
	//FIELD_UNUSED          = 49,
	//FIELD_UNUSED          = 50,
	FIELD_GBTERMID          = 50,
	//FIELD_UNUSED          = 51,
	FIELD_GBDOCID           = 52,
	FIELD_GBCONTENTHASH     = 53, // for deduping at spider time
	FIELD_GBSORTBYFLOAT     = 54, // i.e. sortby:price -> numeric termlist
	FIELD_GBREVSORTBYFLOAT  = 55, // i.e. sortby:price -> low to high
	FIELD_GBNUMBERMIN       = 56,
	FIELD_GBNUMBERMAX       = 57,
	//FIELD_UNUSED          = 58,
	FIELD_GBSORTBYINT       = 59,
	FIELD_GBREVSORTBYINT    = 60,
	FIELD_GBNUMBERMININT    = 61,
	FIELD_GBNUMBERMAXINT    = 62,
	//FIELD_UNUSED          = 63,
	//FIELD_UNUSED          = 64,
	//FIELD_UNUSED          = 65,
	FIELD_GBNUMBEREQUALINT  = 66,
	FIELD_GBNUMBEREQUALFLOAT= 67,
	//FIELD_UNUSED          = 68,
	FIELD_GBFIELDMATCH      = 69,
};

// returns a FIELD_* code above, or FIELD_GENERIC if not in the list
field_code_t getFieldCode(const char *s, int32_t len, bool *hasColon = NULL);

int32_t getNumFieldCodes ( );

class Query;
class ScoringWeights;

// . values for QueryField::m_flag
// . QTF_DUP means it is just for the help page in PageRoot.cpp to 
//   illustrate a second or third example
#define QTF_DUP  0x01
#define QTF_HIDE 0x02
#define QTF_BEGINNEWTABLE 0x04

struct QueryField {
	const char *text;
	field_code_t field;
	bool hasColon;
	const char *example;
	const char *desc;
	const char *m_title;
	char  m_flag;
};

extern const struct QueryField g_fields[];
	
// reasons why we ignore a particular QueryWord's word or phrase
enum ignore_reason_t {
	IGNORE_NO_IGNORE = 0,
	IGNORE_DEFAULT   = 1,	// punct
	IGNORE_CONNECTED = 2,	// connected sequence (cd-rom)
	IGNORE_QSTOP     = 3,	// query stop word (come 'to' me)
	IGNORE_REPEAT    = 4,	// repeated term (time after time)
	IGNORE_FIELDNAME = 5,	// word is a field name, like title:
	IGNORE_BREECH    = 6,	// query exceeded MAX_QUERY_TERMS so we ignored part
	IGNORE_BOOLOP    = 7,	// boolean operator (OR,AND,NOT)
	IGNORE_QUOTED    = 8,	// word in quotes is ignored. "the day"
	IGNORE_HIGHFREMTERM = 9	//trem(word) is a high-freq-term and is too expensive to look up
};

// boolean query operators (m_opcode field in QueryWord)
#define OP_OR         1
#define OP_AND        2
#define OP_NOT        3
#define OP_LEFTPAREN  4
#define OP_RIGHTPAREN 5
#define OP_UOR        6
#define OP_PIPE       7

// . these first two classes are functionless
// . QueryWord, like the Phrases class, is an extension on the Words class
// . the array of QueryWords, m_qwords[], is contained in the Query class
// . we compute the QueryTerms (m_qterms[]) from the QueryWords
class QueryWord {

 public:
	bool isAlphaWord() const { return is_alnum_utf8(m_word); }

	void constructor ();
	void destructor ();

	// this ptr references into the actual query
	const char     *m_word;
	int32_t        m_wordLen ;
	// the length of the phrase, if any. it starts at m_word. It can be less than the source string length because
	// non-alfanum words are treated as a single space by Phrases::getPhrase(), so eg "aaa::bbb" will only have m_phraseLen=7
	int32_t        m_phraseLen;
	// this is the term hash with collection and field name and
	// can be looked up directly in indexdb
	int64_t   m_wordId ;
	int64_t   m_phraseId;
	// hash of field name then collection, used to hash termId
	int64_t   m_prefixHash;
	int32_t        m_posNum;
	// are we in a phrase in a wikipedia title?
	int32_t        m_wikiPhraseId;

	// . this is just the hash of m_term and is used for highlighting, etc.
	// . it is 0 for terms in a field?
	int64_t   m_rawWordId ;
	int64_t   m_rawPhraseId ;

	// the field as a convenient numeric code
	field_code_t m_fieldCode;
	// . '-' means to exclude from search results
	// . '+' means to include in all search results
	// . if we're a phrase term, signs distribute across quotes
	char        m_wordSign;
	char        m_phraseSign;
	// the parenthetical level of this word in the boolean expression.
	// level 0 is the first level.
	char        m_level;
	// is this word a query stop word?
	bool        m_isQueryStopWord ; 
	// is it a plain stop word?
	bool        m_isStopWord ; 
	bool        m_isPunct;
	// are we an op code?
	char        m_opcode;
	// . the ignore code
	// . explains why this query term should be ignored
	// . see IGNORE_* enums above
	ignore_reason_t m_ignoreWord;
	ignore_reason_t m_ignorePhrase;

	// so we ignore gbsortby:offerprice in bool expressions
	bool        m_ignoreWordInBoolQuery;

	// is this query single word in quotes?
	bool        m_inQuotes ; 
	// is this word in a phrase that is quoted?
	bool        m_inQuotedPhrase;
	// what word # does the quote we are in start at?
	int32_t        m_quoteStart;
	int32_t        m_quoteEnd; // inclusive!
	// are we connected to the alnum word on our left/right?
	bool        m_leftConnected;
	bool        m_rightConnected;
	// if we're in middle or right end of a phrase, where does it start?
	int32_t        m_leftPhraseStart;
	// . what QueryTerm does our "phrase" map to? NULL if none.
	// . this allows us to OR in extra bits into that QueryTerm's m_bits
	//   member that correspond to the single word constituents
	// . remember, m_bits is a bit vector that represents the QueryTerms
	//   a document contains
	class QueryTerm *m_queryPhraseTerm;
	// . what QueryTerm does our "word" map to? NULL if none.
	// . used by QueryBoolean since it uses QueryWords heavily
	class QueryTerm *m_queryWordTerm;

	// user defined weights
	int32_t m_userWeightForWord;
	float m_userWeightForPhrase;

	bool m_queryOp;
	// is this query word before a | (pipe) operator?
	bool m_piped;

	// for min/max score ranges like gbmin:price:1.99
	float m_float;

	// for gbminint:99 etc. uses integers instead of floats for better res
	int32_t  m_int;

	// for holding some synonyms
	SafeBuf m_synWordBuf;

	// when an operand is an expression...
	class Expression *m_expressionPtr;
};

// . we filter the QueryWords and turn them into QueryTerms
// . QueryTerms are the important parts of the QueryWords
class QueryTerm {
 public:
	void constructor ( ) ;

	// the query word we were derived from
	const QueryWord *m_qword;

	// . are we a phrase termid or single word termid from that QueryWord?
	// . the QueryWord instance represents both, so we must choose
	bool       m_isPhrase;

	// this is phraseId for phrases, and wordId for words
	int64_t  m_termId;

	// used by Matches.cpp
	int64_t  m_rawTermId;

	// sign of the phrase or word we used
	char       m_termSign;

	// the "number" of the query term used for evaluation boolean
	// expressions in Expression::isTruth(). Basically just the
	// QueryTermInfo for which this query term belongs. each QueryTermInfo
	// is like a single query term and all its synonyms, etc.
	int32_t       m_bitNum;

	// point to term, either m_word or m_phrase
	const char      *m_term;
	int32_t       m_termLen;

	// point to the posdblist that represents us
	class RdbList   *m_posdbListPtr;

	// languages query term is in. currently this is only valid for
	// synonyms of other query terms. so we can show what language the
	// synonym is for in the xml/json feed.
	uint64_t m_langIdBits;
	bool m_langIdBitsValid;

	int64_t   m_termFreq;
	float     m_termFreqWeight;

	// Summary.cpp and Matches.cpp use this one
	bool m_isQueryStopWord ; 

	// IndexTable.cpp uses this one
	bool m_inQuotes;

	//base weight of this term. normally 1.0 for regular terms. Less of synonyms, more for bigrams
	float m_termWeight;
	
	// user defined weight for this term, be it phrase or word
	float m_userWeight;

	// . is this query term before a | (pipe) operator?
	// . if so we must read the whole termlist
	bool m_piped;

	// . we ignore component terms unless their compound term is not cached
	// . now this is used to ignore low tf synonym terms only
	bool m_ignored ;

	// . if synonymOf is not NULL, then m_term points into m_synBuf, not
	//   m_buf
	const QueryTerm *m_synonymOf;
	int64_t m_synWids0;
	int64_t m_synWids1;
	int32_t      m_numAlnumWordsInSynonym;

	// copied from derived QueryWord
	field_code_t m_fieldCode;
	bool isSplit() const;
	bool m_isRequired;

	bool m_isWikiHalfStopBigram;

	// if a single word term, what are the term #'s of the 2 phrases
	// we can be in? uses -1 to indicate none.
	int32_t  m_leftPhraseTermNum;
	int32_t  m_rightPhraseTermNum;

	// same as above basically
	const QueryTerm *m_leftPhraseTerm;
	const QueryTerm *m_rightPhraseTerm;

	char m_startKey[MAX_KEY_BYTES];
	char m_endKey  [MAX_KEY_BYTES];
};

#define MAX_EXPRESSIONS 100

// operand1 AND operand2 OR  ...
// operand1 OR  operand2 AND ...
class Expression {
public:
	bool addExpression (int32_t start, 
			    int32_t end, 
			    Query   *q,
			    int32_t    level );
	bool isTruth(const unsigned char *bitVec, int32_t vecSize) const;
	// . what QueryTerms are UNDER the influence of the NOT opcode?
	// . we read in the WHOLE termlist of those that are (like '-' sign)
	// . returned bit vector is 1-1 with m_qterms in Query class

	int32_t m_expressionStartWord;
	int32_t m_numWordsInExpression;
	const Query *m_q;
};

// . this is the main class for representing a query
// . it contains array of QueryWords (m_qwords[]) and QueryTerms (m_qterms[])
class Query {

 public:
	void reset();

	Query();
	~Query();

	// . returns false and sets g_errno on error
	// . after calling this you can call functions below
	bool set2 ( const char *query    , 
		    lang_t  langId ,
		    float  bigramWeight,
		    float  synonymWeight,
		    const WordVariationsConfig *wordVariationsConfig, //NULL=disable variations
		    bool     useQueryStopWords,
	        bool allowHighFreqTermCache,
		    int32_t  maxQueryTerms);

	const char *getQuery() const { return m_originalQuery.getBufStart(); }
	int32_t     getQueryLen() const { return m_originalQuery.length(); }

	int32_t     getNumTerms() const { return m_numTerms; }
	char        getTermSign(int32_t i) const { return m_qterms[i].m_termSign; }
	bool        isPhrase(int32_t i) const { return m_qterms[i].m_isPhrase; }
	int64_t     getTermId(int32_t i) const { return m_qterms[i].m_termId; }
	int64_t     getRawTermId (int32_t i) const { return m_qterms[i].m_rawTermId; }
	const char *getTerm(int32_t i) const { return m_qterms[i].m_term; }
	int32_t     getTermLen(int32_t i) const { return m_qterms[i].m_termLen; }
	bool        isSplit() const;

	// the new way as of 3/12/2014. just determine if matches the bool
	// query or not. let's try to offload the scoring logic to other places
	// if possible.
	// bitVec is all the QueryWord::m_opBits some docid contains, so
	// does it match our boolean query or not?
	bool matchesBoolQuery(const unsigned char *bitVec, int32_t vecSize) const;

	// modify query terms based on patters and rule-of-thumb. Eg "example.com" is probably a search
	// for a domain and "file.open()" is probably for an API/SDK
	void modifyQuery(ScoringWeights *scoringWeights, const CollectionRec& cr, bool *doSiteClustering);

	void dumpToLog() const;

private:
	// sets m_qwords[] array, this function is the heart of the class
	bool setQWords ( char boolFlag , bool keepAllSingles ,
			 class Words &words , class Phrases &phrases ) ;

	// sets m_qterms[] array from the m_qwords[] array
	bool setQTerms ( const class Words &words ) ;

	// helper funcs for parsing query into m_qwords[]
	bool        isConnection(const char *s, int32_t len) const;

	void traceTermsToLog(const char *header);

public:
	const char *originalQuery() const { return m_originalQuery.getBufStart(); }

	// hash of all the query terms
	int64_t getQueryHash() const;

	// return -1 if does not exist in query, otherwise return the 
	// query word num
	int32_t getWordNum(int64_t wordId) const;

public:
	// language of the query
	lang_t m_langId;

	bool m_useQueryStopWords;

private:
	bool m_allowHighFreqTermCache;

	// use a generic buffer for m_qwords to point into
	// so we don't have to malloc for them
	SmallBuf<GBUF_SIZE> m_queryWordBuf;

	std::vector<WordVariationGenerator::Variation> m_wordVariations; //have to keep that around because queryterms point into it with qt->m_term
public:
	QueryWord *m_qwords;
	int32_t       m_numWords;

	// QueryWords are converted to QueryTerms
	int32_t      m_numTerms;

	int32_t m_numTermsUntruncated;

private:
	SafeBuf    m_queryTermBuf;
public:
	QueryTerm *m_qterms         ;

	// site: field will disable site clustering
	// ip: field will disable ip clustering
	bool m_hasPositiveSiteField;
	bool m_hasIpField;
	bool m_hasUrlField;
	bool m_hasSubUrlField;

	// . we set this to true if it is a boolean query
	// . when calling Query::set() above you can tell it explicitly
	//   if query is boolean or not, OR you can tell it to auto-detect
	//   by giving different values to the "boolFlag" parameter.
	bool m_isBoolean;

	// if they got a gbdocid: in the query and it's not boolean, set these
	int64_t m_docIdRestriction;

private:
	// for holding the filtered query, in utf8
	SmallBuf<128> m_filteredQuery;

	SmallBuf<128> m_originalQuery;
public:

	// . we now contain the parsing components for boolean queries
	Expression        m_expressions[MAX_EXPRESSIONS];
	int32_t              m_numExpressions;

	int32_t m_maxQueryTerms ;

	//used for setting the qterm->m_termWeight value
	float  m_bigramWeight;
	float  m_synonymWeight;
	WordVariationsConfig m_word_variations_config;

	bool m_truncated;
};

#endif // GB_QUERY_H
