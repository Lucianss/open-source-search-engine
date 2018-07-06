#include "UrlMatch.h"
#include "Url.h"
#include "hash.h"
#include "GbUtil.h"
#include "Log.h"
#include "Conf.h"
#include "UrlParser.h"
#include <algorithm>

urlmatchstr_t::urlmatchstr_t(urlmatchtype_t type, const std::string &str)
	: m_type(type)
	, m_str(str) {
}

urlmatchdomain_t::urlmatchdomain_t(const std::string &domain, const std::string &allow, pathcriteria_t pathcriteria)
	: m_domain(domain)
	, m_allow(split(allow, ','))
	, m_pathcriteria(pathcriteria) {
}

urlmatchhost_t::urlmatchhost_t(const std::string &host, const std::string &path)
	: m_host(host)
	, m_path(path)
	, m_port(0) {
	size_t pos = m_host.find(':');
	if (pos != std::string::npos) {
		m_port = static_cast<int>(strtol(m_host.c_str() + pos + 1, NULL, 10));
		m_host.erase(pos, std::string::npos);
	}
}

urlmatchparam_t::urlmatchparam_t(urlmatchtype_t type, const std::string &name, const std::string &value)
	: m_type(type)
	, m_name(name)
	, m_value(value) {
}

urlmatchregex_t::urlmatchregex_t(const std::string &regexStr, const GbRegex &regex)
	: m_regex(regex)
	, m_regexStr(regexStr) {
}

UrlMatch::UrlMatch(const std::shared_ptr<urlmatchstr_t> &urlmatchstr, bool invert)
	: m_invert(invert)
	, m_type(urlmatchstr->m_type)
	, m_str(urlmatchstr){
}

UrlMatch::UrlMatch(const std::shared_ptr<urlmatchdomain_t> &urlmatchdomain, bool invert)
	: m_invert(invert)
	, m_type(url_match_domain)
	, m_domain(urlmatchdomain) {
}

UrlMatch::UrlMatch(const std::shared_ptr<urlmatchhost_t> &urlmatchhost, bool invert)
	: m_invert(invert)
	, m_type(url_match_host)
	, m_host(urlmatchhost) {
}

UrlMatch::UrlMatch(const std::shared_ptr<urlmatchparam_t> &urlmatchparam, bool invert)
	: m_invert(invert)
	, m_type(urlmatchparam->m_type)
	, m_param(urlmatchparam) {
}

UrlMatch::UrlMatch(const std::shared_ptr<urlmatchregex_t> &urlmatchregex, bool invert)
	: m_invert(invert)
	, m_type(url_match_regex)
	, m_regex(urlmatchregex) {
}

static bool matchString(const std::string &needle, const char *haystack, int32_t haystackLen) {
	return ((needle.length() == static_cast<size_t>(haystackLen)) && memcmp(needle.c_str(), haystack, needle.length()) == 0);
}

static bool matchStringPrefix(const std::string &needle, const char *haystack, int32_t haystackLen) {
	return ((needle.length() <= static_cast<size_t>(haystackLen)) && memcmp(needle.c_str(), haystack, needle.length()) == 0);
}

static bool matchStringSuffix(const std::string &needle, const char *haystack, int32_t haystackLen) {
	return ((needle.length() <= static_cast<size_t>(haystackLen)) && memcmp(needle.c_str(), haystack + haystackLen - needle.length(), needle.length()) == 0);
}

