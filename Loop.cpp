#include "gb-include.h"

#include "Loop.h"
#include "JobScheduler.h"
#include "UdpServer.h"
#include "HttpServer.h" // g_httpServer.m_tcp.m_numQueued
#include "Profiler.h"
#include "Process.h"
#include "PageParser.h"
#include "Conf.h"
#include "ScopedLock.h"
#include "Mem.h"
#include "InstanceInfoExchange.h"

#include "Stats.h"
#include "GbDns.h"

#include <execinfo.h>
#include <sys/auxv.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>      // fcntl()
#include <sys/poll.h>   // POLLIN, POLLPRI, ...

// raised from 5000 to 10000 because we have more UdpSlots now and Multicast
// will call g_loop.registerSleepCallback() if it fails to get a UdpSlot to
// send on.
#define MAX_SLOTS 10000


// TODO: . if signal queue overflows another signal is sent
//       . capture that signal and use poll or something???

// Tricky Gotchas:
// TODO: if an event happens on a TCP fd/socket before we fully accept it
//       we should just register it then call the read callback in case
//       we just missed a ready for reading signal!!!!!
// TODO: signals can be gotten off the queue after we've closed an fd
//       in which case the handler should be removed from Loop's registry
//       BEFORE being closed... so the handler will be NULL... ???
// NOTE: keep in mind that the signals might be delayed or be really fast!

// TODO: don't mask signals, catch them as they arrive? (like in phhttpd)


class Slot {
 public:
	void   *m_state;
	void  (* m_callback)(int fd, void *state);
	// the next Slot thats registerd on this fd
	Slot   *m_next;

	// save niceness level for doPoll() to segregate
	int32_t    m_niceness;

	// this callback should be called every X milliseconds
	int32_t      m_tick;

	// when we were last called in ms time (only valid for sleep callbacks)
	int64_t m_lastCall;

	const char *m_description;

	// linked list of available slots
	Slot     *m_nextAvail;
};


// a global class extern'd in .h file
Loop g_loop;


// free up all our mem
void Loop::reset() {
	if ( m_slots ) {
		log(LOG_DEBUG,"db: resetting loop");
		mfree ( m_slots , MAX_SLOTS * sizeof(Slot) , "Loop" );
	}
	m_slots = NULL;
}

static void sigpwrHandler ( int x , siginfo_t *info , void *y ) ;
static void sighupHandler ( int x , siginfo_t *info , void *y ) ;
static void sigprofHandler(int signo, siginfo_t *info, void *context);

void Loop::unregisterReadCallback ( int fd, void *state , void (* callback)(int fd,void *state) ){
	if ( fd < 0 ) return;
	// from reading
	unregisterCallback ( m_readSlots,fd, state , callback, true);
}

void Loop::unregisterWriteCallback ( int fd, void *state , void (* callback)(int fd,void *state)){
	// from writing
	unregisterCallback ( m_writeSlots , fd  , state,callback,false);
}

void Loop::unregisterSleepCallback ( void *state , void (* callback)(int fd,void *state)){
	unregisterCallback (m_readSlots,MAX_NUM_FDS,state,callback,true);
}

static fd_set s_selectMaskRead;
static fd_set s_selectMaskWrite;

static int s_readFds[MAX_NUM_FDS];
static int32_t s_numReadFds = 0;
static int s_writeFds[MAX_NUM_FDS];
static int32_t s_numWriteFds = 0;

