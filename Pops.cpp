#include "Pops.h"
#include "tokenizer.h"
#include "Speller.h"
#include "Mem.h"
#include "Sanity.h"


Pops::Pops () {
	m_pops = NULL;
	m_popsSize = 0;
	memset(m_localBuf, 0, sizeof(m_localBuf));
}

Pops::~Pops() {
	if ( m_pops && m_pops != (int32_t *)m_localBuf ) {
		mfree ( m_pops , m_popsSize , "Pops" );
	}
}

bool Pops::set ( const TokenizerResult *tr , int32_t a , int32_t b ) {
	int32_t nw = tr->size();

	int32_t need = nw * 4;
	if ( need > POPS_BUF_SIZE ) m_pops = (int32_t *)mmalloc(need,"Pops");
	else                        m_pops = (int32_t *)m_localBuf;
	if ( ! m_pops ) return false;
	m_popsSize = need;

	for ( int32_t i = a ; i < b && i < nw ; i++ ) {
		const auto &token = (*tr)[i];
		// skip if not indexable
		if ( !token.is_alfanum ) {
			m_pops[i] = 0;
			continue;
		}

		// once again for the 50th time partap's utf16 crap gets in 
		// the way... we have to have all kinds of different hashing
		// methods because of it...
		uint64_t key;
		const char *wp = token.token_start;
		int32_t wlen = token.token_len;
		key = hash64d( wp, wlen );
		m_pops[i] = g_speller.getPhrasePopularity( wp, key, 0 );

		// sanity check
		if ( m_pops[i] < 0 ) gbshutdownLogicError();

		if ( m_pops[i] == 0 ) {
			m_pops[i] = 1;
		}
	}

	return true;
}

