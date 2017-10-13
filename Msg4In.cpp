#include "Msg4In.h"
#include "Parms.h"

#include "UdpServer.h"
#include "Hostdb.h"
#include "Conf.h"
#include "UdpSlot.h"
#include "Rdb.h"
#include "Repair.h"
#include "JobScheduler.h"
#include "ip.h"
#include "Mem.h"
#include "Titledb.h"	// for Titledb::validateSerializedRecord
#include <sys/stat.h> //stat()
#include <fcntl.h>

#ifdef _VALGRIND_
#include <valgrind/memcheck.h>
#endif

// . TODO: use this instead of spiderrestore.dat
// . call this once for every Msg14 so it can add all at once...
// . make Msg14 add the links before anything else since that uses Msg10
// . also, need to update spiderdb rec for the url in Msg14 using Msg4 too!
// . need to add support for passing in array of lists for Msg14

namespace Msg4In {
static bool addMetaList(const char *p, class UdpSlot *slot = NULL);
static void handleRequest4(UdpSlot *slot, int32_t niceness);
static void processMsg4(void *item);

static GbThreadQueue s_incomingThreadQueue;
}

// all these parameters should be preset
bool Msg4In::registerHandler() {
	logTrace( g_conf.m_logTraceMsg4, "BEGIN" );

	// register ourselves with the udp server
	if ( ! g_udpServer.registerHandler ( msg_type_4, handleRequest4 ) ) {
		log(LOG_ERROR,"%s:%s: Could not register with UDP server!", __FILE__, __func__ );
		return false;
	}

	logTrace( g_conf.m_logTraceMsg4, "END - returning true");

	return true;
}

bool Msg4In::initializeIncomingThread() {
	return s_incomingThreadQueue.initialize(processMsg4, "process-msg4");
}

void Msg4In::finalizeIncomingThread() {
	s_incomingThreadQueue.finalize();
}

// . destroys the slot if false is returned
// . this is registered in Msg4::set() to handle add rdb record msgs
// . seems like we should always send back a reply so we don't leave the
//   requester's slot hanging, unless he can kill it after transmit success???
// . TODO: need we send a reply back on success????
// . NOTE: Must always call g_udpServer::sendReply or sendErrorReply() so
//   read/send bufs can be freed
static void Msg4In::processMsg4(void *item) {
	UdpSlot *slot = static_cast<UdpSlot*>(item);

	logTrace( g_conf.m_logTraceMsg4, "BEGIN" );

	// extract what we read
	char *readBuf     = slot->m_readBuf;

	// this returns false with g_errno set on error
	if (!addMetaList(readBuf, slot)) {
		logError("calling sendErrorReply error='%s'", mstrerror(g_errno));
		g_udpServer.sendErrorReply(slot,g_errno);

		logTrace(g_conf.m_logTraceMsg4, "END - addMetaList returned false. g_errno=%d", g_errno);
		return;
	}

	// good to go
	g_udpServer.sendReply(NULL, 0, NULL, 0, slot);

	logTrace(g_conf.m_logTraceMsg4, "END - OK");
}