void Loop::unregisterCallback(Slot **slots, int fd, void *state, void (* callback)(int fd,void *state), bool forReading) {
	// bad fd
	if(fd<0) {
		log(LOG_LOGIC, "loop: fd to unregister is negative.");
		return;
	}
	// . keep track of new min tick for sleep callbacks
	// . sleep a min of 40ms so g_now is somewhat up to date
	int32_t min     = 40; // 0x7fffffff;

	ScopedLock sl(m_slotMutex);
	// chase through all callbacks registered with this fd
	Slot *prevSlot = NULL;
	for(Slot *s = slots[fd]; s; ) {
		// get the next slot (NULL if no more)
		Slot *next = s->m_next;
		if(s->m_callback == callback &&
		   s->m_state    == state) {
			// free this slot since it callback matches "callback"
			returnSlot ( s );
			// if the last one, then remove the FD from s_fdList
			// so and clear a bit so doPoll() function is fast
			if(slots[fd] == s && s->m_next == NULL) {
				if(forReading) {
					for(int32_t i = 0; i < s_numReadFds; i++ ) {
						if (s_readFds[i] == fd) {
							s_readFds[i] = s_readFds[s_numReadFds-1];
							s_numReadFds--;
							// remove from select mask too
							FD_CLR(fd,&s_selectMaskRead );
							if(g_conf.m_logDebugLoop || g_conf.m_logDebugTcp) {
								log( "loop: unregistering read callback for fd=%i", fd );
							}
							break;
						}
					}
				} else {
					for(int32_t i = 0; i < s_numWriteFds; i++ ) {
						if(s_writeFds[i] == fd) {
							s_writeFds[i] = s_writeFds[s_numWriteFds-1];
							s_numWriteFds--;
							// remove from select mask too
							FD_CLR(fd,&s_selectMaskWrite);
							if(g_conf.m_logDebugLoop || g_conf.m_logDebugTcp) {
								log( LOG_DEBUG, "loop: unregistering write callback for fd=%" PRId32" from write #wrts=%" PRId32,
								     ( int32_t ) fd, ( int32_t ) s_numWriteFds );
							}
							break;
						}
					}
				}
			}
			// excise the previous slot from linked list
			if(prevSlot)
				prevSlot->m_next = next;
			else
				slots[fd]        = next;
			// watch out if we're in the previous callback, we need to
			// fix the linked list in callCallbacks_ass
			if(m_callbacksNext == s)
				m_callbacksNext = next;
		} else {
			prevSlot = s;
			// if we're unregistering a sleep callback
			// we might have to recalculate m_minTick
			if(s->m_tick < min)
				min = s->m_tick;
		}
		// advance to the next slot
		s = next;
	}
	// set our new minTick if we were unregistering a sleep callback
	if ( fd == MAX_NUM_FDS ) {
		m_minTick = min;
	}

	return;
}

bool Loop::registerReadCallback(int fd, void *state, void (*callback)(int fd, void *state),
                                const char *description, int32_t niceness) {
	// the "true" answers the question "for reading?"
	if (addSlot(true, fd, state, callback, niceness, description)) {
		return true;
	}

	log( LOG_WARN, "loop: Unable to register read callback." );
	return false;
}


bool Loop::registerWriteCallback(int fd, void *state, void (*callback)(int fd, void *state),
                                 const char *description, int32_t niceness) {
	// the "false" answers the question "for reading?"
	if (addSlot(false, fd, state, callback, niceness, description)) {
		return true;
	}

	log( LOG_WARN, "loop: Unable to register write callback.");
	return false;
}

// tick is in milliseconds
bool Loop::registerSleepCallback(int32_t tick, void *state, void (*callback)(int fd, void *state),
                                 const char *description, int32_t niceness, bool immediate) {
	if (!addSlot(true, MAX_NUM_FDS, state, callback, niceness, description, tick, immediate)) {
		log( LOG_WARN, "loop: Unable to register sleep callback" );
		return false;
	}

	ScopedLock sl(m_slotMutex);
	if ( tick < m_minTick ) {
		m_minTick = tick;
	}

	return true;
}

