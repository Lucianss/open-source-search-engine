#include "XmlDoc.h"
#include "Collectiondb.h"
#include "SpiderCache.h"
#include "Titledb.h"
#include "Doledb.h"
#include "CountryCode.h"
#include "Log.h"
#include "Conf.h"
#include "Mem.h"
#include "UrlBlockCheck.h"
#include "UrlMatchList.h"
#include "WantedChecker.h"
#include "utf8_convert.h"
#include "GbUtil.h"
#include <libgen.h>
#include <algorithm>
#include <limits.h>

static void print_usage(const char *argv0) {
	fprintf(stdout, "Usage: %s [-h] PATH\n", argv0);
	fprintf(stdout, "Dump unwanted titlerec\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "  -h, --help     display this help and exit\n");
}

static void cleanup() {
	g_log.m_disabled = true;

	g_linkdb.reset();
	g_clusterdb.reset();
	g_spiderCache.reset();
	g_doledb.reset();
	g_spiderdb.reset();
	g_tagdb.reset();
	g_titledb.reset();
	g_posdb.reset();

	g_collectiondb.reset();

	g_loop.reset();

	WantedChecker::finalize();
}

static bool find_str(const char *haystack, size_t haystackLen, const std::string &needle) {
	return (memmem(haystack, haystackLen, needle.c_str(), needle.size()) != nullptr);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ) {
		print_usage(argv[0]);
		return 1;
	}

	g_log.m_disabled = true;

	// initialize library
	g_mem.init();
	hashinit();

	// current dir
	char path[PATH_MAX];
	realpath(argv[1], path);
	size_t pathLen = strlen(path);
	if (path[pathLen] != '/') {
		strcat(path, "/");
	}

	g_hostdb.init(-1, false, false, true, path);
	g_conf.init(path);

	const char *errmsg;
	if (!UnicodeMaps::load_maps("ucdata",&errmsg)) {
		log("Unicode initialization failed!");
		exit(1);
	}

	if (!utf8_convert_initialize()) {
		log(LOG_ERROR, "db: utf-8 conversion initialization failed!");
		exit(1);
	}

	// initialize rdbs
	g_loop.init();

	g_collectiondb.loadAllCollRecs();

	g_posdb.init();
	g_titledb.init();
	g_tagdb.init();
	g_spiderdb.init();
	g_doledb.init();
	g_spiderCache.init();
	g_clusterdb.init();
	g_linkdb.init();

	g_collectiondb.addRdbBaseToAllRdbsForEachCollRec();

	g_log.m_disabled = false;
	g_log.m_logPrefix = false;

	CollectionRec *cr = g_collectiondb.getRec("main");
	if (!cr) {
		logf(LOG_TRACE, "No main collection found");
		return 1;
	}

	// initialize shlib & blacklist
	if (!WantedChecker::initialize()) {
		fprintf(stderr, "Unable to initialize WantedChecker");
		return 1;
	}

	g_urlBlackList.init();
	g_urlWhiteList.init();

	Msg5 msg5;
	RdbList list;

	key96_t startKey;
	startKey.setMin();

	key96_t endKey;
	endKey.setMax();

	static const std::vector<eIANACharset> charsets = {
		csISOLatin1,
		csISOLatin2,
		csISOLatin4,
		cslatin6,
		cswindows1250,
		cswindows1252,
		cswindows1257,
	};

	static const std::vector<const char*> inputs_danish = {
		// danish
		"æ",
		"ø",
		"å",
		"Æ",
		"Ø",
		"Å",
	};

	static const std::vector<const char*> inputs_swedish = {
		// swedish
		"å",
		"Å",
		"ä",
		"ö",
		"Ä",
		"Ö",
	};

	static const std::vector<const char*> inputs_german = {
		// german
		"ä",
		"ö",
		"Ä",
		"Ö",
		"ü",
		"ß",
		"Ü",
		"ẞ",
	};

	static const std::vector<std::pair<lang_t, std::vector<const char*>>> lang_inputs = {
		std::make_pair(langDanish, inputs_danish),
		std::make_pair(langSwedish, inputs_swedish),
		std::make_pair(langGerman, inputs_german),
	};

	std::set<std::string> unwanted_encodes;
	for (const auto &lang_input : lang_inputs) {
		for (const auto &input : lang_input.second) {
			std::vector<std::string> encodes;
			for (const auto &charset : charsets) {
				char buffer[8];
				if (ucToAny(buffer, sizeof(buffer), get_charset_str(csUTF8),
				            input, strlen(input), get_charset_str(charset), -1) > 0) {
					SafeBuf sb;
					urlEncode(&sb, buffer);
					unwanted_encodes.insert(sb.getBufStart());
				}
			}
		}
	}

	while (msg5.getList(RDB_TITLEDB, cr->m_collnum, &list, &startKey, &endKey, 10485760, true, 0, -1, NULL, NULL, 0, true, -1, false)) {
		if (list.isEmpty()) {
			break;
		}

		for (list.resetListPtr(); !list.isExhausted(); list.skipCurrentRecord()) {
			key96_t key = list.getCurrentKey();
			int64_t docId = Titledb::getDocIdFromKey(&key);

			XmlDoc xmlDoc;
			if (!xmlDoc.set2(list.getCurrentRec(), list.getCurrentRecSize(), "main", NULL, 0)) {
				logf(LOG_TRACE, "Unable to set XmlDoc for docId=%" PRIu64, docId);
				continue;
			}

			// extract the url
			Url *url = xmlDoc.getFirstUrl();
			const char *reason = NULL;

			if (isUrlUnwanted(*url, &reason)) {
				fprintf(stdout, "%" PRId64"|%s|%s\n", docId, reason, url->getUrl());
				continue;
			}

			Url **redirUrlPtr = xmlDoc.getRedirUrl();
			if (redirUrlPtr && *redirUrlPtr) {
				Url *redirUrl = *redirUrlPtr;
				if (isUrlUnwanted(*redirUrl, &reason)) {
					fprintf(stdout, "%" PRId64"|redir %s|%s|%s\n", docId, reason, url->getUrl(), redirUrl->getUrl());
					continue;
				}
			}
			uint8_t *contentType = xmlDoc.getContentType();
			switch (*contentType) {
				case CT_GIF:
				case CT_JPG:
				case CT_PNG:
				case CT_TIFF:
				case CT_BMP:
				case CT_JS:
				case CT_CSS:
				case CT_JSON:
				case CT_IMAGE:
				case CT_GZ:
				case CT_ARC:
				case CT_WARC:
					fprintf(stdout, "%" PRId64"|blocked content type|%s\n", docId, url->getUrl());
					continue;
				default:
					break;
			}

			// check content
			int32_t contentLen = xmlDoc.size_utf8Content > 0 ? (xmlDoc.size_utf8Content - 1) : 0;
			if (contentLen > 0) {
				if (!WantedChecker::check_single_content(url->getUrl(), xmlDoc.ptr_utf8Content, contentLen).wanted) {
					fprintf(stdout, "%" PRId64"|blocked content|%s\n", docId, url->getUrl());
					continue;
				}
			}

			bool *ini = xmlDoc.getIsNoIndex();
			if (*ini) {
				bool *inf = xmlDoc.getIsNoFollow();
				if (*inf) {
					fprintf(stdout, "%" PRId64"|meta noindex nofollow|%s\n", docId, url->getUrl());
					continue;
				}
			}

			bool found = false;
			for (const auto &unwanted_encode : unwanted_encodes) {
				if (find_str(xmlDoc.ptr_firstUrl, xmlDoc.size_firstUrl, unwanted_encode)) {
					fprintf(stdout, "%" PRId64"|bad encoding|%s\n", docId, url->getUrl());
					found = true;
					break;
				}
			}

			if (found) {
				continue;
			}
		}

		startKey = *(key96_t *)list.getLastKey();
		startKey++;

		// watch out for wrap around
		if ( startKey < *(key96_t *)list.getLastKey() ) {
			break;
		}
	}

	cleanup();

	return 0;
}
