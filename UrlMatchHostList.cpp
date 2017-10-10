#include "UrlMatchHostList.h"
#include "Log.h"
#include "Conf.h"
#include "Url.h"
#include <fstream>
#include <sys/stat.h>


UrlMatchHostList g_urlHostBlackList;

UrlMatchHostList::UrlMatchHostList()
	: m_filename()
	, m_matchHost(false)
	, m_urlmatchhostlist(new urlmatchhostlist_t) {
}

bool UrlMatchHostList::load(const char *filename, bool matchHost) {
	m_filename = filename;
	m_matchHost = matchHost;

	log(LOG_INFO, "Loading %s", m_filename);

	struct stat st;
	if (stat(m_filename, &st) != 0) {
		// probably not found
		log(LOG_INFO, "UrlMatchHostList::load: Unable to stat %s", m_filename);
		return false;
	}

	urlmatchhostlist_ptr_t tmpUrlMatchHostList(new urlmatchhostlist_t);

	std::ifstream file(m_filename);
	std::string line;
	while (std::getline(file, line)) {
		// ignore empty lines
		if (line.length() == 0) {
			continue;
		}

		tmpUrlMatchHostList->insert(line);

		logTrace(g_conf.m_logTraceUrlMatchHostList, "Adding criteria '%s' to list", line.c_str());
	}

	logTrace(g_conf.m_logTraceUrlMatchHostList, "Number of urlhost-match entries in %s: %ld", m_filename, (long)tmpUrlMatchHostList->size());
	swapUrlMatchHostList(tmpUrlMatchHostList);

	log(LOG_INFO, "Loaded %s", m_filename);
	return true;
}

void UrlMatchHostList::unload() {
	log(LOG_INFO, "Unloading %s", m_filename);
	swapUrlMatchHostList(urlmatchhostlist_ptr_t(new urlmatchhostlist_t));
	log(LOG_INFO, "Unloaded %s", m_filename);
}

bool UrlMatchHostList::isUrlMatched(const Url &url) {
	auto urlmatchhostlist = getUrlMatchHostList();

	std::string key = m_matchHost ? std::string(url.getHost(), url.getHostLen()) : std::string(url.getUrl(), url.getUrlLen());
	return (urlmatchhostlist->count(key) > 0);
}

urlmatchhostlistconst_ptr_t UrlMatchHostList::getUrlMatchHostList() {
	return m_urlmatchhostlist;
}

void UrlMatchHostList::swapUrlMatchHostList(urlmatchhostlistconst_ptr_t urlMatchHostList) {
	std::atomic_store(&m_urlmatchhostlist, urlMatchHostList);
}