// . returns false and sets g_errno on error
bool Loop::addSlot(bool forReading, int fd, void *state, void (*callback)(int fd, void *state),
                   int32_t niceness, const char *description, int32_t tick, bool immediate) {
	// ensure fd is >= 0
	if ( fd < 0 ) {
		g_errno = EBADENGINEER;
		log(LOG_LOGIC,"loop: fd to register is negative.");
		return false;
	}
	// sanity
	if ( fd > MAX_NUM_FDS ) {
		log(LOG_ERROR, "loop: bad fd of %" PRId32,(int32_t)fd);
		g_process.shutdownAbort(true);
	}

	if ( g_conf.m_logDebugLoop || g_conf.m_logDebugTcp ) {
		log( LOG_DEBUG, "loop: registering %s callback sd=%i", forReading ? "read" : "write", fd);
	}

	ScopedLock sl(m_slotMutex);
	// . ensure fd not already registered with this callback/state
	// . prevent dups so you can keep calling register w/o fear
	Slot *s;
	if ( forReading ) {
		s = m_readSlots  [ fd ];
	} else {
		s = m_writeSlots [ fd ];
	}

	while ( s ) {
		if ( s->m_callback == callback &&
		     s->m_state    == state      ) {
			// don't set g_errno for this anymore, just bitch
			//g_errno = EBADENGINEER;
			log(LOG_LOGIC,"loop: fd=%i is already registered.",fd);
			return true;
		}
		s = s->m_next;
	}
	// . make a new slot
	// . TODO: implement mprimealloc() to pre-alloc slots for us for speed
	//s = (Slot *) mmalloc ( sizeof(Slot ) ,"Loop");
	s = getEmptySlot ( );
	if ( ! s ) return false;
	// for pointing to slot already in position for fd
	Slot *next ;
	// store ourselves in the slot for this fd
	if ( forReading ) {
		next = m_readSlots [ fd ];
		m_readSlots  [ fd ] = s;
		// if not already registered, add to list
		if ( fd < MAX_NUM_FDS && ! FD_ISSET( fd,&s_selectMaskRead ) ) {
			// sanity
			if ( s_numReadFds >= MAX_NUM_FDS){
				g_process.shutdownAbort(true);
			}
			
			s_readFds[s_numReadFds++] = fd;
			FD_SET ( fd,&s_selectMaskRead  );
		}
	}
	else {
	 	next = m_writeSlots [ fd ];
	 	m_writeSlots [ fd ] = s;
	 	//FD_SET ( fd , &m_writefds );
	 	// if not already registered, add to list
	 	if ( fd<MAX_NUM_FDS && ! FD_ISSET ( fd,&s_selectMaskWrite ) ) {
	 		// sanity
	 		if ( s_numWriteFds>=MAX_NUM_FDS){
			    g_process.shutdownAbort(true);
		    }

	 		s_writeFds[s_numWriteFds++] = fd;
	 		FD_SET ( fd,&s_selectMaskWrite  );
	 	}
	}
	// set our callback and state
	s->m_callback  = callback;
	s->m_state     = state;
	s->m_description = description;

	// point to the guy that was registered for fd before us
	s->m_next      = next;

	// save our niceness for doPoll()
	s->m_niceness  = niceness;

	// store the tick for sleep wrappers (should be max for others)
	s->m_tick      = tick;

	// the last called time
	s->m_lastCall = immediate ? 0 : gettimeofdayInMilliseconds();

	// if fd == MAX_NUM_FDS if it's a sleep callback
	if ( fd == MAX_NUM_FDS ) {
		return true;
	}

	// watch out for big bogus fds used for thread exit callbacks
	if ( fd >  MAX_NUM_FDS ) {
		return true;
	}

	// set fd non-blocking
	return setNonBlocking(fd);
}

// . now make sure we're listening for an interrupt on this fd
// . set it non-blocing and enable signal catching for it
// . listen for an interrupt for this fd
bool Loop::setNonBlocking(int fd) {
	int flags = fcntl ( fd , F_GETFL ) ;
	if ( flags < 0 ) {
		g_errno = errno;
		log( LOG_WARN, "loop: fcntl(F_GETFL): %s.",strerror(errno));
		return false;
	}

	if ( fcntl ( fd, F_SETFL, flags|O_NONBLOCK) < 0 ) {
		g_errno = errno;
		log( LOG_WARN, "loop: fcntl(NONBLOCK): %s.",strerror(errno));
		return false;
	}

	// we use select()/poll now so skip stuff below
	return true;
}