bool UrlMatch::match(const Url &url, const UrlParser &urlParser) const {
	switch (m_type) {
		case url_match_domain:
			if (matchString(m_domain->m_domain, url.getDomain(), url.getDomainLen())) {
				// check subdomain
				if (!m_domain->m_allow.empty()) {
					auto subDomainLen = (url.getDomain() == url.getHost()) ? 0 : url.getDomain() - url.getHost() - 1;
					std::string subDomain(url.getHost(), subDomainLen);
					bool match = (std::find(m_domain->m_allow.cbegin(), m_domain->m_allow.cend(), subDomain) == m_domain->m_allow.cend());
					if (!match) {
						// check for pathcriteria
						switch (m_domain->m_pathcriteria) {
							case urlmatchdomain_t::pathcriteria_allow_all:
								return false;
							case urlmatchdomain_t::pathcriteria_allow_index_only:
								return (url.getPathLen() > 1);
							case urlmatchdomain_t::pathcriteria_allow_rootpages_only:
								return (url.getPathDepth(false) > 0);
						}
					}
				}

				return true;
			}
			break;
		case url_match_file:
			return matchString(m_str->m_str, url.getFilename(), url.getFilenameLen());
		case url_match_host:
			if (matchString(m_host->m_host, url.getHost(), url.getHostLen())) {
				// validate port
				if (m_host->m_port > 0 && (m_host->m_port != url.getPort())) {
					return false;
				}

				if (m_host->m_path.empty()) {
					return true;
				}

				return matchStringPrefix(m_host->m_path, url.getPath(), url.getPathLenWithCgi());
			}
			break;
		case url_match_hostsuffix:
			if (matchStringSuffix(m_str->m_str, url.getHost(), url.getHostLen())) {
					// full match
				if ((m_str->m_str.length() == static_cast<size_t>(url.getHostLen())) ||
					// hostsuffix starts with a dot
					(m_str->m_str[0] == '.') ||
					// hostsuffix doesn't start with a dot, but we always want a full segment match
					(url.getHost()[url.getHostLen() - m_str->m_str.length() - 1] == '.')) {
					return true;
				}
			}
			return false;
		case url_match_queryparam:
			if (strncasestr(url.getQuery(), m_param->m_name.c_str(), url.getQueryLen()) != nullptr) {
				// not the most efficient, but there is already parsing logic for query parameter in UrlParser
				auto queryMatches = urlParser.matchQueryParam(UrlComponent::Matcher(m_param->m_name.c_str()));
				if (m_param->m_value.empty()) {
					return (!queryMatches.empty());
				}

				for (auto &queryMatch : queryMatches) {
					if (matchString(m_param->m_value, queryMatch->getValue(), queryMatch->getValueLen())) {
						return true;
					}
				}
			}
			break;
		case url_match_path:
			return matchStringPrefix(m_str->m_str, url.getPath(), url.getPathLenWithCgi());
		case url_match_pathparam:
			if (strncasestr(url.getPath(), m_param->m_name.c_str(), url.getPathLen()) != nullptr) {
				// not the most efficient, but there is already parsing logic for path parameter in UrlParser
				auto pathParamMatches = urlParser.matchPathParam(UrlComponent::Matcher(m_param->m_name.c_str()));
				if (m_param->m_value.empty()) {
					return (!pathParamMatches.empty());
				}

				for (auto &pathParamMatch : pathParamMatches) {
					if (matchString(m_param->m_value, pathParamMatch->getValue(), pathParamMatch->getValueLen())) {
						return true;
					}
				}
			}
			break;
		case url_match_pathpartial:
			return (strncasestr(url.getPath(), m_str->m_str.c_str(), url.getPathLen()) != nullptr);
		case url_match_regex:
			return m_regex->m_regex.match(url.getUrl());
	}

	return false;
}

void UrlMatch::logMatch(const Url &url) const {
	const char *type = NULL;
	const char *value = NULL;

	switch (m_type) {
		case url_match_domain:
			type = "domain";
			value = m_domain->m_domain.c_str();
			break;
		case url_match_file:
			type = "file";
			value = m_str->m_str.c_str();
			break;
		case url_match_host:
			type = "host";
			value = m_host->m_host.c_str();
			break;
		case url_match_hostsuffix:
			type = "hostsuffix";
			value = m_str->m_str.c_str();
			break;
		case url_match_queryparam:
			type = "param";
			value = m_param->m_name.c_str();
			break;
		case url_match_path:
			type = "path";
			value = m_str->m_str.c_str();
			break;
		case url_match_pathparam:
			type = "pathparam";
			value = m_param->m_name.c_str();
			break;
		case url_match_pathpartial:
			type = "pathpartial";
			value = m_str->m_str.c_str();
			break;
		case url_match_regex:
			type = "regex";
			value = m_regex->m_regexStr.c_str();
			break;
	}

	logTrace(g_conf.m_logTraceUrlMatchList, "Url match criteria %s='%s' matched url '%s'", type, value, url.getUrl());
}
