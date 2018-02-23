#include "Statistics.h"
#include "ScopedLock.h"
#include "Log.h"
#include "gb-include.h"
#include "types.h"
#include "Msg3.h"            //getDiskPageCache()
#include "Mem.h"             //memory statistics
#include "UdpServer.h"       //g_udpServer.getNumUsedSlotsIncoming()
#include "HttpServer.h"      //g_httpServer.m_tcp.m_numUsed
#include "Msg5.h"            //g_numCorrupt
#include "SpiderLoop.h"
#include "RdbCache.h"
#include "Rdb.h"
#include "GbMutex.h"
#include "Lang.h"
#include "CountryCode.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <map>
#include <set>
#include <vector>

static const time_t dump_interval = 60;
static const char tmp_filename[] = "statistics.txt.new";
static const char final_filename[] = "statistics.txt";

static const unsigned timerange_lower_bound[] = {
	0,
	10,
	20,
	50,
	100,
	200,
	500,
	1000,
	2000,
	5000,
	10000,
	20000
};

static const size_t timerange_count = sizeof(timerange_lower_bound)/sizeof(timerange_lower_bound[0]);


struct TimerangeStatistics {
	TimerangeStatistics()
		: min_time(0)
		, max_time(0)
		, count(0)
		, sum(0) {
	}

	unsigned min_time;
	unsigned max_time;
	unsigned count;
	unsigned sum;
};


static unsigned ms_to_tr(unsigned ms) {
	unsigned i=timerange_count-1;
	while(ms<timerange_lower_bound[i])
		i--;
	return i;
}

//////////////////////////////////////////////////////////////////////////////
// Query statistics

typedef std::map<std::tuple<int, unsigned, unsigned>, TimerangeStatistics[timerange_count]> query_trs_t;
static query_trs_t query_trs;
static GbMutex mtx_query_trs;

void Statistics::register_query_time(unsigned term_count, unsigned qlang, int error_code, unsigned ms) {
	unsigned i = ms_to_tr(ms);
	auto key = std::make_tuple(error_code, term_count, qlang);

	ScopedLock sl(mtx_query_trs);
	TimerangeStatistics &ts = query_trs[key][i];
	if (ts.count != 0) {
		if (ms < ts.min_time)
			ts.min_time = ms;
		if (ms > ts.max_time)
			ts.max_time = ms;
	} else {
		ts.min_time = ms;
		ts.max_time = ms;
	}
	ts.count++;
	ts.sum += ms;
}

static void dump_query_statistics( FILE *fp ) {
	ScopedLock sl(mtx_query_trs);
	query_trs_t qcopy(query_trs);
	query_trs.clear();
	sl.unlock();

	for (auto it = qcopy.begin(); it != qcopy.end(); ++it) {
		for (unsigned i = 0; i < timerange_count; ++i) {
			const TimerangeStatistics &ts = it->second[i];
			if (ts.count == 0) {
				continue;
			}

			std::string tmp_str;
			const char *status = "SUCCESS";
			if (std::get<0>(it->first)) {
				status = merrname(std::get<0>(it->first));
				if (status == NULL) {
					tmp_str = std::to_string(std::get<0>(it->first));
					status = tmp_str.c_str();
				}
			}
			fprintf(fp, "query:lower_bound=%u;terms=%u;qlang=%s;status=%s;min=%u;max=%u;count=%u;sum=%u\n",
			        timerange_lower_bound[i],
			        std::get<1>(it->first),
			        getLanguageAbbr(std::get<2>(it->first)),
			        status,
			        ts.min_time,
			        ts.max_time,
			        ts.count,
			        ts.sum);
		}
	}
}

//////////////////////////////////////////////////////////////////////////////
// Spidering statistics

typedef std::map<std::pair<int, int>, TimerangeStatistics[timerange_count]> spider_trs_t;
static spider_trs_t old_spider_trs;
static spider_trs_t new_spider_trs;
static GbMutex mtx_spider_trs;

void Statistics::register_spider_time(bool is_new, int error_code, int http_status, unsigned ms) {
	int i = ms_to_tr(ms);
	auto key = std::make_pair(error_code, http_status);

	ScopedLock sl(mtx_spider_trs);
	TimerangeStatistics &ts = is_new ? new_spider_trs[key][i] : old_spider_trs[key][i];

	if (ts.count != 0) {
		if (ms < ts.min_time)
			ts.min_time = ms;
		if (ms > ts.max_time)
			ts.max_time = ms;
	} else {
		ts.min_time = ms;
		ts.max_time = ms;
	}
	ts.count++;
	ts.sum += ms;
}