static void Msg4In::handleRequest4(UdpSlot *slot, int32_t /*netnice*/) {
	// if we just came up we need to make sure our hosts.conf is in
	// sync with everyone else before accepting this! it might have
	// been the case that the sender thinks our hosts.conf is the same
	// since last time we were up, so it is up to us to check this
	if ( g_hostdb.hostsConfInDisagreement() ) {
		g_errno = EBADHOSTSCONF;
		logError("call sendErrorReply");
		g_udpServer.sendErrorReply ( slot , g_errno );

		log(LOG_WARN,"%s:%s: END - hostsConfInDisagreement", __FILE__, __func__ );
		return;
	}

	// need to be in sync first
	if ( ! g_hostdb.hostsConfInAgreement() ) {
		// . if we do not know the sender's hosts.conf crc, wait 4 it
		// . this is 0 if not received yet
		if (!slot->m_host->isHostsConfCRCKnown()) {
			g_errno = EWAITINGTOSYNCHOSTSCONF;
			logError("call sendErrorReply");
			g_udpServer.sendErrorReply ( slot , g_errno );

			log(LOG_WARN,"%s:%s: END - EWAITINGTOSYNCHOSTCONF", __FILE__, __func__ );
			return;
		}

		// compare our hosts.conf to sender's otherwise
		if (!slot->m_host->hasSameHostsConfCRC()) {
			g_errno = EBADHOSTSCONF;
			logError("call sendErrorReply");
			g_udpServer.sendErrorReply ( slot , g_errno );

			log(LOG_WARN,"%s:%s: END - EBADHOSTSCONF", __FILE__, __func__ );
			return;
		}
	}

	// extract what we read
	char *readBuf     = slot->m_readBuf;
	int32_t  readBufSize = slot->m_readBufSize;

	// must at least have an rdbId
	if (readBufSize < 7) {
		g_errno = EREQUESTTOOSHORT;
		logError("call sendErrorReply");
		g_udpServer.sendErrorReply ( slot , g_errno );

		log(LOG_ERROR,"%s:%s: END - EREQUESTTOOSHORT", __FILE__, __func__ );
		return;
	}


	// get total buf used
	int32_t used = *(int32_t *)readBuf; //p += 4;

	// sanity check
	if ( used != readBufSize ) {
		logError("msg4: got corrupted request from hostid %" PRId32" used [%" PRId32"] != readBufSize [%" PRId32"]. tid=%" PRId32 "",
		         slot->m_host->m_hostId, used, readBufSize, slot->getTransId());
		loghex(LOG_ERROR, readBuf, (readBufSize < 160 ? readBufSize : 160), "readBuf (first max. 160 bytes)");

		gbshutdownAbort(true);
	}

	// if we did not sync our parms up yet with host 0, wait...
	if ( g_hostdb.m_myHostId != 0 && ! g_parms.inSyncWithHost0() ) {
		// limit logging to once per second
		static int32_t s_lastTime = 0;
		int32_t now = getTimeLocal();
		if ( now - s_lastTime >= 1 ) {
			s_lastTime = now;
			log(LOG_INFO, "msg4: waiting to sync with host #0 before accepting data");
		}
		// tell send to try again shortly
		g_errno = ETRYAGAIN;
		logError("call sendErrorReply");
		g_udpServer.sendErrorReply(slot,g_errno);

		logTrace( g_conf.m_logTraceMsg4, "END - ETRYAGAIN. Waiting to sync with host #0" );
		return;
	}

	s_incomingThreadQueue.addItem(slot);
}

struct RdbItem {
	RdbItem(collnum_t collNum, const char *rec, int32_t recSize)
		: m_collNum(collNum)
		, m_rec(rec)
		, m_recSize(recSize) {
	}

	collnum_t m_collNum;
	const char *m_rec;
	int32_t m_recSize;
};

struct RdbItems {
	RdbItems()
		: m_numRecs(0)
		, m_dataSizes(0)
		, m_items() {
	}

	int32_t m_numRecs;
	int32_t m_dataSizes;
	std::vector<RdbItem> m_items;
};

