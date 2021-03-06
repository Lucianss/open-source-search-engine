#include "utf8_fast.h"
#include "unicode/UCMaps.h"
#include "Log.h"

const unsigned char g_map_to_lower[256] = {
	  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
	 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
	 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
	'@','a','b','c','d','e','f','g','h','i','j','k','l','m','n','o',
	'p','q','r','s','t','u','v','w','x','y','z', 91, 92, 93, 94, 95,
	 96,'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o',
	'p','q','r','s','t','u','v','w','x','y','z',123,124,125,126,127,
	128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
	144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
	160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
	176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
	224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
	240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,223,
	224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
	240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
};

// converts ascii chars and IS_O chars to their lower case versions
const unsigned char g_map_to_upper[256] = {
	  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
	 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
	 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
	 64,'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O',
	'P','Q','R','S','T','U','V','W','X','Y','Z', 91, 92, 93, 94, 95,
	96, 'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O',
	'P','Q','R','S','T','U','V','W','X','Y','Z',123,124,125,126,127,
	128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
	144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
	160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
	176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
	192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
	208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
	192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
	208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,255
};

const char g_map_is_upper[256] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

// people mix windows 1252 into latin-1 so we have to be less restrictive here...
const char g_map_is_binary[256] = {
	1,1,1,1,1,1,1,1,1,0,0,1,1,0,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,0,0,0,0,0,0,1,0,0,1,1,0,0,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};


// converts ascii chars and IS_O chars to their lower case versions
const char g_map_is_lower[256] = { // 97-122 and 224-255 (excluding 247)
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1
};

