#ifndef GB_URLPARSER_H
#define GB_URLPARSER_H

#include "UrlComponent.h"
#include <vector>
#include <map>

class UrlParser {
public:
	UrlParser(const char *url, size_t urlLen, int32_t titledbVersion);

	void print() const;

	void deleteComponent(UrlComponent *urlComponent);
	void deleteComponents(std::vector<UrlComponent*> &urlComponents);

	// path
	std::vector<std::pair<UrlComponent*, UrlComponent*> > matchPath( const UrlComponent::Matcher &keyMatch );

	bool removePath( const std::vector<std::pair<UrlComponent*, UrlComponent*> > &urlComponents, const UrlComponent::Validator &validator );
	bool removePath( const UrlComponent::Matcher &keyMatch, const UrlComponent::Validator &validator );

	// path param
	std::vector<UrlComponent*> matchPathParam( const UrlComponent::Matcher &keyMatch );
	std::vector<const UrlComponent*> matchPathParam( const UrlComponent::Matcher &keyMatch ) const;

	bool removePathParam( const std::vector<UrlComponent*> &urlComponents, const UrlComponent::Validator &validator );
	bool removePathParam( const UrlComponent::Matcher &matcher, const UrlComponent::Validator &validator );

	// query
	size_t getQueryParamCount() {
		return m_queries.size();
	}

	const std::vector<UrlComponent*> matchQueryParam(const UrlComponent::Matcher &keyMatch);
	const std::vector<const UrlComponent*> matchQueryParam(const UrlComponent::Matcher &keyMatch) const;

	bool removeQueryParam( const char *param );
	bool removeQueryParam( const std::vector<UrlComponent*> &urlComponents, const UrlComponent::Validator &validator );
	bool removeQueryParam( const UrlComponent::Matcher &keyMatch, const UrlComponent::Validator &validator );

	void unparse();

	// member access
	int32_t getTitledbVersion() const;

	const char* getUrl() const;
	size_t getUrlLen() const;

	const char* getScheme() const;
	size_t getSchemeLen() const;

	const char* getAuthority() const;
	size_t getAuthorityLen() const;

	const char* getHost() const;
	size_t getHostLen() const;

	const char* getDomain() const;
	size_t getDomainLen() const;

	const std::vector<UrlComponent>* getPaths() const;
	const std::vector<UrlComponent>* getQueries() const;

	const char* getUrlParsed() const;
	size_t getUrlParsedLen() const;
private:
	void parse();

	bool removeComponent( const std::vector<UrlComponent*> &urlComponents, const UrlComponent::Validator &validator );

	int32_t m_titledbVersion;

	std::string m_url;

	const char *m_scheme;
	size_t m_schemeLen;

	const char *m_authority;
	size_t m_authorityLen;

	const char *m_host;
	size_t m_hostLen;

	const char *m_port;
	size_t m_portLen;

	const char *m_domain;
	size_t m_domainLen;

	std::vector<UrlComponent> m_paths;
	char m_pathEndChar;
	size_t m_pathsDeleteCount;

	std::vector<UrlComponent> m_queries;
	size_t m_queriesDeleteCount;

	std::string m_urlParsed;
};

// member access
inline int32_t UrlParser::getTitledbVersion() const {
	return m_titledbVersion;
}

inline const char* UrlParser::getUrl() const {
	return m_url.c_str();
}

inline size_t UrlParser::getUrlLen() const {
	return m_url.length();
}

inline const char* UrlParser::getScheme() const {
	return m_scheme;
}

inline size_t UrlParser::getSchemeLen() const {
	return m_schemeLen;
}

inline const char* UrlParser::getAuthority() const {
	return m_authority;
}

inline size_t UrlParser::getAuthorityLen() const {
	return m_authorityLen;
}

inline const char* UrlParser::getHost() const {
	return m_host;
}

inline size_t UrlParser::getHostLen() const {
	return m_hostLen;
}

inline const char* UrlParser::getDomain() const {
	return m_domain;
}

inline size_t UrlParser::getDomainLen() const {
	return m_domainLen;
}

inline const std::vector<UrlComponent>* UrlParser::getPaths() const {
	return &m_paths;
}

inline const std::vector<UrlComponent>* UrlParser::getQueries() const {
	return &m_queries;
}

inline const char* UrlParser::getUrlParsed() const {
	return m_urlParsed.c_str();
}

inline size_t UrlParser::getUrlParsedLen() const {
	return m_urlParsed.size();
}

#endif // GB_URLPARSER_H
