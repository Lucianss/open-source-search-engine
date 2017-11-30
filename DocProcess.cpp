//
// Copyright (C) 2017 Privacore ApS - https://www.privacore.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// License TL;DR: If you change this file, you must publish your changes.
//
#include "DocProcess.h"
#include "Errno.h"
#include "Log.h"
#include "Conf.h"
#include "Loop.h"
#include "XmlDoc.h"
#include "ScopedLock.h"
#include "Collectiondb.h"
#include <fstream>
#include <sys/stat.h>
#include <algorithm>

static GbThreadQueue s_docProcessFileThreadQueue;
static GbThreadQueue s_docProcessDocThreadQueue;
static std::atomic<int> s_count(0);

struct DocProcessFileItem {
	DocProcessFileItem(DocProcess *docProcess, const std::string &lastPos)
		: m_docProcess(docProcess)
		, m_lastPos(lastPos) {
	}

	DocProcess *m_docProcess;
	std::string m_lastPos;
};

DocProcessDocItem::DocProcessDocItem(DocProcess *docProcess, const std::string &key, int64_t lastPos)
	: m_docProcess(docProcess)
	, m_key(key)
	, m_lastPos(lastPos)
	, m_xmlDoc(new XmlDoc()) {
}

DocProcessDocItem::~DocProcessDocItem(){
}

static bool docProcessDisabled() {
	CollectionRec *collRec = g_collectiondb.getRec("main");
	if (g_conf.m_spideringEnabled) {
		if (collRec) {
			if (collRec->m_spideringEnabled) {
				return true;
			} else {
				return g_hostdb.hasDeadHostCached();
			}
		}

		return true;
	}

	return g_hostdb.hasDeadHostCached();
}

DocProcess::DocProcess(const char *filename, bool isUrl)
	: m_isUrl(isUrl)
	, m_filename(filename)
	, m_tmpFilename(filename)
	, m_lastPosFilename(filename)
	, m_pendingDocItems()
	, m_pendingDocItemsMtx()
	, m_pendingDocItemsCond(PTHREAD_COND_INITIALIZER)
	, m_stop(false) {
	m_tmpFilename.append(".processing");
	m_lastPosFilename.append(".lastpos");
}

bool DocProcess::init() {
	if (s_count.fetch_add(1) == 0) {
		if (!s_docProcessFileThreadQueue.initialize(processFile, "docprocess-file")) {
			logError("Unable to initialize process file queue");
			return false;
		}

		if (!s_docProcessDocThreadQueue.initialize(processDoc, "docprocess-doc")) {
			logError("Unable to initialize process doc queue");
			return false;
		}
	}

	if (!g_loop.registerSleepCallback(60000, this, &reload, "DocProcess::reload", 0)) {
		log(LOG_WARN, "DocProcess::init: Failed to register callback.");
		return false;
	}

	reload(0, this);

	return true;
}

void DocProcess::finalize() {
	// only finalize once
	if (m_stop.exchange(true)) {
		return;
	}

	pthread_cond_broadcast(&m_pendingDocItemsCond);

	// only finalize static variables once
	if (s_count.fetch_sub(1) == 0) {
		s_docProcessFileThreadQueue.finalize();
		s_docProcessDocThreadQueue.finalize();
	}
}

void DocProcess::reload(int /*fd*/, void *state) {
	if (!s_docProcessFileThreadQueue.isEmpty()) {
		// we're currently processing another file
		return;
	}

	DocProcess *that = static_cast<DocProcess*>(state);

	struct stat st;
	std::string lastPos;
	if (stat(that->m_tmpFilename.c_str(), &st) == 0) {
		if (docProcessDisabled()) {
			log(LOG_INFO, "Processing of %s is disabled", that->m_tmpFilename.c_str());
			return;
		}

		if (stat(that->m_lastPosFilename.c_str(), &st) == 0) {
			std::ifstream file(that->m_lastPosFilename);
			std::string line;
			if (std::getline(file, line)) {
				lastPos = line;
			}
		}
	} else {
		if (stat(that->m_filename, &st) != 0) {
			// probably not found
			logTrace(g_conf.m_logTraceDocProcess, "DocProcess::load: Unable to stat %s", that->m_filename);
			that->m_lastModifiedTime = 0;
			return;
		}

		// we only process the file if we have 2 consecutive loads with the same m_time
		if (that->m_lastModifiedTime == 0 || that->m_lastModifiedTime != st.st_mtime) {
			that->m_lastModifiedTime = st.st_mtime;
			logTrace(g_conf.m_logTraceDocProcess, "DocProcess::load: Modified time changed between load");
			return;
		}

		// only start processing if spidering is disabled
		if (docProcessDisabled()) {
			log(LOG_INFO, "Processing of %s is disabled", that->m_filename);
			return;
		}

		// make sure file is not changed while we're processing it
		int rc = rename(that->m_filename, that->m_tmpFilename.c_str());
		if (rc == -1) {
			log(LOG_WARN, "Unable to rename '%s' to '%s' due to '%s'", that->m_filename, that->m_tmpFilename.c_str(), mstrerror(errno));
			return;
		}
	}

	s_docProcessFileThreadQueue.addItem(new DocProcessFileItem(that, lastPos));
}

DocProcessDocItem* DocProcess::createDocItem(DocProcess *docProcess, const std::string &key, int64_t lastPos) {
	return new DocProcessDocItem(docProcess, key, lastPos);
}