// . Syncdb.cpp will call this after it has received checkoff keys from
//   all the alive hosts for this zid/sid
// . returns false and sets g_errno on error, returns true otherwise
static bool Msg4In::addMetaList(const char *p, UdpSlot *slot) {
	logDebug(g_conf.m_logDebugSpider, "syncdb: calling addMetalist zid=%" PRIu64, *(int64_t *) (p + 4));

	// get total buf used
	int32_t used = *(int32_t *)p;
	// the end
	const char *pend = p + used;
	// skip the used amount
	p += 4;
	// skip zid
	p += 8;

	Rdb  *rdb       = NULL;
	char  lastRdbId = -1;

	/// @note we can have multiple meta list here

	// check if we have enough room for the whole request
	// note: we also use this variable for keeping track of which rdbs we have touch and may need an integrity check
	std::map<rdbid_t, RdbItems> rdbItems;

	while (p < pend) {
		collnum_t collnum = *(collnum_t *)p;
		p += sizeof(collnum_t);

		rdbid_t rdbId = static_cast<rdbid_t>(*(char *)p);
		p += 1;

		int32_t recSize = *(int32_t *)p;
		p += 4;

		const char *rec = p;

		// recSize can't go over pend
		if(p + recSize > pend)
			gbshutdownAbort(true);

		// Sanity. Shut down if data sizes are wrong.
		if( rdbId == RDB_TITLEDB ) {
			Titledb::validateSerializedRecord( rec, recSize );
		}

		// . get the rdb to which it belongs, use Msg0::getRdb()
		// . do not call this for every rec if we do not have to
		if (rdbId != lastRdbId || !rdb) {
			rdb = getRdbFromId(rdbId);

			if (!rdb) {
				char ipbuf[16];
				log(LOG_WARN, "msg4: rdbId of %" PRId32" unrecognized from hostip=%s. dropping WHOLE request",
				    (int32_t)rdbId, slot ? iptoa(slot->getIp(),ipbuf) : "unknown");
				gbshutdownAbort(true);
			}

			// an uninitialized secondary rdb?
			// don't core any more, we probably restarted this shard
			// and it needs to wait for host #0 to syncs its
			// g_conf.m_repairingEnabled to '1' so it can start its
			// Repair.cpp repairWrapper() loop and init the secondary
			// rdbs so "rdb" here won't be NULL any more.
			if (!rdb->isInitialized()) {
				time_t currentTime = getTime();
				static time_t s_lastTime = 0;
				if (currentTime > s_lastTime + 10) {
					s_lastTime = currentTime;
					log(LOG_WARN, "msg4: oops. got an rdbId key for a secondary "
							"rdb and not in repair mode. waiting to be in repair mode.");
				}
				g_errno = ETRYAGAIN;
				return false;
			}
		}

		// if we don't have data, recSize must be the same with keySize
		if (rdb->getFixedDataSize() == 0 && recSize != rdb->getKeySize()) {
			gbshutdownAbort(true);
		}

		// don't add to spiderdb when we're nospider host
		if (!g_hostdb.getMyHost()->m_spiderEnabled && (rdbId == RDB_SPIDERDB || rdbId == RDB2_SPIDERDB2)) {
			// advance over the rec data to point to next entry
			p += recSize;
			continue;
		}

		auto &rdbItem = rdbItems[rdbId];
		++rdbItem.m_numRecs;

		int32_t dataSize = recSize - rdb->getKeySize();
		if (rdb->getFixedDataSize() == -1) {
			dataSize -= 4;
		}

		rdbItem.m_dataSizes += dataSize;

		rdbItem.m_items.emplace_back(collnum, rec, recSize);

		// advance over the rec data to point to next entry
		p += recSize;
	}

	bool hasRoom = true;
	bool anyDumping = false;
	for (auto const &rdbItem : rdbItems) {
		Rdb *rdb = getRdbFromId(rdbItem.first);
		if (rdb->isDumping()) {
			anyDumping = true;
		} else if (!rdb->hasRoom(rdbItem.second.m_numRecs, rdbItem.second.m_dataSizes)) {
			rdb->submitRdbDumpJob(true);
			hasRoom = false;
		}
	}

	if (!hasRoom) {
		logDebug(g_conf.m_logDebugSpider, "One or more target Rdbs don't have room currently. Returning try-again for this Msg4");
		g_errno = ETRYAGAIN;
		return false;
	}

	if (anyDumping) {
		logDebug(g_conf.m_logDebugSpider, "One or more target Rdbs is dumping. Returning try-again for this Msg4");
		g_errno = ETRYAGAIN;
		return false;
	}


	for (auto const &rdbItem : rdbItems) {
		Rdb *rdb = getRdbFromId(rdbItem.first);

		bool status = false;
		for (auto const &item : rdbItem.second.m_items) {
			// reset g_errno
			g_errno = 0;

			// . make a list from this data
			// . skip over the first 4 bytes which is the rdbId
			// . TODO: embed the rdbId in the msgtype or something...
			RdbList list;

			// set the list
			// todo: dodgy cast to char*. RdbList should be fixed
			list.set((char *)item.m_rec, item.m_recSize, (char *)item.m_rec, item.m_recSize,
			         rdb->getFixedDataSize(), false, rdb->useHalfKeys(), rdb->getKeySize());

			// keep track of stats
			rdb->readRequestAdd(item.m_recSize);

			// this returns false and sets g_errno on error
			status = rdb->addListNoSpaceCheck(item.m_collNum, &list);

			// bad coll #? ignore it. common when deleting and resetting
			// collections using crawlbot. but there are other recs in this
			// list from different collections, so do not abandon the whole
			// meta list!! otherwise we lose data!!
			if (g_errno == ENOCOLLREC && !status) {
				g_errno = 0;
				status = true;
			}

			if (!status) {
				break;
			}
		}

		if (!status) {
			break;
		}
	}

	// verify integrity if wanted
	if (g_conf.m_verifyTreeIntegrity) {
		for (auto const &rdbItem : rdbItems) {
			Rdb *rdb = getRdbFromId(rdbItem.first);
			rdb->verifyTreeIntegrity();
		}
	}

	// no memory means to try again
	if (g_errno == ENOMEM) {
		g_errno = ETRYAGAIN;
	}

	// doing a full rebuid will add collections
	if (g_errno == ENOCOLLREC && g_repairMode > 0) {
		g_errno = ETRYAGAIN;
	}

	// are we done
	if (g_errno) {
		return false;
	}

	// Initiate dumps for any Rdbs wanting it
	for (auto const &rdbItem : rdbItems) {
		Rdb *rdb = getRdbFromId(rdbItem.first);
		rdb->submitRdbDumpJob(false);
	}

	// success
	return true;
}
