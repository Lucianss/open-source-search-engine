// Matt Wells, Copyright Apr 2001

// . core class for handling interupts-based i/o on non-blocking descriptors
// . when an fd/state/callback is registered for reading we call your callback //   when fd has a read event (same for write and sleeping)

#ifndef GB_LOOP_H
#define GB_LOOP_H

#include "GbMutex.h"
#include <inttypes.h>


int gbsystem(const char *cmd);


/**
 * Print stack trace
 */
void printStackTrace (bool print_location = false);


class Slot;


// linux 2.2 kernel has this limitation
#define MAX_NUM_FDS 1024


// . niceness can only be 0, 1 or 2
// . we use 0 for query traffic
// . we use 1 for merge disk threads
// . we use 2 for indexing/spidering
// . 0  will use the high priority udp server, g_udpServer2
// . 1+ will use the low  priority udp server, g_udpServer
// . 0  niceness for disk threads will cancel other threads before launching
// . 1+ niceness threads will be set to lowest priority using setpriority()
// . 1  niceness disk thread, when running, will not allow niceness 2 to launch
//#define MAX_NICENESS 2




class Loop {

 public:

	// contructor and stuff
	Loop();
	~Loop();

	// free up all our mem
	void reset();

	// set up the signal handlers or block the signals for queueing
	bool init();
	
	// . call this to begin polling/selecting of all registed fds
	// . returns false on error
	[[ noreturn ]] void runLoop();

	// . register this "fd" with "callback"
	// . "callback" will be called when fd is ready for reading
	// . "timeout" is -1 if this never timesout
	bool registerReadCallback(int fd, void *state, void (*callback)(int fd, void *state),
	                          const char *description, int32_t niceness);

	// . register this "fd" with "callback"
	// . "callback" will be called when fd is ready for reading
	// . "callback" will be called when there is an error on fd
	bool registerWriteCallback(int fd, void *state, void (*callback)(int fd, void *state),
	                           const char *description, int32_t niceness);

	// . register this callback to be called every second
	// . TODO: implement "seconds" parameter
	bool registerSleepCallback(int32_t milliseconds, void *state, void (*callback)(int fd, void *state),
	                           const char *description, int32_t niceness = 1, bool immediate = false);

	// unregister call back for reading, writing or sleeping
	void unregisterReadCallback  ( int fd, void *state , void (* callback)(int fd,void *state) );
	void unregisterWriteCallback ( int fd, void *state , void (* callback)(int fd,void *state) );
	void unregisterSleepCallback ( void *state , void (* callback)(int fd,void *state) );

	// sets up for signal capture by us, g_loop
	bool setNonBlocking(int fd);

	// . keep this public so sighandler() can call it
	// . we also call it from HttpServer::getMsgPieceWrapper() to
	//   notify a socket that it's m_sendBuf got some new data to send
	void callCallbacks_ass (bool forReading, int fd, int64_t now = 0LL,
				int32_t niceness = -1 );

	void wakeupPollLoop();

	
	bool        m_isDoingLoop;

	// the sighupHandler() will set this to 1 when we receive
	// a SIGHUP, 2 if a thread crashed, 3 if we got a SIGPWR
	char m_shutdown;

	// called when sigqueue overflows and we gotta do a select() or poll()
	void doPoll ( );
 private:


	void unregisterCallback ( Slot **slots , int fd , void *state ,
				  void (* callback)(int fd,void *state) ,
				  bool forReading );

	bool addSlot(bool forReading, int fd, void *state, void (*callback)(int fd, void *state),
	             int32_t niceness, const char *description, int32_t tick = 0x7fffffff, bool immediate = false);

	// now we use a linked list of pre-allocated slots to avoid a malloc
	// failure which can cause the merge to dump with "URGENT MERGE FAILED"
	// message becaise it could not register the sleep wrapper to wait
	Slot *getEmptySlot (         ) ;
	void  returnSlot   ( Slot *s ) ;

	// . these arrays map an fd to a Slot (see above for Slot definition)
	// . that slot may chain to other slots if more than one procedure
	//   is waiting on a file to become available for reading/writing
	// . these fd's are real, not virtual
	// . m_read/writeFds[i] is NULL if no one is waiting on fd #i
	// . fd of MAX_NUM_FDS   is used for sleep callbacks
	// . fd of MAX_NUM_FDS+1 is used for thread exit callbacks
	Slot *m_readSlots  [MAX_NUM_FDS+2];
	Slot *m_writeSlots [MAX_NUM_FDS+2];

	// the minimal tick time in milliseconds (ms)
	int32_t m_minTick;

	// now we pre-allocate our slots to prevent nasty coredumps from merge
	// because it could not register a sleep callback with us
	Slot *m_slots;
	Slot *m_head;
	Slot *m_tail;
	Slot *m_callbacksNext; //in case we unregister the "next" callback

	GbMutex m_slotMutex; //protects all slot linked list modification and traversal
	
	int m_pipeFd[2]; //used for waking up from select/poll
	
	int64_t m_lastKeepaliveTimestamp;
};

extern class Loop g_loop;

#endif // GB_LOOP_H
