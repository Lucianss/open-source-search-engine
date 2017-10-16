#include "UrlParser.h"
#include "Log.h"
#include "fctypes.h"
#include "Domains.h"
#include "ip.h"
#include <string.h>
#include <iterator>
#include <algorithm>

static const char *strnpbrk(const char *str1, size_t len, const char *str2) {
	const char *haystack = str1;
	const char *haystackEnd = str1 + len;

	while (haystack < haystackEnd && *haystack) {
		const char *needle = str2;
		while (*needle) {
			if (*haystack == *needle) {
				return haystack;
			}
			++needle;
		}
		++haystack;
	}

	return NULL;
}

/// @todo ALC we should see if we need to do relative path resolution here
/// @todo ALC we should cater for scheme relative address (pass in parent scheme)
/// https://tools.ietf.org/html/rfc3986#section-5.2
UrlParser::UrlParser(const char *url, size_t urlLen, int32_t titledbVersion)
	: m_titledbVersion(titledbVersion)
	, m_url(url, urlLen)
	, m_scheme(NULL)
	, m_schemeLen(0)
	, m_authority(NULL)
	, m_authorityLen(0)
	, m_host(NULL)
	, m_hostLen(0)
	, m_port(NULL)
	, m_portLen(0)
	, m_domain(NULL)
	, m_domainLen(0)
	, m_paths()
	, m_pathEndChar('\0')
	, m_pathsDeleteCount(0)
	, m_queries()
	, m_queriesDeleteCount(0)
	, m_urlParsed() {
	m_urlParsed.reserve(m_url.length());
	parse();
}

void UrlParser::print() const {
	logf(LOG_DEBUG, "UrlParser::url       : '%s'", m_url.c_str());
	logf(LOG_DEBUG, "UrlParser::scheme    : '%.*s'", static_cast<uint32_t>(m_schemeLen), m_scheme);
	logf(LOG_DEBUG, "UrlParser::authority : '%.*s'", static_cast<uint32_t>(m_authorityLen), m_authority);
	logf(LOG_DEBUG, "UrlParser::host      : '%.*s'", static_cast<uint32_t>(m_hostLen), m_host);
	logf(LOG_DEBUG, "UrlParser::domain    : '%.*s'", static_cast<uint32_t>(m_domainLen), m_domain);
	logf(LOG_DEBUG, "UrlParser::port      : '%.*s'", static_cast<uint32_t>(m_portLen), m_port);

	for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
		logf(LOG_DEBUG, "UrlParser::path[%02zi]  : '%s'%s", std::distance(m_paths.begin(), it), it->getString().c_str(), it->isDeleted() ? " (deleted)" : "");
	}

	for (auto it = m_queries.begin(); it != m_queries.end(); ++it) {
		logf(LOG_DEBUG, "UrlParser::query[%02zi] : '%s'%s", std::distance(m_queries.begin(), it), it->getString().c_str(), it->isDeleted() ? " (deleted)" : "");
	}
}