const char g_map_is_ascii[256] = { // 32 to 126
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

const char g_map_is_punct[256] = { // 33-47, 58-64, 91-96, 123-126, 161-191, 215,247
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,
	1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,
	1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0
};

const char g_map_is_alnum[256] = { // 48-57, 65-90,97-122,192-255(excluding 215,247)
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1
};

const char g_map_is_alpha[256] = { // 65-90, 97-122, 192-255 (excluding 215, 247)
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1
};

const char g_map_is_digit[256] = { // 48-57
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};


const char g_map_is_hex[256] = { // 48-57
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
	0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

// stolen from is_alnum, but turned on - and _
// 48-57, 65-90,97-122,192-255(excluding 215,247)
// we include the : for feedburner:origlink
const char g_map_is_tagname_char [256] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,
	0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};




bool has_alpha_utf8(const char *s, const char *send) {
	char cs = 0;
	for ( ; s < send ; s += cs ) {
		cs = getUtf8CharSize ( s );
		if ( cs == 1 ) {
			if (is_alpha_a(*s)) return true;
			continue;
		}
		if ( is_alpha_utf8(s) ) return true;
	}
	return false;
}

bool is_alnum_utf8_string(const char *s, const char *send) {
	char cs = 0;
	for( ; s < send ; s += cs ) {
		cs = getUtf8CharSize(s);
		if(cs == 1) {
			if(!is_alnum_a(*s))
				return false;
		} else {
			if(!is_alnum_utf8(s) )
				return false;
		}
	}
	return true;
}

bool is_upper_utf8_string(const char *s, const char *send) {
	//todo: what about titlecase?
	char cs = 0;
	for( ; s < send; s += cs) {
		if(is_digit(*s)) {
			cs=1;
			continue;
		}
		cs = getUtf8CharSize(s);
		if(cs == 1) {
			if(is_lower_a(*s))
				return false;
		} else {
			if(is_lower_utf8(s))
				return false;
		}
	}
	return true;
}

bool is_all_upper_utf8_string(const char *s, const char *send) {
	//todo: what about titlecase?
	char cs = 0;
	for( ; s < send; s += cs) {
		cs = getUtf8CharSize(s);
		if(cs == 1) {
			if(!is_upper_a(*s))
				return false;
		} else {
			if(!is_upper_utf8(s))
				return false;
		}
	}
	return true;
}

//Check for pure capitalized words like "Smith", "London", "ǅemal"
//Surname oddities like "oBrien" and "macDonal" are not supported (returns false)
bool is_capitalized_utf8_word(const char *s, const char *send) {
	bool first_char = true;
	char cs = 0;
	for( ; s < send; s += cs) {
		cs = getUtf8CharSize(s);
		if(cs == 1) {
			//ascii optimization
			if(first_char) {
				if(!is_upper_a(*s))
					return false;
			} else {
				//must lowercase letter
				if(is_upper_a(*s))
					return false;
				if(!is_alpha_a(*s))
					return false;
			}
		} else {
			UChar32 uc = utf8Decode(s);
			if(first_char) {
				//no is_titlecase() function. We just check for 4 titlecase codepoitns. The rest are in Greek extended, which we ignore for now.
				if(!UnicodeMaps::is_uppercase(uc) &&
				   uc!=0x01C5 &&  //ǅ
				   uc!=0x01C8 &&  //ǈ
				   uc!=0x01CB &&  //ǋ
				   uc!=0x01F2)    //ǲ
					return false;
			} else {
				//must lowercase letter
				if(UnicodeMaps::is_uppercase(uc))
					return false;
				if(!UnicodeMaps::is_alphabetic(uc))
					return false;
			}
		}
		first_char = false;
	}
	return true;
}

bool is_wspace_utf8_string(const char *s, const char *send) {
	char cs = 0;
	for( ; s < send ; s += cs) {
		cs = getUtf8CharSize(s);
		if(!is_wspace_utf8(s))
			return false;
	}
	return true;
}

bool has_wspace_utf8_string(const char *s, const char *send) {
	char cs = 0;
	for( ; s < send ; s += cs) {
		cs = getUtf8CharSize(s);
		if(is_wspace_utf8(s))
			return true;
	}
	return false;
}

bool is_alnum_api_utf8_string(const char *s, const char *send) {
	if(s==send)
		return false; //empty string is not an identifyer
	if(!is_ascii(*s))
		return false; //first char must be ascii
	if(*s!='_' && !is_alpha_a(*s)) //first char must be underscore or letter
		return false;
	s++;
	char cs = 0;
	for( ; s < send ; s += cs ) {
		cs = getUtf8CharSize(s);
		if(cs == 1) {
			if(!is_ascii(*s) || !is_alnum_a(*s))
				return false;
		} else
			return false; //must be ascii
	}
	return true;
}

bool is_ascii_digit_string(const char *s, const char *send) {
	while(s<send) {
		char c = *s++;
		if(c<'0' || c>'9')
			return false;
	}
	return true;
}


// . returns bytes stored into "dst" from "src"
// . just do one character, which may be from 1 to 4 bytes
int32_t to_lower_utf8(char *dst, const char *src) {
	// if in ascii do it quickly
	if(is_ascii3(*src)) {
		*dst = to_lower_a ( *src );
		return 1;
	}
	// convert to a code point
	UChar32 x = utf8Decode(src);
	// covert to lower
	UChar32 y = UnicodeMaps::to_lower(x);
	// put it back to utf8. return bytes stored.
	return utf8Encode(y, dst);
}

int32_t to_lower_utf8(char *dst, char * /*dstEnd*/, const char *src, const char *srcEnd) {
	char *dstart = dst;
	for ( ; src < srcEnd ; src += getUtf8CharSize((uint8_t *)src) )
		dst += to_lower_utf8 ( dst , src );
	// return bytes written
	return dst - dstart;
}

int32_t to_lower_utf8(char *dst, char * /*dstEnd*/, const char *src ) {
	char *dstart = dst;
	for ( ; *src ; src += getUtf8CharSize((uint8_t *)src) )
		dst += to_lower_utf8 ( dst , src );
	// return bytes written
	return dst - dstart;
}


int32_t to_upper_utf8(char *dst, const char *src) {
	if(is_ascii3(*src)) {
		*dst = to_upper_a(*src);
		return 1;
	}
	UChar32 x = utf8Decode(src);
	auto e = UnicodeMaps::g_unicode_uppercase_map.lookup(x);
	if(!e)
		return utf8Encode(x, dst);
	int bytes_encoded = 0;
	for(unsigned i=0; i<e->count; i++) {
		int l = utf8Encode(e->values[i], dst);
		dst += l;
		bytes_encoded += l;
	}
	return bytes_encoded;
}


int32_t to_upper_utf8(char *dst, char * /*dstEnd*/, const char *src, const char *srcEnd) {
	char *dstart = dst;
	for ( ; src < srcEnd ; src += getUtf8CharSize((uint8_t *)src) )
		dst += to_upper_utf8 ( dst , src );
	// return bytes written
	return dst - dstart;
}



int32_t to_capitalized_utf8(char *dst, char * /*dstEnd*/, const char *src, const char *srcEnd) {
	char *dstart = dst;
	bool first = true;
	for( ; src < srcEnd; src += getUtf8CharSize(src) ) {
		if(first) {
			dst += to_upper_utf8(dst, src);
			first = false;
		} else
			dst += to_lower_utf8(dst, src);
	}
	// return bytes written
	return dst - dstart;
}