// . if "forReading" is true  call callbacks registered for reading on "fd"
// . if "forReading" is false call callbacks registered for writing on "fd"
// . if fd is MAX_NUM_FDS and "forReading" is true call all sleepy callbacks
void Loop::callCallbacks_ass ( bool forReading , int fd , int64_t now , int32_t niceness ) {
	// save the g_errno to send to all callbacks
	int saved_errno = g_errno;

	ScopedLock sl(m_slotMutex);
	// get the first Slot in the chain that is waiting on this fd
	Slot *s ;
	if ( forReading ) s = m_readSlots  [ fd ];
	else              s = m_writeSlots [ fd ];
	//s = m_readSlots [ fd ];
	// ensure we called something
	int32_t numCalled = 0;

	// . now call all the callbacks
	// . most will re-register themselves (i.e. call registerCallback...()
	while ( s ) {
		// skip this slot if he has no callback
		if ( ! s->m_callback ) {
			continue;
		}

		// watch out if clock was set back
		if ( s->m_lastCall > now ) {
			s->m_lastCall = now;
		}

		// if we're a sleep callback, check to make sure not premature
		if ( fd == MAX_NUM_FDS && s->m_lastCall + s->m_tick > now ) {
			s = s->m_next;
			continue;
		}

		// skip if not a niceness match
		if ( niceness == 0 && s->m_niceness != 0 ) {
			s = s->m_next;
			continue;
		}

		// update the lastCall timestamp for this slot
		if ( fd == MAX_NUM_FDS ) {
			s->m_lastCall = now;
		}

		// do the callback

		// NOTE: callback can unregister fd for Slot s, so get next
		m_callbacksNext = s->m_next;

		logDebug(g_conf.m_logDebugLoop, "loop: enter fd callback '%s' fd=%d nice=%" PRId32,
		         s->m_description, fd, s->m_niceness);

		// sanity check. -1 no longer supported
		if (s->m_niceness < 0) {
			g_process.shutdownAbort(true);
		}

		int64_t took = 0;

		m_slotMutex.unlock();
		{
			int64_t start = gettimeofdayInMilliseconds();
			s->m_callback(fd, s->m_state);
			took = gettimeofdayInMilliseconds() - start;
		}
		m_slotMutex.lock();

		if (took > g_conf.m_logLoopTimeThreshold) {
			log(LOG_WARN, "loop: %s took %" PRId64"ms", s->m_description, took);
		}

		logDebug(g_conf.m_logDebugLoop, "loop: exit fd callback '%s' fd=%d nice=%" PRId32,
		         s->m_description, fd, s->m_niceness);

		// inc the flag
		numCalled++;
		// reset g_errno so all callbacks for this fd get same g_errno
		g_errno = saved_errno;
		// get the next n (will be -1 if no slot after it)
		s = m_callbacksNext;
	}

	m_callbacksNext = NULL;
}

Loop::Loop()
  : m_callbacksNext(NULL),
    m_slotMutex(),
    m_lastKeepaliveTimestamp(0)
{
	m_isDoingLoop      = false;

	// set all callbacks to NULL so we know they're empty
	for ( int32_t i = 0 ; i < MAX_NUM_FDS+2 ; i++ ) {
		m_readSlots [i] = NULL;
		m_writeSlots[i] = NULL;
	}
	// the extra sleep slots
	//m_readSlots [ MAX_NUM_FDS ] = NULL;
	m_slots = NULL;
	m_pipeFd[0] = -1;
	m_pipeFd[1] = -1;
	m_shutdown = 0;
	m_minTick = 40;
	m_head = NULL;
	m_tail = NULL;
}

// free all slots from addSlots
Loop::~Loop ( ) {
	reset();
	if(m_pipeFd[0]>=0) {
		close(m_pipeFd[0]);
		m_pipeFd[0] = -1;
	}
	if(m_pipeFd[1]>=0) {
		close(m_pipeFd[1]);
		m_pipeFd[1] = -1;
	}
}