enum SpiderStatistics {
	spider_doc_new = 0,
	spider_doc_changed,
	spider_doc_unchanged,
	spider_doc_deleted,
	spider_doc_disallowed,
	spider_doc_http_error,
	spider_doc_other_error,
	spider_doc_end
};

static const char* s_spider_statistics_name[] {
	"new",
	"changed",
	"unchanged",
	"deleted",
	"disallowed",
	"http_error",
	"other_error",
	""
};

static void status_to_spider_statistics( std::vector<unsigned> *spiderdoc_counts, bool is_new, int status, unsigned count ) {
	switch ( status ) {
		case 0:
			(*spiderdoc_counts)[ is_new ? spider_doc_new : spider_doc_changed ] += count;
			break;
		case EDOCUNCHANGED:
			(*spiderdoc_counts)[ spider_doc_unchanged ] += count;
			break;
		case EDOCFILTERED:
		case EDOCFORCEDELETE:
		case EDOCBLOCKEDDNS:
		case EDOCBLOCKEDURL:
		case EDOCBLOCKEDURLIP:
		case EDOCBLOCKEDIP:
		case EDOCBLOCKEDURLCORRUPT:
		case EDOCBLOCKEDURLINVALID:
		case EDOCBLOCKEDSHLIBDOMAIN:
		case EDOCBLOCKEDSHLIBURL:
			(*spiderdoc_counts)[ spider_doc_deleted ] += count;
			break;
		case EDOCDISALLOWED:
		case EDOCDISALLOWEDHTTPSTATUS:
		case EDOCDISALLOWEDERROR:
		case EDOCDISALLOWEDROOT:
			(*spiderdoc_counts)[ spider_doc_disallowed ] += count;
			break;
		case EDOCBADHTTPSTATUS:
			(*spiderdoc_counts)[ spider_doc_http_error ] += count;
			break;
		default:
			(*spiderdoc_counts)[ spider_doc_other_error ] += count;
			break;
	}
}

static void dump_spider_statistics( FILE *fp ) {
	ScopedLock sl(mtx_spider_trs);
	spider_trs_t soldcopy( old_spider_trs );
	old_spider_trs.clear();

	spider_trs_t snewcopy( new_spider_trs );
	new_spider_trs.clear();

	sl.unlock();

	std::vector<unsigned> spiderdoc_counts( spider_doc_end );

	for ( auto it = soldcopy.begin(); it != soldcopy.end(); ++it ) {
		for ( unsigned i = 0; i < timerange_count; ++i ) {
			const TimerangeStatistics &ts = it->second[ i ];
			if ( ts.count == 0 ) {
				continue;
			}

			std::string tmp_str;
			const char *status = "SUCCESS";
			if ( it->first.first ) {
				status = merrname( it->first.first );
				if ( status == NULL ) {
					tmp_str = std::to_string( it->first.first );
					status = tmp_str.c_str();
				}
			}
			fprintf( fp, "spider:lower_bound=%u;is_new=0;status=%s;http_code=%d;min=%u;max=%u;count=%u;sum=%u\n",
			         timerange_lower_bound[ i ],
			         status,
			         it->first.second,
			         ts.min_time,
			         ts.max_time,
			         ts.count,
			         ts.sum );

			status_to_spider_statistics( &spiderdoc_counts, false, it->first.first, ts.count );
		}
	}

	for ( auto it = snewcopy.begin(); it != snewcopy.end(); ++it ) {
		for ( unsigned i = 0; i < timerange_count; ++i ) {
			const TimerangeStatistics &ts = it->second[ i ];
			if ( ts.count == 0 ) {
				continue;
			}

			std::string tmp_str;
			const char *status = "SUCCESS";
			if ( it->first.first ) {
				status = merrname( it->first.first );
				if ( status == NULL ) {
					tmp_str = std::to_string( it->first.first );
					status = tmp_str.c_str();
				}
			}
			fprintf( fp, "spider:lower_bound=%u;is_new=1;status=%s;http_code=%d;min=%u;max=%u;count=%u;sum=%u\n",
			         timerange_lower_bound[ i ],
			         status,
			         it->first.second,
			         ts.min_time,
			         ts.max_time,
			         ts.count,
			         ts.sum );

			status_to_spider_statistics( &spiderdoc_counts, true, it->first.first, ts.count );
		}
	}

	for ( unsigned i = 0; i < spider_doc_end; ++i ) {
		unsigned count = spiderdoc_counts[ i ];
		if ( count > 0 ) {
			fprintf( fp, "spiderdoc:%s=%u\n", s_spider_statistics_name[ i ], count );
		}
	}
}

