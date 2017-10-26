// Matt Wells, Copyright Jun 2001


// . TODO: if i'm sending to 1 host in a specified group how do you know
//         to switch adn try anoher host in the group?  What if target host 
//         has to access disk ,etc...???

// . this class is used for performing multicasting through a UdpServer
// . the Multicast class is used to govern a multicast
// . each UdpSlot in the Multicast class corresponds to an ongoing transaction
// . takes up 24*4 = 96 bytes
// . TODO: have broadcast option for hosts in same network
// . TODO: watch out for director splitting the groups - it may
//         change the groups we select
// . TODO: set individual host timeouts yourself based on their std.dev pings

#ifndef GB_MULTICAST_H
#define GB_MULTICAST_H

#include "msgtype_t.h"
#include "GbMutex.h"
#include <inttypes.h>
#include <stddef.h>


#define MAX_HOSTS_PER_GROUP 10

//various timeouts, in milliseconds
static const int64_t multicast_infinite_send_timeout       = 9999999999;
static const int64_t multicast_msg20_summary_timeout       =       1500;
static const int64_t multicast_msg3a_default_timeout       =      10000;
static const int64_t multicast_msg3a_maximum_timeout       =      60000;
static const int64_t multicast_msg1c_getip_default_timeout =      60000;

class UdpSlot;
class Host;


class Multicast {

 public:

	Multicast  ( ) ;
	~Multicast ( ) ;
	void constructor ( );
	void destructor  ( );

	// . returns false and sets errno on error
	// . returns true on success -- your callback will be called
	// . check errno when your callback is called
	// . we timeout the whole shebang after "totalTimeout" seconds
	// . if "sendToWholeGroup" is true then this sends your msg to all 
	//   hosts in the group with id "groupId"
	// . if "sendToWholeGroup" is true we don't call callback until we 
	//   got a non-error reply from each host in the group
	// . it will keep re-sending errored replies every 1 second, forever
	// . this is useful for safe-sending adds to databases
	// . spider will reach max spiders and be stuck until
	// . if "sendToWholeGroup" is false we try to get a reply as fast
	//   as possible from any of the hosts in the group "groupId"
	// . callback will be called when a good reply is gotten or when all
	//   hosts in the group have failed to give us a good reply
	// . generally, for querying for data make "sendToWholeGroup" false
	// . generally, when adding      data make "sendToWholeGroup" true
	// . "totalTimeout" is for the whole multicast
	// . "key" is used to pick a host in the group if sendToWholeGroup
	//   is false
	// . Msg40 uses largest termId in winning group as the key to ensure
	//   that queries with the same largest termId go to the same machine
	// . if you pass in a "replyBuf" we'll store the reply in there
	// . "doDiskLoadBalancing" is no longer used.
	bool send ( char       *msg             ,
		    int32_t        msgSize         ,
		    msg_type_t     msgType         ,
		    // does this Multicast own this "msg"? if so, it will
		    // free it when done with it.
		    bool        ownMsg          ,
		    uint32_t shardNum ,
		    // should the request be sent to all hosts in the group
		    // "groupId", or just one host. Msg1 adds data to all 
		    /// hosts in the group so it sets this to true.
		    bool        sendToWholeShard, // Group, 
		    // if "key" is not 0 then it is used to select
		    // a host in the group "groupId" to send to.
		    int32_t        key             ,
		    void       *state           , // callback state
		    void       *state2          , // callback state
		    void      (*callback)(void *state,void *state2),
		    int64_t        totalTimeout    , //relative timeout in milliseconds
		    int32_t        niceness        ,
		    int32_t        firstHostId,      // first host to try (-1=don't care)
		    bool           freeReplyBuf);    //normally true

	// . get the reply from your NON groupSend
	// . if *freeReply is true then you are responsible for freeing this 
	//   reply now, otherwise, don't free it
	char *getBestReply ( int32_t *replySize , 
			     int32_t *replyMaxSize, 
			     bool *freeReply ,
			     bool  steal = false);

	// free all non-NULL ptrs in all UdpSlots, and free m_msg
	void reset ( ) ;

	// private:

	// . stuff set directly by send() parameters
	char       *m_msg;
	int32_t        m_msgSize;
	msg_type_t     m_msgType;
	bool        m_ownMsg;

	class UdpSlot *m_slot;

	bool        m_inUse;

	// host we got reply from. used by Msg3a for timing.
	Host      *m_replyingHost;
	// when the request was launched to the m_replyingHost
	int64_t  m_replyLaunchTime;

private:
	GbMutex m_mtx;

	void       *m_state;
	void       *m_state2;
	void       (* m_callback)( void *state , void *state2 );
	int64_t       m_totalTimeout;   // in milliseconds

	// . m_slots[] is our list of concurrent transactions
	// . we delete all the slots only after cast is done
	int64_t        m_startTime;   // milliseconds since the epoch

	// # of replies we've received
	int32_t        m_numReplies;

	// . the group we're sending to or picking from
	// . up to MAX_HOSTS_PER_GROUP hosts
	struct HostSlot {
		Host       *m_hostPtr;
		bool        m_retired;          // hostIds that we've tried to send to but failed
		bool        m_inProgress;       // transaction in progress?
		UdpSlot    *m_slot;
		int32_t     m_errno;            // did we have an errno with this slot?
		int64_t     m_launchTime;
		HostSlot()
		  : m_hostPtr(NULL),
		    m_retired(false),
		    m_inProgress(false),
		    m_slot(NULL),
		    m_errno(0),
		    m_launchTime(0)
		{ }
		void reset() {
			m_hostPtr=NULL;
			m_retired=false;
			m_inProgress=false;
			m_slot=NULL;
			m_errno=0;
			m_launchTime=0;
		}
	} m_host[MAX_HOSTS_PER_GROUP];
	int32_t        m_numHosts;



	// steal this from the slot(s) we get
	char       *m_readBuf;
	int32_t        m_readBufSize;
	int32_t        m_readBufMaxSize;

	// we own it until caller calls getBestReply()
	bool        m_ownReadBuf;
	// are we registered for a callback every 1 second
	bool        m_registeredSleep;

	int32_t        m_niceness;

	// . last sending of the request to ONE host in a group (pick & send)
	// . in milliseconds
	int64_t   m_lastLaunch;

	// only free m_reply if this is true
	bool        m_freeReadBuf;

	int32_t        m_key;

	bool        m_sentToTwin;

	void getCandidateHostList(uint32_t shardNum, msg_type_t msgType, const char *msg, int32_t msgSize);

	void destroySlotsInProgress ( UdpSlot *slot );

	void sendToWholeGroup();

	int calculateTimeout();

	static void sleepCallback1Wrapper(int bogusfd, void *state);
	void sleepCallback1();
	static void sleepWrapper2(int bogusfd, void *state);
	static void gotReply1(void *state, UdpSlot *slot);
	void gotReply1(UdpSlot *slot);
	static void gotReply2(void *state, UdpSlot *slot);
	void gotReply2(UdpSlot *slot);

	bool sendToHostLoop(int32_t key, int32_t firstHostId);
	bool sendToHost    ( int32_t i ); 
	int32_t pickBestHost(uint32_t key, int32_t firstHostId);
	void closeUpShop   ( UdpSlot *slot ) ;
};

#endif // GB_MULTICAST_H