// returns NULL and sets g_errno if none are left
Slot *Loop::getEmptySlot ( ) {
	Slot *s = m_head;
	if ( ! s ) {
		g_errno = EBUFTOOSMALL;
		log(LOG_WARN, "loop: No empty slots available. Increase #define MAX_SLOTS.");
		return NULL;
	}
	m_head = s->m_nextAvail;
	return s;
}

void Loop::returnSlot ( Slot *s ) {
	s->m_nextAvail = m_head;
	m_head = s;
}


bool Loop::init ( ) {

	// clear this up here before using in doPoll()
	FD_ZERO(&s_selectMaskRead);
	FD_ZERO(&s_selectMaskWrite);

	// set-up wakeup pipe
	if(pipe(m_pipeFd)!=0) {
		log(LOG_ERROR,"pipe() failed with errno=%d",errno);
		return false;
	}
	setNonBlocking(m_pipeFd[0]);
	setNonBlocking(m_pipeFd[1]);
	FD_SET(m_pipeFd[0],&s_selectMaskRead);

	// sighupHandler() will set this to true so we know when to shutdown
	m_shutdown  = 0;
	// . reset this cuz we have no sleep callbacks right now
	// . sleep a min of 40ms so g_now is somewhat up to date
	m_minTick = 40; //0x7fffffff;
	// make slots
	m_slots = (Slot *) mmalloc ( MAX_SLOTS * (int32_t)sizeof(Slot) , "Loop" );
	if ( ! m_slots ) return false;
	// log it
	log(LOG_DEBUG,"loop: Allocated %" PRId32" bytes for %" PRId32" callbacks.",
	     MAX_SLOTS * (int32_t)sizeof(Slot),(int32_t)MAX_SLOTS);
	// init link list ptr
	for ( int32_t i = 0 ; i < MAX_SLOTS - 1 ; i++ ) {
		m_slots[i].m_nextAvail = &m_slots[i+1];
	}
	m_slots[MAX_SLOTS - 1].m_nextAvail = NULL;
	m_head = &m_slots[0];
	m_tail = &m_slots[MAX_SLOTS - 1];
	// an innocent log msg
	//log ( 0 , "Loop: starting the i/o loop");
	// . when using threads GB_SIGRTMIN becomes 35, not 32 anymore
	//   since threads use these signals to reactivate suspended threads
	// . debug msg
	//log("admin: GB_SIGRTMIN=%" PRId32, (int32_t)GB_SIGRTMIN );
	// . block the GB_SIGRTMIN signal
	// . anytime this is raised it goes onto the signal queue
	// . we use sigtimedwait() to get signals off the queue
	// . sigtimedwait() selects the lowest signo first for handling
	// . therefore, GB_SIGRTMIN is higher priority than (GB_SIGRTMIN + 1)
	//sigfillset ( &sigs );

	struct sigaction actSigPipe;
	//Ignore SIGPIPE. We want a plain error return instead from system calls,.
	actSigPipe.sa_handler = SIG_IGN;
	sigemptyset(&actSigPipe.sa_mask);
	actSigPipe.sa_flags = 0;
	sigaction(SIGPIPE,&actSigPipe,NULL);

	// handle SIGHUP and SIGTERM signals gracefully by saving and shutting down
	struct sigaction saShutdown;
	sigemptyset(&saShutdown.sa_mask);
	saShutdown.sa_flags = SA_SIGINFO | SA_RESTART;
	saShutdown.sa_sigaction = sighupHandler;
	sigaction(SIGHUP, &saShutdown, NULL);
	sigaction(SIGTERM, &saShutdown, NULL);

	// if the UPS is about to go off it sends a SIGPWR
	struct sigaction saPower;
	sigemptyset(&saPower.sa_mask);
	saPower.sa_flags = SA_SIGINFO | SA_RESTART;
	saPower.sa_sigaction = sigpwrHandler;
	sigaction(SIGPWR, &saPower, NULL);

	//SIGPROF is used by the profiler
	struct sigaction saProfile;
	sigemptyset(&saProfile.sa_mask);
	saProfile.sa_flags  = SA_SIGINFO | SA_RESTART;
	saProfile.sa_sigaction = sigprofHandler;
	sigaction(SIGPROF, &saProfile, NULL);
	// setitimer(ITIMER_PROF...) is called when profiling is enabled/disabled
	// it has noticeable overhead so it must not be enabled by default.

	// success
	return true;
}

