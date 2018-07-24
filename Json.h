#ifndef GB_JSON_H
#define GB_JSON_H

#define JT_NULL 2
#define JT_NUMBER 3
#define JT_STRING 4
#define JT_ARRAY 5
#define JT_OBJECT 6

#include "SafeBuf.h"
#include <inttypes.h>


#define MAXJSONPARENTS 64

bool endsInCurly ( char *s , int32_t slen );

class JsonItem {

 public:
	// scan the linked list
	class JsonItem *m_next,*m_prev;
	class JsonItem *m_parent;//child;


	// the JT_* values above
	int m_type;

	// . the NAME of the item
	// . points into the ORIGINAL json string
	char *m_name;
	int32_t m_nameLen;

	// for JT_NUMBER
	int32_t m_valueLong;
	int64_t m_value64;
	// for JT_NUMBER
	double m_valueDouble;

	// for JT_String
	int32_t m_valueLen;

	const char *m_valueArray;

	// for JT_String
	int32_t  getValueLen() { return m_valueLen; }

	// for arrays (JT_ARRAY), hack the char ptr into m_valueLong
	const char *getArrayStart() { return m_valueArray;}
	int32_t  getArrayLen  () { return m_valueLen; }

	// for JT_String
	char *getValue () { 
		// if value is another json object, then return NULL
		// must be string
		if ( m_type != JT_STRING ) return NULL;
		// otherwie return the string which is stored decoded
		// after this object in the same buffer
		return (char *)this + sizeof(JsonItem);
	}

	// convert numbers and bools to strings for this one
	char *getValueAsString ( int32_t *valueLen ) ;

	// like acme.product.offerPrice if "acme:{product:{offerprice:1.23}}"
	bool getCompoundName ( SafeBuf &nameBuf ) ;

	bool isInArray ( );
};


class Json {
 public:

	JsonItem *parseJsonStringIntoJsonItems ( const char *json );

	JsonItem *getFirstItem ( ) ;

	JsonItem *getItem ( char *name );

	JsonItem *addNewItem ();

	Json() { 
		m_stackPtr = 0;
		m_prev = NULL;
		memset(m_stack, 0, sizeof(m_stack));
	}
	
	static bool prependKey(SafeBuf& jsonString, char* newKey);

	SafeBuf m_sb;
	JsonItem *m_stack[MAXJSONPARENTS];
	int32_t m_stackPtr;
	class JsonItem *m_prev;

	void reset() { 
		m_sb.purge(); 
	}
};

#endif // GB_JSON_H