//////////////////////////////////////////////////////////////////////////////
// IO statistics

struct IOStatistics {
	IOStatistics()
		: count(0)
		, sum(0) {
	}

	unsigned count;
	unsigned sum;
};

typedef std::map<std::pair<bool, int>, IOStatistics> ios_t;
static ios_t io_stats;
static GbMutex mtx_io_stats;

void Statistics::register_io_time( bool is_write, int error_code, unsigned long bytes, unsigned /*ms*/ ) {
	auto key = std::make_pair(is_write, error_code);

	ScopedLock sl(mtx_io_stats);
	IOStatistics &ios = io_stats[key];

	ios.count++;
	ios.sum += bytes;
}

static void dump_io_statistics( FILE *fp ) {
	ScopedLock sl(mtx_io_stats);
	ios_t iocopy( io_stats );
	io_stats.clear();
	sl.unlock();

	for ( auto it = iocopy.begin(); it != iocopy.end(); ++it ) {
		const IOStatistics &ios = it->second;
		if (ios.count == 0) {
			continue;
		}

		std::string tmp_str;
		const char *status = "SUCCESS";
		if ( it->first.second ) {
			status = merrname( it->first.second );
			if ( status == NULL ) {
				tmp_str = std::to_string( it->first.second );
				status = tmp_str.c_str();
			}
		}
		fprintf( fp, "io:is_write=%d;status=%s;count=%u;sum=%u\n",
		         it->first.first,
		         status,
		         ios.count,
		         ios.sum );
	}
}

//////////////////////////////////////////////////////////////////////////////
// Language/Charset statistics

typedef std::map<std::tuple<int, int16_t, uint8_t, uint8_t>, unsigned> encs_t;
static encs_t encoding_stats;
static GbMutex mtx_encoding_stats;

void Statistics::register_document_encoding(int error_code, int16_t charsetId, uint8_t langId, uint16_t countryId) {
	auto key = std::make_tuple(error_code, charsetId, langId, countryId);

	ScopedLock sl(mtx_encoding_stats);
	++(encoding_stats[key]);
}

static void dump_encoding_statistics(FILE *fp) {
	ScopedLock sl(mtx_encoding_stats);
	encs_t enccopy(encoding_stats);
	encoding_stats.clear();
	sl.unlock();

	for (auto it = enccopy.begin(); it != enccopy.end(); ++it) {
		std::string tmp_str;
		const char *status = "SUCCESS";
		if (std::get<0>(it->first)) {
			status = merrname(std::get<0>(it->first));
			if (status == NULL) {
				tmp_str = std::to_string(std::get<0>(it->first));
				status = tmp_str.c_str();
			}
		}

		int16_t charsetId = std::get<1>(it->first);
		uint8_t langId = std::get<2>(it->first);
		uint8_t countryId = std::get<3>(it->first);

		fprintf(fp, "enc:status=%s;charset=%s;lang=%s;country=%s;count=%u\n",
		        status, charsetId == csUnknown ? "unknown" : get_charset_str(charsetId),
		        langId == langUnknown ? "zz" : getLanguageAbbr(langId),
		        countryId == 0 ? "zz" : getCountryCode(countryId),
		        it->second);
	}
}

//////////////////////////////////////////////////////////////////////////////
// RdbCache statistics

// RdbCache keeps its own statistics so we just pull those out

struct RdbCacheHistory {
	rdbid_t rdb_id;
	const char *name;
	int64_t last_hits;
	int64_t last_misses;
};

static RdbCacheHistory rdb_cache_history[] = {
	{RDB_POSDB,    "posdb",    0,0},
	{RDB_TAGDB,    "tagdb",    0,0},
	{RDB_CLUSTERDB,"clusterdb",0,0},
	{RDB_TITLEDB,  "titledb",  0,0},
	{RDB_SPIDERDB_DEPRECATED, "spiderdb", 0,0},
	{RDB_NONE,0,0,0}
};