// TODO: if we get a segfault while saving, what then?
void sigpwrHandler ( int x , siginfo_t *info , void *y ) {
	// let main process know to shutdown
	g_loop.m_shutdown = 3;
}

void printStackTrace (bool print_location) {
	logf(LOG_ERROR, "gb: Printing stack trace");

	static void *s_bt[200];
	size_t sz = backtrace(s_bt, 200);

	// find ourself
	const char* process = (const char*)getauxval(AT_EXECFN);

	for( size_t i = 0; i < sz; ++i ) {
		char cmd[256];
		sprintf(cmd,"addr2line -e %s 0x%" PRIx64, process, (uint64_t)s_bt[i]);
		logf(LOG_ERROR, "%s", cmd);
	}
}


static void sigprofHandler(int signo, siginfo_t *info, void *context)
{
	//This is called on SIGPROF meaning that profiling is enabled
	g_profiler.getStackFrame();
}

// shit, we can't make this realtime!! RdbClose() cannot be called by a
// real time sig handler
void sighupHandler ( int x , siginfo_t *info , void *y ) {
	// let main process know to shutdown
	g_loop.m_shutdown = 1;
}

// . keep a timestamp for the last time we called the sleep callbacks
// . we have to call those every 1 second
static int64_t s_lastTime = 0;

void Loop::runLoop ( ) {
	s_lastTime = 0;

	m_isDoingLoop = true;

	// . now loop forever waiting for signals
	// . but every second check for timer-based events
	for (;;) {
		g_errno = 0;

		if ( m_shutdown ) {
			// a msg
			if (m_shutdown == 1) {
				log(LOG_INIT,"loop: got SIGHUP or SIGTERM.");
			} else if (m_shutdown == 2) {
				log(LOG_INIT,"loop: got SIGBAD in thread.");
			} else {
				log(LOG_INIT,"loop: got SIGPWR.");
			}

			// . turn off interrupts here because it doesn't help to do
			//   it in the thread
			// . TODO: turn off signals for sigbadhandler()
			// if thread got the signal, just wait for him to save all
			// Rdbs and then dump core
			if ( m_shutdown == 2 ) {
				//log(0,"Thread is saving & shutting down urgently.");
				//log("loop: Resuming despite thread crash.");
				//m_shutdown = 0;
				//goto BIGLOOP;
			}

			// otherwise, thread did not save, so we must do it
			log ( LOG_INIT ,"loop: Saving and shutting down urgently.");

			g_process.shutdown ( true );
		}

		//
		//
		// THE HEART OF GB. process events/signals on FDs.
		//
		//
		doPoll();
	}

}


static void cleanupFinishedJobs() {
	if(g_conf.m_maxJobCleanupTime<=0)
		g_jobScheduler.cleanup_finished_jobs();
	else {
		struct timespec ts_start;
		clock_gettime(CLOCK_MONOTONIC,&ts_start);
		
		g_jobScheduler.cleanup_finished_jobs();
		
		struct timespec ts_end;
		clock_gettime(CLOCK_MONOTONIC,&ts_end);
		
		int msecs = (ts_end.tv_sec - ts_start.tv_sec)*1000
		          + (ts_end.tv_nsec - ts_start.tv_nsec)/1000000;
		if(msecs>g_conf.m_maxJobCleanupTime) {
			log(LOG_LOGIC,"Cleaning up job(s) took %dms hwich is more than allowed. Dumping core",msecs);
			g_process.shutdownAbort();
		}
	}
}


