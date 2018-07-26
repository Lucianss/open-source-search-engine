// Matt Wells, copyright Sep 2001

// . class to parse and form HTTP requests

#ifndef GB_HTTPREQUEST_H
#define GB_HTTPREQUEST_H

// . allow for up to 256 cgi fields
// . this was stopping us from having more than about 253 banned ips, so i
//   raised it to 600
//#define MAX_CGI_PARMS 600
// . new prioirty controls has 128 rows!!
#define MAX_CGI_PARMS 1400

// for getting a file from http server
#define MAX_HTTP_FILENAME_LEN 1024

// i raised this from 1.3k to 5.3k so we can log the full request better
//#define MAX_REQ_LEN (1024*5+300)
//#define MAX_REQ_LEN (8024*5+300)

// keep it small now that we use m_reqBuf
//#define MAX_REQ_LEN (1024)

#include "SafeBuf.h"
class TcpSocket;

#include "GbFormat.h"
#include <time.h>



class HttpRequest {

 public:

	// . form an HTTP request 
	// . use size 0 for HEAD requests
	// . use size -1 for GET whole doc requests
	// . fill in your own offset/size for partial GET requests
	// . returns false and sets errno on error
	bool set ( char *url , int32_t offset = 0 , int32_t size = -1 ,
		   time_t ifModifiedSince = 0 , const char *userAgent = NULL ,
		   const char *proto = "HTTP/1.0" ,
		   bool doPost = false ,
		   const char *cookieJar = NULL ,
		   const char *additionalHeader = NULL , // does not incl \r\n
		   int32_t postContentLen = -1 , // for content-length of POST
		   int32_t proxyIp = 0 ,
		   const char *proxyUsernamePwdAuth = NULL );

	// use this
	SafeBuf m_reqBuf;
	bool    m_reqBufValid;

	// get the request length
	int32_t getRequestLen() const { return m_reqBuf.length(); }

	// . get the outgoing request we made by calling set() above
	// . OR get the first line of an incoming request
	const char *getRequest() const { 
		if ( m_reqBufValid ) return m_reqBuf.getBufStart();
		else return NULL;
	}

	// FORMAT_HTML FORMAT_JSON FORMAT_XML
	char getFormat() const { return getReplyFormat(); }
	char getReplyFormat() const;
	mutable bool m_replyFormatValid;
	mutable char m_replyFormat;

	// get the referer field of the MIME header
	char *getReferer () { return m_ref; }

	// this is NULL terminated too
	char *getUserAgent () { return m_userAgent; }

	// just does a simply gbmemcpy() operation, since it should be pointing
	// into the TcpSocket's buffer which is safe until after reply is sent
	// . returns false and sets g_errno on error, true otherwise
	bool copy(const HttpRequest *r);

	// . the url being reuqested
	// . removes &code= facebook cruft
	bool getCurrentUrl ( SafeBuf &cu );
	bool getCurrentUrlPath ( SafeBuf &cup );

	// . parse an incoming request
	// . returns false and set errno on error
	// . may alloc mem for m_cgiBuf to hold cgi vars from GET or POST op
	bool set ( char *req , int32_t reqSize , TcpSocket *s );

	// for gigablast's own rendering of squid
	bool m_isSquidProxyRequest;
	char *m_squidProxiedUrl;
	int32_t m_squidProxiedUrlLen;

	// is it this type of request?
	bool isGETRequest  () const { return (m_requestType == 0); }
	bool isHEADRequest () const { return (m_requestType == 1); }
	bool isPOSTRequest () const { return (m_requestType == 2); }

	const char *getFilename () const { return m_filename; }
	int32_t  getFilenameLen () const { return m_filenameLen; }
	int32_t  getFileOffset  () const { return m_fileOffset; }
	int32_t  getFileSize    () const { return m_fileSize; }

	const char *getOrigUrlRequest() const { return m_origUrlRequest; }
	int32_t getOrigUrlRequestLen() const { return m_origUrlRequestLen; }

	const char *getHost     () const { return m_host;    }
	int32_t  getHostLen     () const { return m_hostLen; }
	bool  isLocal        () const { return m_isLocal; }


	// . the &ucontent= cgi var does not get its value decoded
	//   because it's already decoded
	// . this is so Mark doesn't have to url encode his injected content
	const char *getUnencodedContent() const { return m_ucontent; }
	int32_t  getUnencodedContentLen() const { return m_ucontentLen; }
	
	// . for parsing the terms in a cgi url
	// . the returned string is NOT NULL terminated
	const char *getString  ( const char *field, int32_t *len = NULL,
				 const char *defaultString = NULL , int32_t *next=NULL) const;
	bool       getBool     ( const char *field, bool defaultBool ) const;
	int32_t    getLong     ( const char *field, int32_t defaultLong ) const;
	int64_t    getLongLong ( const char *field, int64_t defaultLongLong ) const;
	float      getFloat    ( const char *field, double defaultFloat ) const;
	double     getDouble   ( const char *field, double defaultDouble ) const;