void UrlParser::parse() {
	// URI = scheme ":" hier-part [ "?" query ] [ "#" fragment ]

	const char *urlEnd = m_url.c_str() + m_url.length();
	const char *currentPos = m_url.c_str();

	// hier-part   = "//" authority path-abempty
	//             / path-absolute
	//             / path-rootless
	//             / path-empty

	const char *authorityPos = static_cast<const char *>(memmem(currentPos, urlEnd - currentPos, "//", 2));
	if (authorityPos != NULL) {
		if (authorityPos != currentPos) {
			m_scheme = currentPos;
			m_schemeLen = authorityPos - currentPos - 1;
		}

		m_authority = authorityPos + 2;
		currentPos = m_authority;
	} else {
		m_authority = currentPos;
	}

	const char *pathPos = static_cast<const char *>(memchr(currentPos, '/', urlEnd - currentPos));
	if (pathPos != NULL) {
		m_authorityLen = pathPos - m_authority;
		currentPos = pathPos + 1;
	} else {
		m_authorityLen = urlEnd - m_authority;
	}

	// @todo similar logic in Url.cpp (merge this)

	// authority   = [ userinfo "@" ] host [ ":" port ]
	const char *userInfoPos = static_cast<const char *>(memchr(m_authority, '@', m_authorityLen));
	if (userInfoPos != NULL) {
		m_host = userInfoPos + 1;
		m_hostLen = m_authorityLen - (userInfoPos - m_authority) - 1;
	} else {
		m_host = m_authority;
		m_hostLen = m_authorityLen;
	}

	const char *portPos = static_cast<const char *>(memrchr(m_host, ':', m_hostLen));
	if (portPos != NULL) {
		m_port = portPos + 1;
		m_portLen = m_authorityLen - (portPos - m_authority) - 1;

		m_hostLen -= (m_hostLen - (portPos - m_host));
	}

	// host        = IP-literal / IPv4address / reg-name

	/// @todo ALC we should remove the const cast once we fix all the const issue
	int32_t ip = atoip(m_host, m_hostLen);
	if (ip) {
		int32_t domainLen = 0;
		m_domain = getDomainOfIp(const_cast<char *>(m_host), m_hostLen, &domainLen);
		m_domainLen = domainLen;
	} else {
		const char *tldPos = ::getTLD(const_cast<char *>(m_host), m_hostLen);
		if (tldPos) {
			size_t tldLen = m_host + m_hostLen - tldPos;
			if (tldLen < m_hostLen) {
				m_domain = static_cast<const char *>(memrchr(m_host, '.', m_hostLen - tldLen - 1));
				if (m_domain) {
					m_domain += 1;
					m_domainLen = m_hostLen - (m_domain - m_host);
				} else {
					m_domain = m_host;
					m_domainLen = m_hostLen;
				}
			}
		}
	}

	if (pathPos == NULL) {
		// nothing else to process
		return;
	}

	const char *queryPos = static_cast<const char *>(memchr(currentPos, '?', urlEnd - currentPos));
	if (queryPos != NULL) {
		currentPos = queryPos + 1;
	}

	/// @note url fragment is stripped and not part of the rebuild url
	const char *fragmentPos = static_cast<const char *>(memrchr(currentPos, '#', urlEnd - currentPos));
	if (fragmentPos != NULL) {
		// https://developers.google.com/webmasters/ajax-crawling/docs/getting-started
		// don't treat '#!" as anchor
		if (fragmentPos != urlEnd && *(fragmentPos + 1) == '!') {
			fragmentPos = NULL;
		}
	}

	const char *pathEnd = queryPos ? queryPos : (fragmentPos ? fragmentPos : urlEnd);
	m_pathEndChar = *(pathEnd - 1);

	const char *queryEnd = fragmentPos ? fragmentPos : urlEnd;

	// path
	bool updatePathEncChar = false;
	const char *prevPos = pathPos + 1;
	while (prevPos && (prevPos <= pathEnd)) {
		size_t len = pathEnd - prevPos;
		currentPos = strnpbrk(prevPos, len, "/;&");
		if (currentPos) {
			len = currentPos - prevPos;
		}

		UrlComponent urlPart = UrlComponent(UrlComponent::TYPE_PATH, prevPos, len, *(prevPos - 1));

		// check for special cases before adding to m_paths
		if (len == 1 && memcmp(prevPos, ".", 1) == 0) {
			deleteComponent(&urlPart);
			updatePathEncChar = true;
		} else if (len == 2 && memcmp(prevPos, "..", 2) == 0) {
			deleteComponent(&urlPart);
			updatePathEncChar = true;

			for (auto it = m_paths.rbegin(); it != m_paths.rend(); ++it) {
				if (it->isDeleted()) {
					continue;
				}

				deleteComponent(&(*it));

				if (it->getSeparator() == '/') {
					break;
				}
			}

		}
		m_paths.push_back(urlPart);

		prevPos = currentPos ? currentPos + 1 : NULL;
	}

	// set pathEndChar to component after last non-deleted component (if exist)
	if (updatePathEncChar) {
		for (auto it = m_paths.rbegin(); it != m_paths.rend(); ++it) {
			if (it->isDeleted()) {
				continue;
			}

			if (it != m_paths.rbegin()) {
				m_pathEndChar = std::prev(it)->getSeparator();
			}

			break;
		}
	}

	// query
	if (queryPos) {
		prevPos = queryPos + 1;

		bool isPrevAmpersand = false;
		while (prevPos && (prevPos < queryEnd)) {
			size_t len = queryEnd - prevPos;
			currentPos = strnpbrk(prevPos, len, "&;");
			if (currentPos) {
				len = currentPos - prevPos;
			}

			UrlComponent urlPart = UrlComponent(UrlComponent::TYPE_QUERY, prevPos, len, *(prevPos - 1));
			std::string key = urlPart.getKey();

			// check previous urlPart
			if (isPrevAmpersand) {
				urlPart.setSeparator('&');
			}

			bool isAmpersand = (!urlPart.hasValue() && urlPart.getKey() == "amp");
			if (!key.empty() && !isAmpersand) {
				// we don't cater for case sensitive query parameter (eg: parm, Parm, PARM is assumed to be the same)
				auto it = std::find_if(m_queries.begin(), m_queries.end(), [&key](const UrlComponent& u) { return key == u.getKey(); });
				if (it == m_queries.end()) {
					m_queries.push_back(urlPart);
				} else {
					*it = urlPart;
				}
			}

			prevPos = currentPos ? currentPos + 1 : NULL;
			isPrevAmpersand = isAmpersand;
		}
	}

	if (m_titledbVersion >= 124) {
		// remove empty query parameters
		for (auto &query : m_queries) {
			if (query.getValueLen() == 0) {
				deleteComponent(&query);
			}
		}
	}
}