//--- TODO: flush the signal queue after polling until done
//--- are we getting stale signals resolved by flush so we get
//--- read event on a socket that isnt in read mode???
// TODO: set signal handler to SIG_DFL to prevent signals from queuing up now
// . this handles high priority fds first (lowest niceness)
void Loop::doPoll ( ) {
	// set time
	//g_now = gettimeofdayInMilliseconds();

	logDebug( g_conf.m_logDebugLoop, "loop: Entered doPoll." );

	if(g_udpServer.needBottom()) {
		g_udpServer.makeCallbacks(1);
	}

	if(m_lastKeepaliveTimestamp + g_conf.m_vagusKeepaliveSendInterval <= gettimeofdayInMilliseconds()) {
		m_lastKeepaliveTimestamp = gettimeofdayInMilliseconds();
		InstanceInfoExchange::weAreAlive();
	}

	GbDns::makeCallbacks();

	int32_t n;

	timeval v;
	v.tv_sec  = 0;
	// 10ms for sleepcallbacks so they can be called...
	// and we need this to be the same as sigalrmhandler() since we
	// keep track of cpu usage here too, since sigalrmhandler is "VT"
	// based it only goes off when that much "cpu time" has elapsed.
	v.tv_usec = 10 * 1000;

	// gotta copy to our own since bits get cleared by select() function
	fd_set readfds = s_selectMaskRead;
	fd_set writefds = s_selectMaskWrite;

	logDebug( g_conf.m_logDebugLoop, "loop: in select" );

	// . poll the fd's searching for socket closes
	// . the sigalrms and sigvtalrms and SIGCHLDs knock us out of this
	//   select() with n < 0 and errno equal to EINTR.
	// . crap the sigalarms kick us out here every 1ms. i noticed
	//   then when running disableTimer() above and we don't get
	//   any EINTRs... can we mask those out here? it only seems to be
	//   the SIGALRMs not the SIGVTALRMs that interrupt us.
	n = select (MAX_NUM_FDS,
		    &readfds,
		    &writefds,
		    NULL,//&exceptfds,
		    &v );

	if(n<0) {
		g_errno = errno;
		log( LOG_WARN, "loop: select: %s.", strerror( g_errno ) );
		return;
	}
	
	errno = 0;

	logDebug( g_conf.m_logDebugLoop, "loop: select() returned %d", n);

	if (g_conf.m_logDebugLoop || g_conf.m_logDebugTcp) {
		for ( int32_t i = 0; i < MAX_NUM_FDS; i++) {
			// continue if not set for reading
			if ( FD_ISSET ( i, &readfds ) ) {
				log( LOG_DEBUG, "loop: fd=%" PRId32" is on for read", i);
			}
			if ( FD_ISSET ( i, &writefds ) ) {
				log( LOG_DEBUG, "loop: fd=%" PRId32" is on for write", i);
			}
			// if niceness is not -1, handle it below
		}
	}

	cleanupFinishedJobs();

	const int64_t now = gettimeofdayInMilliseconds();

	if( n > 0 && FD_ISSET( m_pipeFd[0], &readfds ) ) {
		//drain the wakeup pipe
		char buf[32];
		ssize_t ignored __attribute__((unused)) = read( m_pipeFd[0], buf, sizeof(buf) ); // shut up gcc warning: ignoring return value
		n--;
		FD_CLR( m_pipeFd[0], &readfds );
	}

	// now keep this fast, too. just check fds we need to.
	for ( int32_t i = 0 ; i < s_numReadFds ; i++ ) {
		if ( n == 0 ) break;
		int fd = s_readFds[i];
		Slot *s = m_readSlots  [ fd ];
	 	// if niceness is not 0, handle it below
		if ( s && s->m_niceness > 0 ) continue;
		// must be set
		if ( ! FD_ISSET ( fd , &readfds ) ) continue;
		if ( g_conf.m_logDebugLoop || g_conf.m_logDebugTcp ) {
			log( LOG_DEBUG, "loop: calling cback0 niceness=%" PRId32" fd=%i", s ? s->m_niceness : -1, fd );
		}
		callCallbacks_ass (true,fd, now,0);//read?
	}
	for ( int32_t i = 0 ; i < s_numWriteFds ; i++ ) {
		if ( n == 0 ) break;
		int fd = s_writeFds[i];
		Slot *s = m_writeSlots  [ fd ];
	 	// if niceness is not 0, handle it below
		if ( s && s->m_niceness > 0 ) continue;
		// fds are always ready for writing so take this out.
		if ( ! FD_ISSET ( fd , &writefds ) ) continue;
		if ( g_conf.m_logDebugLoop || g_conf.m_logDebugTcp ) {
			log( LOG_DEBUG, "loop: calling wcback0 niceness=%" PRId32" fd=%i", s ? s->m_niceness : -1, fd );
		}
		callCallbacks_ass (false,fd, now,0);//false=forRead?
	}

	cleanupFinishedJobs();

	// now for lower priority fds
	for ( int32_t i = 0 ; i < s_numReadFds ; i++ ) {
		if ( n == 0 ) break;
		int fd = s_readFds[i];
		Slot *s = m_readSlots  [ fd ];
	  	// if niceness is <= 0 we did it above
		if ( s && s->m_niceness <= 0 ) continue;
		// must be set
		if ( ! FD_ISSET ( fd , &readfds ) ) continue;
		if ( g_conf.m_logDebugLoop || g_conf.m_logDebugTcp ) {
			log( LOG_DEBUG, "loop: calling cback1 fd=%i", fd );
		}
		callCallbacks_ass (true,fd, now,1);//read?
	}

	for ( int32_t i = 0 ; i < s_numWriteFds ; i++ ) {
		if ( n == 0 ) {
			break;
		}

	 	int fd = s_writeFds[i];
		Slot *s = m_writeSlots[fd];

	  	// if niceness is <= 0 we did it above
	 	if ( s && s->m_niceness <= 0 ) {
	 		continue;
	 	}
	 	// must be set
	 	if ( ! FD_ISSET ( fd , &writefds ) ) {
	 		continue;
	 	}
		if ( g_conf.m_logDebugLoop || g_conf.m_logDebugTcp ) {
			if( s ) {
				log( LOG_DEBUG, "loop: calling wcback1 niceness=%" PRId32" fd=%i", s->m_niceness, fd );
			}
			else {
				log( LOG_WARN, "loop: calling wcback1. Slot not found! fd=%i", fd );
			}
		}
		callCallbacks_ass(false, fd, now, 1);//forread?
	}

	cleanupFinishedJobs();

	// call sleepers if they need it
	// call this every (about) 1 second
	int32_t elapsed = gettimeofdayInMilliseconds() - s_lastTime;
	// if someone changed the system clock on us, this could be negative
	// so fix it! otherwise, times may NEVER get called in our lifetime
	if ( elapsed < 0 ) {
		elapsed = m_minTick;
	}

	if ( elapsed >= m_minTick ) {
		// MAX_NUM_FDS is the fd for sleep callbacks
		callCallbacks_ass ( true , MAX_NUM_FDS , gettimeofdayInMilliseconds() );
		// note the last time we called them
		s_lastTime = gettimeofdayInMilliseconds();
		cleanupFinishedJobs();
	}

	logDebug( g_conf.m_logDebugLoop, "loop: Exited doPoll.");
}


void Loop::wakeupPollLoop() {
	char dummy='d';
	ssize_t ignored __attribute__((unused)) = write(m_pipeFd[1],&dummy,1);;	// shut up gcc warning: ignoring return value
}


int gbsystem(const char *cmd ) {
	log("gb: running system(\"%s\")",cmd);
	int ret = system(cmd);
	return ret;
}
