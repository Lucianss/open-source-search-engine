#include "gb-include.h"

#include "HashTableX.h"
#include "Domains.h"
#include "Mem.h"
#include "GbMutex.h"
#include "ScopedLock.h"

static bool isTLDForUrl(const char *tld, int32_t tldLen);


char *getDomainOfIp ( char *host , int32_t hostLen , int32_t *dlen ) {
	// get host length
	//int32_t hostLen = strlen(host);
	// if ip != 0 then host is a numeric ip, point to first 3 #'s
	char *s = host + hostLen - 1;
	while ( s > host && *s!='.' ) s--;
	// if no '.' return NULL and 0
	if ( s == host ) { *dlen = 0; return NULL; }
	// otherwise, set length
	*dlen = s - host;
	// return the first 3 #'s (1.2.3) as the domain
	return host;
}


const char *getDomain ( char *host , int32_t hostLen , const char *tld , int32_t *dlen ) {
	// assume no domain 
	*dlen = 0;
	// get host length
	//int32_t hostLen = strlen(host);
	// get the tld in host, if any, if not, it returns NULL
	const char *s = tld; // getTLD ( host , hostLen );
	// return NULL if host contains no valid tld
	if ( ! s ) return NULL;
	// if s is host we just have tld
	if ( s == host ) return NULL;
	// there MUST be a period before s
	s--; if ( *s != '.' ) return NULL;
	// back up over the period
	s--;
	// now go back until s hits "host" or another period
	while ( s > host && *s !='.' ) s--;
	// . now *s=='.' or s==host
	// . if s is host then "host" is an acceptable domain w/o a hostname
	// . fix http://.xyz.com/...... by checking for period
	if ( s == host && *s !='.' ) { *dlen = hostLen; return s; }
	// skip s forward over the period to point to domain name
	s++;
	// set domain length
	*dlen = hostLen - ( s - host );
	return s;
}

// host must be NULL terminated
const char *getTLD ( const char *host , int32_t hostLen ) {
	if(hostLen==0)
		return NULL;
	// make "s" point to last period in the host
	//char *s = host + strlen(host) - 1;
	const char *hostEnd = host + hostLen;
	const char *s       = hostEnd - 1;
	while ( s > host && *s !='.' ) s--;
	// point to the tld in question
	const char *t  = s;
	if ( *t == '.' ) t++; 
	// reset our current tld ptr
	const char *tld = NULL;
	// is t a valid tld? if so, set "tld" to "t".
	if ( isTLDForUrl ( t , hostEnd - t ) ) tld = t;
	// host had no period at most we had just a tld so return NULL
	if ( s == host ) return tld;

	// back up over last period
	s--;
	// just because it's in table doesn't mean we can't try going up more
	while ( s > host && *s !='.' ) s--;
	// point to the tld in question
	t  = s;
	if ( *t == '.' ) t++; 
	// is t a valid tld? if so, set "tld" to "t".
	if ( isTLDForUrl ( t , hostEnd - t ) ) tld = t;
	// host had no period at most we had just a tld so return NULL
	if ( s == host ) return tld;


	// . now only 1 tld has 2 period and that is "LKD.CO.IM"
	// . so waste another iteration for that (TODO: speed up?)
	// . back up over last period
	s--;
	// just because it's in table doesn't mean we can't try going up more
	while ( s > host && *s !='.' ) s--;
	// point to the tld in question
	t  = s;
	if ( *t == '.' ) t++; 
	// is t a valid tld? if so, set "tld" to "t".
	if ( isTLDForUrl ( t , hostEnd - t ) ) tld = t;
	// we must have gotten the tld by this point, if there was a valid one
	return tld;
}


static HashTableX s_table;
static bool       s_isInitialized = false;
static GbMutex    s_tableMutex;

#include "tlds.inc"

static bool initializeTLDTable() {
	ScopedLock sl(s_tableMutex);
	if(!s_isInitialized) {
		if(!s_table.set(8, 0, sizeof(s_tlds)*2,NULL,0,false, "tldtbl")) {
			log(LOG_WARN, "build: Could not init table of TLDs.");
			return false;
		}

		int32_t n = (int32_t)sizeof(s_tlds)/ sizeof(char *); 
		for(int32_t i = 0; i < n; i++) {
			const char *d    = s_tlds[i];
			int32_t     dlen = strlen (d);
			int64_t     dh   = hash64Lower_a(d, dlen);
			if(!s_table.addKey (&dh,NULL)) {
				log( LOG_WARN, "build: dom table failed");
				return false;
			}
		}
		s_isInitialized = true;
	} 
	sl.unlock();
	return true;
}

static bool isTLDForUrl(const char *tld, int32_t tldLen) {
	if(!initializeTLDTable())
		return false;

	int32_t pcount = 0;
	for ( int32_t i = 0 ; i < tldLen ; i++ ) {
		// period count
		if ( tld[i] == '.' ) { pcount++; continue; }
		if ( ! is_alnum_a(tld[i]) && tld[i] != '-' ) return false;
	}

	if ( pcount == 0 ) return true;
	if ( pcount >= 2 ) return false;

	// otherwise, if one period, check table to see if qualified

	int64_t h = hash64Lower_a ( tld , tldLen ); // strlen(tld));
	return s_table.isInTable ( &h );//getScoreFromTermId ( h );
}		


bool isTLD(const char *tld, int32_t tldLen) {
	if(!initializeTLDTable())
		return false;
	int64_t h = hash64Lower_a(tld, tldLen);
	return s_table.isInTable(&h);
}


void resetDomains ( ) {
	s_table.reset();
	s_isInitialized = false;
}