/// @todo ALC a better way of doing this will be to check if the url has changed,
/// and call unparse automatically when getUrlParsed/getUrlParsedLen is called
void UrlParser::unparse() {
	m_urlParsed.clear();

	if (m_scheme == NULL || m_schemeLen == 0) {
		m_urlParsed.append("http");
	} else {
		for (size_t i = 0; i < m_schemeLen; ++i) {
			m_urlParsed.push_back(tolower(m_scheme[i]));
		}
	}

	m_urlParsed.append("://");

	// userinfo '@'
	m_urlParsed.append(m_authority, m_host - m_authority);

	// host
	for (size_t i = 0; i < m_hostLen; ++i) {
		m_urlParsed.push_back(tolower(m_host[i]));
	}

	// port
	if (m_port) {
		m_urlParsed.push_back(':');
		m_urlParsed.append(m_port, m_portLen);
	}

	if (m_pathsDeleteCount != m_paths.size()) {
		bool isFirst = true;

		for (auto &path : m_paths) {
			if (!path.isDeleted()) {
				if (isFirst) {
					isFirst = false;
					if (path.getSeparator() != '/') {
						m_urlParsed.append("/");
					}
				}

				m_urlParsed += path.getSeparator();
				m_urlParsed.append(path.getString());
			}
		}

		if (m_urlParsed[m_urlParsed.size() - 1] != '/' && m_pathEndChar == '/') {
			m_urlParsed += m_pathEndChar;
		}
	} else {
		if (m_titledbVersion >= 124) {
			m_urlParsed += '/';
		}
	}

	if (m_queriesDeleteCount != m_queries.size()) {
		bool isFirst = true;
		for (auto &query : m_queries) {
			if (!query.isDeleted()) {
				if (isFirst) {
					isFirst = false;
					m_urlParsed.append("?");
				} else {
					m_urlParsed += (query.getSeparator() == '?') ? '&' : query.getSeparator();
				}

				m_urlParsed.append(query.getString());
			}
		}
	}
}

void UrlParser::deleteComponent(UrlComponent *urlComponent) {
	if (urlComponent == nullptr || urlComponent->isDeleted()) {
		return;
	}

	urlComponent->setDeleted();

	switch (urlComponent->getType()) {
		case UrlComponent::TYPE_PATH:
			++m_pathsDeleteCount;
			break;
		case UrlComponent::TYPE_QUERY:
			++m_queriesDeleteCount;
			break;
	}
}

void UrlParser::deleteComponents(std::vector<UrlComponent*> &urlComponents) {
	for (auto &urlComponent : urlComponents) {
		if (urlComponent->isDeleted()) {
			continue;
		}

		deleteComponent(urlComponent);
	}
}

bool UrlParser::removeComponent(const std::vector<UrlComponent *> &urlComponents, const UrlComponent::Validator &validator) {
	bool hasRemoval = false;

	for (auto urlComponent : urlComponents) {
		if (urlComponent->isDeleted()) {
			continue;
		}

		if ((urlComponent->hasValue() && validator.isValid(*urlComponent)) ||
		    (!urlComponent->hasValue() && validator.allowEmptyValue())) {
			hasRemoval = true;
			deleteComponent(urlComponent);
		}
	}

	return hasRemoval;
}

