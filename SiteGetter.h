#ifndef GB_SITEGETTER_H
#define GB_SITEGETTER_H

#include "gb-include.h"
#include "Msg0.h"
#include "Tagdb.h"

#define MAX_SITE_LEN 	256
#define MAX_SCHEME_LEN 	16


class SiteGetter {

public:

	SiteGetter();
	~SiteGetter();

	// . returns false if blocked, true otherwise
	// . sets g_errno on erorr
	bool getSite ( const char *url, class TagRec *gr, int32_t timestamp, collnum_t collnum, int32_t niceness,
	               void *state    = NULL, void (* callback)(void *) = NULL ) ;

	const char *getSite() const { return m_site; }
	int32_t getSiteLen() const { return m_siteLen; }

	const char *getScheme() const { return m_scheme; }
	int32_t getSchemeLen() const { return m_schemeLen; }

	int32_t getErrno() const { return m_errno; }

	bool allDone() const { return m_allDone; }

private:
	bool setRecognizedSite();
	bool getSiteList();
	bool gotSiteList();
	bool setSite();
	static void gotSiteListWrapper(void *state);

	const char         *m_url;
	collnum_t m_collnum;
	void         *m_state;
	void        (*m_callback) (void *state );
	RdbList       m_list;

	// use Msg0 for getting the no-split termlist that combines 
	// gbpathdepth: with the site hash in a single termid
	Msg0   m_msg0;
	int32_t   m_pathDepth;
	int32_t   m_maxPathDepth;
	int32_t   m_niceness;
	bool   m_allDone;

	bool   m_hasSubdomain;

	// points into provided "u->m_url" buffer
	char   m_site[MAX_SITE_LEN+1];
	int32_t   m_siteLen;

	char   m_scheme[MAX_SCHEME_LEN+1];
	int32_t   m_schemeLen;

	bool   m_tryAgain;

	int32_t   m_errno;
};

#endif // GB_SITEGETTER_H