	float       getFloatFromCookie   ( const char *field, float def ) const;
	int32_t     getLongFromCookie    ( const char *field, int32_t def ) const;
	int64_t     getLongLongFromCookie( const char *field, int64_t def ) const;
	bool        getBoolFromCookie    ( const char *field, bool def ) const;
	const char *getStringFromCookie  ( const char *field, int32_t *len = NULL,
					  const char *defaultString = NULL ,
					  int32_t *next=NULL) const;
	

	bool hasField ( const char *field ) const;

	// are we a redir? if so return non-NULL
	const char *getRedir() const { return m_redir;    }
	int32_t     getRedirLen() const { return m_redirLen; }

	HttpRequest();
	HttpRequest( const HttpRequest &a );
	~HttpRequest();
	void reset();

	const char *getPath() const { return m_path; }
	int32_t  getPathLen() const { return m_plen; }

	// . get value of cgi "field" term in the requested filename
	// . you know GET /myfile.html?q=123&name=nathaniel
	const char *getValue ( const char *field , int32_t *len=NULL, int32_t *next=NULL) const;

	// get value of the ith field
	const char *getValue ( int32_t i, int32_t *len = NULL) const;

	// get the ith cgi parameter name, return NULL if none
	int32_t  getNumFields( ) const { return m_numFields; }
	const char *getField( int32_t i ) const {
		if ( i >= m_numFields ) return NULL;
		return m_fields[i];
	}
	int32_t  getFieldLen ( int32_t i ) const {
		if ( i >= m_numFields ) return 0;
		return m_fieldLens[i];
	}

private:
	// . s is a cgi string
	// . either the stuff after the '?' in a url
	// . or the content in a POST operation
	// . returns false and sets errno on error
	bool addCgi ( char *s , int32_t slen );

	// . parse cgi field terms into m_fields,m_fieldLens,m_fieldValues
	// . "s" should point to cgi string right after the '?' if it exists
	// . s should have had all it's &'s replaced with /0's
	// . slen should include the last \0
	void parseFields ( char *s , int32_t slen ) ;
	void parseFieldsMultipart ( char *s , int32_t slen ) ;

	// 0 for GET, 1 for HEAD
	char  m_requestType;

	// we decode the filename into this buffer (no cgi)
	char  m_filename[MAX_HTTP_FILENAME_LEN];
	int32_t  m_filenameLen;  // excludes ?cgistuff

	// if request is like "GET /poo?foo=bar"
	// then origUrlRequest is "/poo?foo=bar"
	// references into TcpSocket::m_readBuf
	char *m_origUrlRequest;
	int32_t  m_origUrlRequestLen;

	// virtual host in the Host: field of the mime
	char  m_host[256];
	int32_t  m_hostLen;

	// are we coming from a local machine? 
	bool  m_isLocal;
	
	// . decoded cgi data stored here 
	// . this just points into TcpSocket::m_readBuf
	// . now it points into m_reqBuf.m_buf[]
	char *m_cgiBuf       ;
	int32_t  m_cgiBufLen    ;
	int32_t  m_cgiBufMaxLen ;

	// partial GET file read info
	int32_t  m_fileOffset;
	int32_t  m_fileSize;

	// . cgi field term info stored in here
	// . set by parseFields()
	char *m_fields      [ MAX_CGI_PARMS ];
	int32_t  m_fieldLens   [ MAX_CGI_PARMS ];
	char *m_fieldValues [ MAX_CGI_PARMS ];
	int32_t  m_numFields;

	int32_t m_userIP;
	bool m_isSSL;

	// . ptr to the thing we're getting in the request
	// . used by PageAddUrl4.cpp
	char *m_path;
	int32_t  m_plen;

	char  m_redir[128];
	int32_t  m_redirLen;

	// referer, NULL terminated, from Referer: field in MIME
	char  m_ref [ 256 ];
	int32_t  m_refLen;

	// NULL terminated User-Agent: field in MIME
	char  m_userAgent[128];

	// this points into m_cgiBuf
	char *m_ucontent;
	int32_t  m_ucontentLen;

	char *m_cookiePtr;
	int32_t  m_cookieLen;

	// buffer for adding extra parms
	char *m_cgiBuf2;
	int32_t  m_cgiBuf2Size;
};

const int HTTP_REQUEST_DEFAULT_REQUEST_VERSION = 2;

int getVersionFromRequest ( HttpRequest *r );

#endif // GB_HTTPREQUEST_H