std::vector<std::pair<UrlComponent *, UrlComponent *> > UrlParser::matchPath(const UrlComponent::Matcher &matcher) {
	std::vector<std::pair<UrlComponent *, UrlComponent *> > result;

	// don't need to loop if it's all deleted
	if (m_pathsDeleteCount == m_paths.size()) {
		return result;
	}

	for (auto it = m_paths.begin(); it != m_paths.end(); ++it) {
		if (it->isDeleted()) {
			continue;
		}

		if (!it->hasValue() && matcher.isMatching(*it)) {
			auto valueIt = std::next(it, 1);
			result.emplace_back(&(*it), (valueIt != m_paths.end() ? &(*valueIt) : NULL));
		}
	}

	return result;
}

bool UrlParser::removePath(const std::vector<std::pair<UrlComponent *, UrlComponent *> > &urlComponents,
                           const UrlComponent::Validator &validator) {
	bool hasRemoval = false;
	for (const auto &urlComponent : urlComponents) {
		if (urlComponent.second == NULL || (m_titledbVersion <= 123 && urlComponent.second->getValueLen() == 0)) {
			if (validator.allowEmptyValue()) {
				hasRemoval = true;
				deleteComponent(urlComponent.first);
			}
		} else {
			const char *value = (m_titledbVersion <= 123) ? urlComponent.second->getValue() : urlComponent.second->getString().c_str();
			size_t valueLen = (m_titledbVersion <= 123) ? urlComponent.second->getValueLen() : urlComponent.second->getString().size();
			if (validator.isValid(value, valueLen)) {
				hasRemoval = true;
				deleteComponent(urlComponent.first);
				deleteComponent(urlComponent.second);
			}
		}
	}

	return hasRemoval;
}

bool UrlParser::removePath(const UrlComponent::Matcher &matcher, const UrlComponent::Validator &validator) {
	std::vector<std::pair<UrlComponent *, UrlComponent *> > matches = matchPath(matcher);

	return removePath(matches, validator);
}

std::vector<UrlComponent *> UrlParser::matchPathParam(const UrlComponent::Matcher &matcher) {
	std::vector<UrlComponent *> result;

	// don't need to loop if it's all deleted
	if (m_pathsDeleteCount == m_paths.size()) {
		return result;
	}

	for (auto &path : m_paths) {
		if (path.isDeleted()) {
			continue;
		}

		if (path.hasValue() && matcher.isMatching(path)) {
			result.push_back(&path);
		}
	}

	return result;
}

bool UrlParser::removePathParam(const std::vector<UrlComponent *> &urlComponents, const UrlComponent::Validator &validator) {
	return removeComponent(urlComponents, validator);
}

bool UrlParser::removePathParam(const UrlComponent::Matcher &matcher, const UrlComponent::Validator &validator) {
	std::vector<UrlComponent *> matches = matchPathParam(matcher);

	return removeComponent(matches, validator);
}

std::vector<UrlComponent *> UrlParser::matchQueryParam(const UrlComponent::Matcher &matcher) {
	std::vector<UrlComponent *> result;

	// don't need to loop if it's all deleted
	if (m_queriesDeleteCount == m_queries.size()) {
		return result;
	}

	for (auto it = m_queries.begin(); it != m_queries.end(); ++it) {
		if (it->isDeleted()) {
			continue;
		}

		if (matcher.isMatching(*it)) {
			result.push_back(&(*it));
		}
	}

	return result;
}

bool UrlParser::removeQueryParam(const char *param) {
	static const UrlComponent::Validator s_validator(0, 0, true, ALLOW_ALL, MANDATORY_NONE);

	return removeQueryParam(UrlComponent::Matcher(param), s_validator);
}

bool UrlParser::removeQueryParam(const std::vector<UrlComponent *> &urlComponents, const UrlComponent::Validator &validator) {
	return removeComponent(urlComponents, validator);
}

bool UrlParser::removeQueryParam(const UrlComponent::Matcher &matcher, const UrlComponent::Validator &validator) {
	return removeComponent(matchQueryParam(matcher), validator);
}
