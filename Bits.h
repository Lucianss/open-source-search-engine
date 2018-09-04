// Matt Wells, copyright Jun 2001

// . each word has several bits of information we like to keep track of
// . these bits are used for making phrases in Phrases.h
// . also used by spam detector in Spam.h
// . TODO: rename this class to PhraseBits
// . TODO: separate words in phrases w/ period OR space so a search for
//   "chicken.rib" gives you the renderman file, not a recipe or something

#ifndef GB_BITS_H
#define GB_BITS_H

#include <inttypes.h>


//
// Bits set by Bits::set() and available via queryBits/clearBits/assignBits/setBits

// . no punctuation or "big" numbers can be in a phrase
#define D_CAN_BE_IN_PHRASE      0x01
// is this word a stop word?
#define D_IS_STOPWORD           0x02
// . used for phrasing 
// . can we continue forming our phrase after this word?
// . some puntuation words and all stop words can be paired across
#define D_CAN_PAIR_ACROSS       0x04
// set by Sections.cpp::setMenu() function
#define D_IN_LINK               0x08
// is this word potentiialy part of a URL?
#define D_IS_IN_URL             0x10
//If adding more bits make sure they fit in 8 bits or extend wbit_t type below

//
// Bits set by Bits::setForSummary() and available via querySWBits/setSWBits
//

// . is this word a strong connector?
// . used by Summary.cpp so we don't split strongly connected things
// . right now, just single character punctuation that is not a space
// . i don't want to split possessive words at the apostrophe, or split
//   ip addresses at the period, etc. applies to unicode as well.
#define D_IS_STRONG_CONNECTOR   0x0001
// . does it start a sentence? 
// . if our summary excerpt starts with this then it will get bonus points
#define D_STARTS_SENTENCE       0x0002
// . or does it start a sentence fragment, like after a comma or something
// . the summary excerpt will get *some* bonus points for this
#define D_STARTS_FRAGMENT       0x0004
// . does this word have a quote right before it?
#define D_IN_QUOTES             0x0008
// more bits so we can get rid of Summary::setSummaryScores() so that
// Summary::getBestWindow() just uses these bits to score the window now
#define D_IN_TITLE              0x0010
#define D_IN_PARENTHESES        0x0020
#define D_IN_HYPERLINK          0x0040
#define D_IN_BOLDORITALICS      0x0080
#define D_IN_LIST               0x0100
#define D_IN_SUP                0x0200
#define D_IN_PARAGRAPH          0x0400
#define D_IN_BLOCKQUOTE         0x0800
// for Summary.cpp
#define D_USED                  0x1000
//If adding more bits make sure they fit in 16 bits or extend swbit_t type below



// bitmask type for normal bits
typedef uint8_t wbit_t;

// bitmask type for summary bits
typedef uint16_t swbit_t;

class TokenizerResult;
class Sections;

class Bits {
public:
	Bits();
	~Bits();

	// . returns false and sets errno on error
	bool set(const TokenizerResult *tr);
	bool setForSummary(const TokenizerResult *tr);

	void reset();

	bool isStopWord( int32_t i ) const {
		return m_bits[i] & D_IS_STOPWORD;
	}

	bool canBeInPhrase( int32_t i ) const {
		return m_bits[i] & D_CAN_BE_IN_PHRASE;
	}

	bool canPairAcross( int32_t i ) const {
		return m_bits[i] & D_CAN_PAIR_ACROSS;
	}

	void setInLinkBits(const Sections *ss);
	void setInUrlBits  ();

	wbit_t queryBits(unsigned i) const { return m_bits[i]; }
	void assignBits(unsigned i, wbit_t b){ m_bits[i] = b; }
	void setBits(unsigned i, wbit_t b) { m_bits[i] |= b; }
	void clearBits(unsigned i, wbit_t b) { m_bits[i] &= ~b; }
	
	swbit_t querySWBits(unsigned i) const { return m_swbits[i]; }
	void setSWBits(unsigned i, swbit_t b) { m_swbits[i] |= b; }
	
 private:
	const TokenizerResult *m_tr;

	wbit_t *m_bits;
	int32_t m_bitsSize;
	
	// . wordbits
	// . used only by setForSummary() now to avoid having to update a lot of code
	swbit_t *m_swbits;
	int32_t m_swbitsSize;
	
	bool m_inLinkBitsSet;
	bool m_inUrlBitsSet;

	char m_localBuf[sizeof(wbit_t)*10];

	wbit_t getAlnumBits( int32_t i ) const;
	wbit_t getNonAlnumBits(unsigned i) const;
};

#endif // GB_BITS_H