static void dump_rdb_cache_statistics( FILE *fp ) {
	for(int i=0; rdb_cache_history[i].name; i++) {
		const RdbCache *c = getDiskPageCache(rdb_cache_history[i].rdb_id);
		if(!c)
			continue;
		int64_t delta_hits = c->getNumHits() - rdb_cache_history[i].last_hits;
		int64_t delta_misses = c->getNumMisses() - rdb_cache_history[i].last_misses;
		rdb_cache_history[i].last_hits = c->getNumHits();
		rdb_cache_history[i].last_misses = c->getNumMisses();

		fprintf(fp,"rdbcache:%s;hits=%" PRId64 ";misses=%" PRId64 "\n", rdb_cache_history[i].name,delta_hits,delta_misses);
	}
}

//////////////////////////////////////////////////////////////////////////////
// Assorted statistics

static std::atomic<unsigned long> socket_limit_hit_count(0);

void Statistics::register_socket_limit_hit() {
	socket_limit_hit_count++;
}

//Fetch various counters and levels. Some of them were previously exchanged in PingInfo
static void dump_assorted_statistics(FILE *fp) {
	fprintf(fp,"mem:pctused:%f\n",g_mem.getUsedMemPercentage());
	fprintf(fp,"mem:oom_count:%d\n",g_mem.getOOMCount());
	fprintf(fp,"socket:limit_hit:%lu\n",socket_limit_hit_count.load());
	fprintf(fp,"socket:slots_incoming:%d\n",g_udpServer.getNumUsedSlotsIncoming());
	fprintf(fp,"socket:tcp_in_use:%d\n",g_httpServer.m_tcp.m_numUsed.load());
	fprintf(fp,"misc:corrupt_list_reads:%d\n",g_numCorrupt);
	fprintf(fp,"misc:current_spiders:%d\n",g_spiderLoop.getNumSpidersOut());
}


//////////////////////////////////////////////////////////////////////////////
// UrlBLockCheck statistics

struct UrlBlockCheckCounters {
	std::atomic<unsigned long> c_calls;
	std::atomic<unsigned long> c_blacklisted;
	std::atomic<unsigned long> c_blacklisted_urlip;
	std::atomic<unsigned long> c_blacklisted_urlcorrupt;
	std::atomic<unsigned long> c_blacklisted_urlinvalid;
	std::atomic<unsigned long> c_whitelisted;
	std::atomic<unsigned long> c_shlib_domain_block;
	std::atomic<unsigned long> c_shlib_url_blocked;
	std::atomic<unsigned long> c_default;
};
static UrlBlockCheckCounters url_block_check_counters;

void Statistics::increment_url_block_counter_call() {
	url_block_check_counters.c_calls++;
}

void Statistics::increment_url_block_counter_blacklisted() {
	url_block_check_counters.c_blacklisted++;
}

void Statistics::increment_url_block_counter_blacklisted_urlip() {
	url_block_check_counters.c_blacklisted_urlip++;
}

void Statistics::increment_url_block_counter_blacklisted_urlcorrupt() {
	url_block_check_counters.c_blacklisted_urlcorrupt++;
}

void Statistics::increment_url_block_counter_blacklisted_urlinvalid() {
	url_block_check_counters.c_blacklisted_urlinvalid++;
}

void Statistics::increment_url_block_counter_whitelisted() {
	url_block_check_counters.c_whitelisted++;
}

void Statistics::increment_url_block_counter_shlib_domain_block() {
	url_block_check_counters.c_shlib_domain_block++;
}

void Statistics::increment_url_block_counter_shlib_url_block() {
	url_block_check_counters.c_shlib_url_blocked++;
}

void Statistics::increment_url_block_counter_default() {
	url_block_check_counters.c_default++;
}


