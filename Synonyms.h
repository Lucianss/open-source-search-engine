// Matt Wells, copyright Feb 2011

#ifndef GB_SYNONYMS_H
#define GB_SYNONYMS_H

#include "SafeBuf.h"

#define SOURCE_NONE       0
#define SOURCE_PRESET     1
#define SOURCE_WIKTIONARY 2
#define SOURCE_WIKTIONARY_EN 3
#define SOURCE_GENERATED  4
#define SOURCE_BIGRAM     5
#define SOURCE_TRIGRAM    6
#define SOURCE_NUMBER     7

// per word!
#define MAX_SYNS 64

// +1 for langid at end
#define TMPSYNBUFSIZE (MAX_SYNS*(8+8+8+1+4+4+4+4+sizeof(char *)+1))

const char *getSourceString ( char source );

class Words;
class HashTableX;

class Synonyms {

 public:

	Synonyms();
	~Synonyms();

	void reset();

	int32_t getSynonyms(const Words *words,
			    int32_t wordNum,
			    uint8_t langId,
			    char *tmpBuf);

	

	// temporarily store all synonyms here of the word for synonyms
	// like the accent-stripped version of the word. otherwise we
	// can just point to the wiktionary-buf.txt representation in memory.
	SafeBuf m_synWordBuf;

	// for each synonym of this word we fill out these:
	int64_t  *m_aids;
	int64_t  *m_wids0;
	int64_t  *m_wids1;
	char      **m_termPtrs;
	int32_t       *m_termOffs;
	int32_t       *m_termLens;
	int32_t       *m_numAlnumWords;
	int32_t       *m_numAlnumWordsInBase;
	uint8_t    *m_langIds;

private:
	bool addWithoutApostrophe(const Words *words, int32_t wordNum, HashTableX *dt);
	bool addAmpPhrase(const Words *words, int32_t wordNum, HashTableX *dt);
	
	char       *m_src;
	
	int64_t *m_aidsPtr;
	int64_t *m_wids0Ptr;
	int64_t *m_wids1Ptr;
	const char     **m_termPtrsPtr;
	int32_t      *m_termOffsPtr;
	int32_t      *m_termLensPtr;
	int32_t      *m_numAlnumWordsPtr;
	int32_t      *m_numAlnumWordsInBasePtr;
	char      *m_srcPtr;
	uint8_t   *m_langIdsPtr;

};

#endif // GB_SYNONYMS_H
