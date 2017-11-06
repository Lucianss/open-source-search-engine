#include "RobotsBlockedResultOverride.h"
#include "Loop.h"
#include "Url.h"
#include "Log.h"
#include "Conf.h"
#include "JobScheduler.h"


RobotsBlockedResultOverride g_robotsBlockedResultOverride;

static const char *s_resultoverride_filename = "robotsblockedresultoverride.txt";

RobotsBlockedResultOverride::RobotsBlockedResultOverride()
	: LanguageResultOverride(s_resultoverride_filename)
	, m_loading(false) {
}

bool RobotsBlockedResultOverride::init() {
	if (!g_loop.registerSleepCallback(60000, this, &reload, "RobotsBlockedResultOverride::reload", 0)) {
		log(LOG_WARN, "RobotsBlockedResultOverride::init: Failed to register callback.");
		return false;
	}

	load();

	return true;
}

void RobotsBlockedResultOverride::reload(int /*fd*/, void *state) {
	if (g_jobScheduler.submit(reload, nullptr, state, thread_type_config_load, 0)) {
		return;
	}

	// unable to submit job (load on main thread)
	reload(state);
}

void RobotsBlockedResultOverride::reload(void *state) {
	RobotsBlockedResultOverride *robotsBlockedResultOverride = static_cast<RobotsBlockedResultOverride*>(state);

	// don't load multiple times at the same time
	if (robotsBlockedResultOverride->m_loading.exchange(true)) {
		return;
	}

	robotsBlockedResultOverride->load();
	robotsBlockedResultOverride->m_loading = false;
}

std::string RobotsBlockedResultOverride::getTitle(const std::string &lang, const Url &url) const {
	std::string title = LanguageResultOverride::getTitle(lang, url);
	if (title.empty()) {
		// default title when nothing is configured
		title = std::string(url.getHost(), url.getHostLen());
	}

	return title;
}
std::string RobotsBlockedResultOverride::getSummary(const std::string &lang, const Url &url) const {
	std::string summary = LanguageResultOverride::getSummary(lang, url);
	if (summary.empty()) {
		// default description when nothing is configured
		summary = "No description available";
	}

	return summary;
}