static void dump_url_block_check_statistics(FILE *fp) {
	//we don't have a big, fat mutex on this so the counters could be slightly inconsistent.
	auto c_calls = url_block_check_counters.c_calls.exchange(0);
	auto c_blacklisted = url_block_check_counters.c_blacklisted.exchange(0);
	auto c_blacklisted_urlip = url_block_check_counters.c_blacklisted_urlip.exchange(0);
	auto c_blacklisted_urlcorrupt = url_block_check_counters.c_blacklisted_urlcorrupt.exchange(0);
	auto c_blacklisted_urlinvalid = url_block_check_counters.c_blacklisted_urlinvalid.exchange(0);
	auto c_whitelisted = url_block_check_counters.c_whitelisted.exchange(0);
	auto c_shlib_domain_block = url_block_check_counters.c_shlib_domain_block.exchange(0);
	auto c_shlib_url_blocked = url_block_check_counters.c_shlib_url_blocked.exchange(0);
	auto c_default = url_block_check_counters.c_default.exchange(0);

	fprintf(fp,"urlblock:calls:%lu\n", c_calls);
	fprintf(fp,"urlblock:blacklisted:%lu\n", c_blacklisted);
	fprintf(fp,"urlblock:blacklisted_urlip:%lu\n", c_blacklisted_urlip);
	fprintf(fp,"urlblock:blacklisted_urlcorrupt:%lu\n", c_blacklisted_urlcorrupt);
	fprintf(fp,"urlblock:blacklisted_urlinvalid:%lu\n", c_blacklisted_urlinvalid);
	fprintf(fp,"urlblock:whitelisted:%lu\n", c_whitelisted);
	fprintf(fp,"urlblock:shlib_domain_block:%lu\n", c_shlib_domain_block);
	fprintf(fp,"urlblock:shlib_url_blocked:%lu\n", c_shlib_url_blocked);
	fprintf(fp,"urlblock:default:%lu\n", c_default);
}


//////////////////////////////////////////////////////////////////////////////
// crawl ban (blocks / captches)
static GbMutex mtx_crawl_ban_counters;
static std::map<std::string,unsigned long> crawl_ban_counters;

void Statistics::increment_crawl_ban_counter(const char *group) {
	ScopedLock sl(mtx_crawl_ban_counters);
	crawl_ban_counters[group]++;
}

static void dump_crawl_ban_statistics(FILE *fp) {
	std::map<std::string,unsigned long> c;
	ScopedLock sl(mtx_crawl_ban_counters);
	c.swap(crawl_ban_counters);
	sl.unlock();
	for(const auto &iter : c) {
		fprintf(fp,"crawlban:%s:%lu\n",iter.first.c_str(),iter.second);
	}
}


//////////////////////////////////////////////////////////////////////////////
// statistics

static void dump_statistics(time_t now) {
	FILE *fp = fopen(tmp_filename,"w");
	if ( !fp ) {
		log( LOG_ERROR, "fopen(%s,\"w\") failed with errno=%d (%s)", tmp_filename, errno, strerror( errno ) );
		return;
	}

	fprintf( fp, "%ld\n", ( long ) now );

	// dump statistics
	dump_query_statistics(fp);
	dump_spider_statistics(fp);
	dump_io_statistics(fp);
	dump_encoding_statistics(fp);
	dump_rdb_cache_statistics(fp);
	dump_assorted_statistics(fp);
	dump_url_block_check_statistics(fp);
	dump_crawl_ban_statistics(fp);
	
	if ( fflush(fp) != 0 ) {
		log( LOG_ERROR, "fflush(%s) failed with errno=%d (%s)", tmp_filename, errno, strerror( errno ) );
		fclose( fp );
		return;
	}
	fclose(fp);
	
	if ( rename( tmp_filename, final_filename ) != 0 ) {
		log( LOG_ERROR, "rename(%s,%s) failed with errno=%d (%s)", tmp_filename, final_filename, errno, strerror( errno ) );
	}
}




static bool stop_dumping = false;
static GbMutex mtx_dump;
static pthread_cond_t cond_dump = PTHREAD_COND_INITIALIZER;
static pthread_t dump_thread;

extern "C" {

static void *dumper_thread_function(void *)
{
	mtx_dump.lock();
	while(!stop_dumping) {
		timespec ts;
		clock_gettime(CLOCK_REALTIME,&ts);
		ts.tv_sec += dump_interval;
		ts.tv_sec = (ts.tv_sec/dump_interval)*dump_interval;
		pthread_cond_timedwait(&cond_dump,&mtx_dump.mtx,&ts);
		if(stop_dumping)
			break;
		mtx_dump.unlock();
		clock_gettime(CLOCK_REALTIME,&ts);
		dump_statistics(ts.tv_sec);
		mtx_dump.lock();
	}
	mtx_dump.unlock();
	return 0;
}

} //extern C




bool Statistics::initialize()
{
	int rc = pthread_create(&dump_thread, NULL, dumper_thread_function, NULL);
	if(rc!=0) {
		log(LOG_ERROR,"pthread_create() failed with rc=%d (%s)",rc,strerror(rc));
		return false;
	}
	(void)pthread_setname_np(dump_thread,"statdump");
	return true;
}


void Statistics::finalize()
{
	stop_dumping = true;
	pthread_cond_signal(&cond_dump);
	pthread_join(dump_thread,NULL);
}
