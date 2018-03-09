// Matt Wells, copyright Dec 2008

#ifndef GB_WIKI_H
#define GB_WIKI_H

#include "gb-include.h"
#include "BigFile.h"
#include "HashTableX.h"

class TokenizerResult;

class Wiki {
public:
	Wiki();
	~Wiki();

	void reset();

	int32_t getNumWordsInWikiPhrase(unsigned i, const TokenizerResult *tr);

	// . load from disk
	// . wikititles.txt (loads wikititles.dat if and date is newer)
	bool load();
	
private:
	bool loadText ( int32_t size );

	HashTableX m_ht;
	
	BigFile m_f;

	void *m_state;
	void (* m_callback)(void *);

	bool m_opened;
};

extern class Wiki g_wiki;

#endif // GB_WIKI_H