void DocProcess::waitPendingDocCount(unsigned maxCount) {
	ScopedLock sl(m_pendingDocItemsMtx);
	while (!m_stop && m_pendingDocItems.size() > maxCount) {
		pthread_cond_wait(&m_pendingDocItemsCond, &m_pendingDocItemsMtx.mtx);
	}
}

void DocProcess::addPendingDoc(DocProcessDocItem *docItem) {
	logTrace(g_conf.m_logTraceDocProcess, "Adding %s", docItem->m_key.c_str());

	ScopedLock sl(m_pendingDocItemsMtx);
	m_pendingDocItems.push_back(docItem);
}

void DocProcess::removePendingDoc(DocProcessDocItem *docItem) {
	logTrace(g_conf.m_logTraceDocProcess, "Removing %s", docItem->m_key.c_str());

	ScopedLock sl(m_pendingDocItemsMtx);
	auto it = std::find(m_pendingDocItems.begin(), m_pendingDocItems.end(), docItem);

	// docid must be there
	if (it == m_pendingDocItems.end()) {
		gbshutdownLogicError();
	}

	if (it == m_pendingDocItems.begin()) {
		std::ofstream lastPosFile(docItem->m_docProcess->m_lastPosFilename, std::ofstream::out|std::ofstream::trunc);
		lastPosFile << docItem->m_lastPos << "|" << docItem->m_key << std::endl;
	}

	m_pendingDocItems.erase(it);
	pthread_cond_signal(&m_pendingDocItemsCond);
}

void DocProcess::processFile(void *item) {
	DocProcessFileItem *fileItem = static_cast<DocProcessFileItem*>(item);

	log(LOG_INFO, "Processing %s", fileItem->m_docProcess->m_tmpFilename.c_str());

	// start processing file
	std::ifstream file(fileItem->m_docProcess->m_tmpFilename);

	bool isInterrupted = false;

	bool foundLastPos = fileItem->m_lastPos.empty();

	int64_t lastPos = 0;
	std::string lastPosKey;
	if (!fileItem->m_lastPos.empty()) {
		lastPos = strtoll(fileItem->m_lastPos.c_str(), nullptr, 10);
		lastPosKey = fileItem->m_lastPos.substr(fileItem->m_lastPos.find('|') + 1);

		if (lastPosKey.empty()) {
			lastPos = 0;
		}
	}

	// skip to last position
	if (lastPos) {
		file.seekg(lastPos);
	}

	int64_t currentFilePos = file.tellg();
	std::string line;
	while (std::getline(file, line)) {
		// ignore empty lines
		if (line.length() == 0) {
			continue;
		}

		std::string key = fileItem->m_docProcess->m_isUrl ? line : line.substr(0, line.find('|'));

		if (foundLastPos) {
			logTrace(g_conf.m_logTraceDocProcess, "Processing key='%s'", key.c_str());
			DocProcessDocItem *docItem = fileItem->m_docProcess->createDocItem(fileItem->m_docProcess, key, currentFilePos);

			if (fileItem->m_docProcess->m_isUrl) {
				SpiderRequest sreq;
				sreq.setFromAddUrl(key.c_str());
				sreq.m_isAddUrl = 0;

				logTrace(g_conf.m_logTraceDocProcess, "Adding url=%s", key.c_str());
				docItem->m_xmlDoc->set4(&sreq, nullptr, "main", nullptr, 0);
			} else {
				int64_t docId = strtoll(line.c_str(), nullptr, 10);

				if (docId == 0) {
					// ignore invalid docId
					continue;
				}

				logTrace(g_conf.m_logTraceDocProcess, "Adding docid=%" PRId64, docId);
				docItem->m_xmlDoc->set3(docId, "main", 0);
			}

			docItem->m_docProcess->updateXmldoc(docItem->m_xmlDoc);
			docItem->m_xmlDoc->setCallback(docItem, processedDoc);

			fileItem->m_docProcess->addPendingDoc(docItem);
			s_docProcessDocThreadQueue.addItem(docItem);

			fileItem->m_docProcess->waitPendingDocCount(10);
		} else if (lastPosKey.compare(key) == 0) {
			foundLastPos = true;
		}

		// stop processing when we're shutting down or spidering is enabled
		if (fileItem->m_docProcess->m_stop || docProcessDisabled()) {
			isInterrupted = true;
			break;
		}

		currentFilePos = file.tellg();
	}

	fileItem->m_docProcess->waitPendingDocCount(0);

	if (isInterrupted || fileItem->m_docProcess->m_stop) {
		log(LOG_INFO, "Interrupted processing of %s", fileItem->m_docProcess->m_tmpFilename.c_str());
		delete fileItem;
		return;
	}

	log(LOG_INFO, "Processed %s", fileItem->m_docProcess->m_tmpFilename.c_str());

	// delete files
	unlink(fileItem->m_docProcess->m_tmpFilename.c_str());
	unlink(fileItem->m_docProcess->m_lastPosFilename.c_str());

	delete fileItem;
}

void DocProcess::processDoc(void *item) {
	DocProcessDocItem *docItem = static_cast<DocProcessDocItem*>(item);
	docItem->m_docProcess->processDocItem(docItem);
}

void DocProcess::processedDoc(void *state) {
	// reprocess xmldoc
	s_docProcessDocThreadQueue.addItem(state);
}
