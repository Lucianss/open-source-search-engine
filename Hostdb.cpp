#include "Hostdb.h"
#include "UdpServer.h"
#include "Conf.h"
#include "JobScheduler.h"
#include "max_niceness.h"
#include "Process.h"
#include "Sanity.h"
#include "sort.h"
#include "Rdb.h"
#include "Posdb.h"
#include "Titledb.h"
#include "Spider.h"
#include "Clusterdb.h"
#include "HostFlags.h"
#include "Dns.h"
#include "File.h"
#include "IPAddressChecks.h"
#include "ip.h"
#include "Mem.h"
#include "ScopedLock.h"
#include <fcntl.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <sys/types.h>

// a global class extern'd in .h file
Hostdb g_hostdb;

static HashTableX g_hostTableUdp;
static HashTableX g_hostTableTcp;


void Hostdb::resetPortTables () {
	g_hostTableUdp.reset();
	g_hostTableTcp.reset();
}

static int cmp  ( const void *h1 , const void *h2 ) ;

Hostdb::Hostdb ( ) {
	m_hosts = NULL;
	m_numHosts = 0;
	m_ips = NULL;
	m_initialized = false;
	m_crcValid = false;
	m_crc = 0;
	m_created = false;
	m_myHost = NULL;
	m_myIp = 0;
	m_myPort = 0;
	m_myHost = NULL;
	m_myShard = NULL;
	m_loopbackIp = atoip ( "127.0.0.1" , 9 );
	m_numHosts = 0;
	m_numHostsAlive = 0;
	m_allocSize = 0;
	m_numHostsPerShard  = 0;
	m_numStripeHostsPerShard = 0;
	m_bufSize = 0;
	m_numIps = 0;
	m_myHostId = 0;
	m_numShards = 0;
	m_indexSplits = 0;
	m_numSpareHosts = 0;
	m_numProxyHosts = 0;
	m_numProxyAlive = 0;
	m_numTotalHosts = 0;
	m_useTmpCluster = false;

	memset(m_hostPtrs, 0, sizeof(m_hostPtrs));
	memset(m_shards, 0, sizeof(m_shards));
	memset(m_spareHosts, 0, sizeof(m_spareHosts));
	memset(m_proxyHosts, 0, sizeof(m_proxyHosts));
	memset(m_buf, 0, sizeof(m_buf));
	memset(m_dir, 0, sizeof(m_dir));
	memset(m_httpRootDir, 0, sizeof(m_httpRootDir));
	memset(m_logFilename, 0, sizeof(m_logFilename));
	memset(m_map, 0, sizeof(m_map));
	
	m_hostsConfInDisagreement = false;
	m_hostsConfInAgreement = false;
	
	m_minRepairMode = -1;
	m_minRepairModeBesides0 = -1;
	m_minRepairModeHost = NULL;
}


Hostdb::~Hostdb () {
	reset();
}

void Hostdb::reset ( ) {
	if ( m_hosts )
		mfree ( m_hosts, m_allocSize,"Hostdb" );
	if ( m_ips   ) mfree ( m_ips  , m_numIps * 4, "Hostdb" );
	m_hosts = NULL;
	m_ips               = NULL;
	m_numIps            = 0;
}

// . gets filename that contains the hosts from the Conf file
// . return false on errro
// . g_errno may NOT be set
bool Hostdb::init(int32_t hostIdArg, bool proxyHost, bool useTmpCluster, bool initMyHost, const char *cwd) {
	// reset my ip and port
	m_myIp             = 0;
	m_myPort           = 0;
	m_myHost           = NULL;
	//m_myPort2          = 0;
	m_numHosts         = 0;
	m_numHostsPerShard = 0;
	m_numStripeHostsPerShard = 0;
	m_loopbackIp       = atoip ( "127.0.0.1" , 9 );
	m_useTmpCluster    = useTmpCluster;
	m_initialized = true;

	const char *dir = "./";
	if (cwd) {
		dir = cwd;
	}

	const char *filename = "hosts.conf";

	// for now we autodetermine
	if ( hostIdArg != -1 ) {
		g_process.shutdownAbort(true);
	}

	// init to -1
	m_myHostId = -1;

retry:
	// . File::open() open old if it exists, otherwise,
	File f;
	f.set ( dir , filename );
	// . returns -1 on error and sets g_errno
	// . returns false if does not exist, true otherwise
	int32_t status = f.doesExist();
	int32_t numRead;

	// return false on error (g_errno should be set)
	if ( status <= -1 ) return false;
	// return false if the conf file does not exist
	if ( status ==  0 ) { 
		g_errno = ENOHOSTSFILE; 
		// now we generate one if that is not there
createFile:
		if ( ! m_created ) {
			m_created = true;
			g_errno = 0;
			dir = cwd;
			createHostsConf( cwd );
			goto retry;
		}
		log(LOG_WARN, "conf: Filename %s does not exist." ,filename);
		return false; 
	}

	// get file size
	m_bufSize = f.getFileSize();

	// return false if too big
	if ( m_bufSize > (MAX_HOSTS+MAX_SPARES) * 128 ) { 
		g_errno = EBUFTOOSMALL; 
		log(LOG_WARN, "conf: %s has filesize of %" PRId32" bytes, which is greater than %" PRId32" max.",
		    filename,m_bufSize, (int32_t)(MAX_HOSTS+MAX_SPARES)*128);
		return false;
	}

	// open the file
	if ( ! f.open ( O_RDONLY ) ) {
		return false;
	}

	// read in the file
	numRead = f.read ( m_buf , m_bufSize , 0 /*offset*/ );
	// ensure g_errno is now set if numRead != m_bufSize
	if ( numRead != m_bufSize ) {
		log( LOG_WARN, "conf: Error reading %s : %s.", filename, mstrerror( g_errno ) );
		return false;
	}

	// NULL terminate what we read
	m_buf [ m_bufSize ] = '\0';

	// how many hosts do we have?
	char *p    = m_buf;
	char *pend = m_buf + m_bufSize;
	int32_t  i = 0;
	m_numSpareHosts = 0;
	m_numProxyHosts = 0;
	m_numHosts      = 0;
	
	for ( ; *p ; p++ ) {
		if ( is_wspace_a (*p) ) continue;
		// skip comments
		if ( *p == '#' ) { while ( *p && *p != '\n' ) p++; continue; }
		// MUST be a number
		if ( ! is_digit ( *p ) ) {
			// skip known directives
			if (!strncmp(p, "port-offset:", 12) ||
			    !strncmp(p, "index-splits:", 13) ||
			    !strncmp(p, "num-mirrors:", 12) ||
			    !strncmp(p, "working-dir:", 12)) {
				// no op
			}else if (strncasecmp(p, "spare", 5) == 0) {
				// check if this is a spare host
				m_numSpareHosts++;
			} else if (strncasecmp(p, "proxy", 5) == 0) {
				// check if this is a proxy host
				m_numProxyHosts++;
			} else if (strncasecmp(p, "qcproxy", 7) == 0) {
				// query compression proxies count as proxies
				m_numProxyHosts++;
			} else if (strncasecmp(p, "scproxy", 7) == 0) {
				// spider compression proxies count as proxies
				m_numProxyHosts++;
			} else {
				log(LOG_WARN, "conf: %s is malformed. First item of each non-comment line must be a NUMERIC hostId, "
					"SPARE or PROXY. line=%s", filename, p);
				return false;
			}
		} else {
			// count it as a host
			m_numHosts++;
		}
		i++;

		// skip line
		while ( *p && *p != '\n' ) {
			p++;
		}
	}

	// set g_errno, log and return false if no hosts found in the file
	if ( i == 0 ) { 
		g_errno = ENOHOSTS; 
		log( LOG_WARN, "conf: No host entries found in %s.",filename);
		goto createFile;
	}
	// alloc space for this many Hosts structures
	// save buffer size
	m_allocSize = sizeof(Host) * i;
	m_hosts = (Host *) mcalloc ( m_allocSize ,"Hostdb");
	if ( ! m_hosts ) {
		log( LOG_WARN, "conf: Memory allocation failed.");
		return false;
	}

	int32_t numGrunts = 0;

	// now fill up m_hosts
	p = m_buf;
	i = 0;
	int32_t line = 1;
	int32_t proxyNum = 0;

	// assume defaults
	int32_t indexSplits = 0;
	char *wdir2 = NULL;
	int32_t  wdirlen2 = 0;
	int32_t numMirrors = -1;

	int32_t num_nospider = 0;
	int32_t num_noquery  = 0;

	char tmp[256];

	for ( ; *p ; p++ , line++ ) {
		if ( is_wspace_a (*p) ) continue;
		// skip comments
		if ( *p == '#' ) { while ( *p && *p != '\n' ) p++; continue; }

		// does the line say "port-offset: xxxx" ?
		if ( ! strncmp(p,"index-splits:",13) ) {
			p += 13;
			// skip spaces after the colon
			while (  is_wspace_a(*p) ) p++;			
			indexSplits = atol(p);
			while ( *p && *p != '\n' ) p++; 
			continue; 
		}

		if ( ! strncmp(p,"num-mirrors:",12) ) {
			p += 12;
			// skip spaces after the colon
			while (  is_wspace_a(*p) ) p++;			
			numMirrors = atol(p);
			while ( *p && *p != '\n' ) p++; 
			continue; 
		}

		// does the line say "working-dir: xxxx" ?
		if ( ! strncmp(p,"working-dir:",12) ) {
			p += 12;
			// skip spaces after the colon
			while (  is_wspace_a(*p) ) p++;			
			wdir2 = p;
			// skip until not space
			while ( *p && ! is_wspace_a(*p) ) p++;
			// set length
			wdirlen2 = p - wdir2;
			// mark the end
			char *end = p;
			while ( *p && *p != '\n' ) p++; 
			// null term it
			*end = '\0';
			continue; 
		}

		// skip any spaces at start of line
		while (   is_wspace_a(*p) ) p++;

		// get host in order
		Host *h = &m_hosts[i];

		// clear it
		memset ( h , 0 , sizeof(Host) );

		// . see what type of host this is
		// . proxies are not given numbers as yet in the hosts.conf
		//   so number them in the order in which they come
		if ( is_digit(*p) ) {
			h->m_type = HT_GRUNT;
			h->m_hostId = atoi(p);
		}
		else if ( strncasecmp(p,"spare",5)==0 ) {
			h->m_type = HT_SPARE;
			h->m_hostId = -1;
		}
		else if ( strncasecmp(p,"qcproxy",7)==0 ) {
			h->m_type = HT_QCPROXY;
			h->m_hostId = proxyNum++;
		}
		else if ( strncasecmp(p,"scproxy",7)==0 ) {
			h->m_type = HT_SCPROXY;
			h->m_hostId = proxyNum++;
		}
		else if ( strncasecmp(p,"proxy",5)==0 ) {
			h->m_type = HT_PROXY;
			h->m_hostId = proxyNum++;
		}
		// ignore old version "port-offset:"
		else if ( strncasecmp(p,"port-offset:",12)==0 ) {
			while ( *p && *p != '\n' ) p++;
			continue;
		}
		else {
			logf(LOG_INFO,"hosts: hosts.conf bad line: %s",p);
			g_errno = EBADENGINEER;
			return false;
		}

		char *wdir;
		int32_t  wdirlen;

		// reset this
		h->m_retired = false;

		// skip numeric hostid or "proxy" keyword
		while ( ! is_wspace_a(*p) ) p++;

		// skip spaces after hostid/port/spare keyword
		while ( is_wspace_a(*p) ) p++;

		// now the four ports
		int32_t port1 = atol(p);
		// skip digits
		for ( ; is_digit(*p) ; p++ );
		// skip spaces after it
		while ( is_wspace_a(*p) ) p++;

		int32_t port2 = atol(p);
		// skip digits
		for ( ; is_digit(*p) ; p++ );
		// skip spaces after it
		while ( is_wspace_a(*p) ) p++;

		int32_t port3 = atol(p);
		// skip digits
		for ( ; is_digit(*p) ; p++ );
		// skip spaces after it
		while ( is_wspace_a(*p) ) p++;

		int32_t port4 = atol(p);
		// skip digits
		for ( ; is_digit(*p) ; p++ );
		// skip spaces after it
		while ( is_wspace_a(*p) ) p++;

		// set our ports
		h->m_dnsClientPort = port1; // 6000
		h->m_httpsPort     = port2; // 7000
		h->m_httpPort      = port3; // 8000
		h->m_port          = port4; // 9000

		// then hostname
		char *host = p;

		// skip hostname (can be an ip now)
		while ( *p && (*p=='.'||is_alnum_a(*p)) ) p++;
		// get length
		int32_t hlen = p - host;
		// limit
		if ( hlen > 15 ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "admin: hostname too long in hosts.conf");
			return false;
		}
		// copy it
		gbmemcpy ( h->m_hostname , host , hlen );
		// null term it
		h->m_hostname[hlen] = '\0';
		// need this for hashing
		hashinit();
		// if hostname is an ip that's ok i guess
		int32_t ip = atoip ( h->m_hostname );
		// if not an ip, look it up
		if ( ! ip ) {
			// get key
			key96_t k = hash96 ( host , hlen );
			// get eth0 ip of hostname in /etc/hosts
			g_dns.isInFile ( k , &ip );
		}
		// still bad?
		if ( ! ip ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "admin: no ip for hostname \"%s\" in "
			    "hosts.conf in /etc/hosts",
			    h->m_hostname);
			return false;
		}
		// store the ip
		h->m_ip = ip;
		
		// skip spaces or until \n
		for ( ; *p == ' ' ; p++ );
		// must be a 2nd hostname
		char *hostname2 = NULL;
		int32_t hlen2 = 0;
		if ( *p != '\n' ) {
			hostname2 = p;
			// find end of it
			for ( ; *p=='.' || 
				      is_digit(*p) || 
				      is_alnum_a(*p) ; p++ );
			hlen2 = p - hostname2;
		}

		int32_t ip2 = 0;
		// was it "retired"?
		if ( hostname2 && strncasecmp(hostname2,"retired",7) == 0 ) {
			h->m_retired = true;
			hostname2 = NULL;
			//goto retired;
		}

		// limit
		if ( hlen2 > 15 ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "admin: hostname too long in hosts.conf");
			return false;
		}
		// a direct ip address?
		if ( hostname2 ) {
			gbmemcpy ( h->m_hostname2,hostname2,hlen2);
			h->m_hostname2[hlen2] = '\0';
			ip2 = atoip ( h->m_hostname2 );
		}
		if ( ! ip2 && hostname2 ) {
			// set this ip
			//int32_t nextip;
			// now that must have the eth1 ip in /etc/hosts
			key96_t k = hash96 ( h->m_hostname2 , hlen2 );
			// get eth1 ip of hostname in /etc/hosts
			if ( ! g_dns.isInFile ( k , &ip2 ) ) {
				char ipbuf[16];
				log(LOG_WARN, "admin: secondary host %s in hosts.conf "
				    "not in /etc/hosts. Using secondary "
				    "ethernet (eth1) ip "
				    "of %s",hostname2,iptoa(ip,ipbuf));
			}
		}

		// if none, use initial ip as shotgun as well
		if ( ! ip2 ) ip2 = ip;
		// store the ip, the eth1 ip
		h->m_ipShotgun = ip2; // nextip;

		// . now p should point to first char after hostname
		// . skip spaces and tabs
		while ( *p && (*p==' '|| *p=='\t') )p++;

		// is "RETIRED" after hostname?
		if ( strncasecmp(p,"retired",7) == 0 )
			h->m_retired = true;
		
		// for qcproxies, the next thing is always an
		// ip:port of another proxy that we forward the
		// queries to.
		if ( h->m_type & HT_QCPROXY ) {
			char *s = p;
			for ( ; *s && *s!=':' ; s++ );
			int32_t ip = 0;
			if ( *s == ':' ) ip = atoip(p,s-p);
			int32_t port = 0;
			if ( *s ) port = atol(s+1);
			// sanity
			if ( ip == 0 || port == 0 ) {
				g_errno = EBADENGINEER;
				log(LOG_WARN, "admin: bad qcproxy line. must have ip:port after hostname.");
				return false;
			}
			h->m_forwardIp   = ip;
			h->m_forwardPort = port;
			// skip that to port offset now
			for ( ; *p && *p!=' ' && *p !='\t' ; p++);
			// then skip spaces
			for ( ; *p && (*p==' '|| *p=='\t') ; p++ );
		}

		// i guess proxy and spares don't count
		if ( h->m_type != HT_GRUNT ) h->m_shardNum = 0;
		
		// this is the same
		wdir = wdir2;
		wdirlen = wdirlen2; // strlen ( wdir2 );
		// check for working dir override
		if ( *p == '/' ) {
			wdir = p;
			while ( *p && ! isspace(*p) ) p++;
			wdirlen = p - wdir;
		}
		
		if ( ! wdir ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "admin: need working-dir for host in hosts.conf line %" PRId32,line);
			return false;
		}

		// skip spaces or until \n
		for ( ; *p == ' ' ; p++ );
		// then skip spaces
		for ( ; *p && (*p==' '|| *p=='\t') ; p++ );

		char *mdir = NULL;
		int32_t mdirlen = 0;

		// check for merge dir override
		if ( *p == '/' ) {
			mdir = p;
			while ( *p && ! isspace(*p) ) p++;
			mdirlen = p - mdir;
		}


		// skip spaces or until \n
		for ( ; *p == ' ' ; p++ );
		// then skip spaces
		for ( ; *p && (*p==' '|| *p=='\t') ; p++ );

		char *ldir = NULL;
		int32_t ldirlen = 0;

		// check for merge lock dir override
		if ( *p == '/' ) {
			ldir = p;
			while ( *p && ! isspace(*p) ) p++;
			ldirlen = p - ldir;
		}


		h->m_queryEnabled = true;
		h->m_spiderEnabled = true;

		// check for something after the working dir
		h->m_note[0] = '\0';
		if ( *p != '\n' ) {
			// save the note
			char *n = p;
			while ( *n && *n != '\n' && n < pend ) n++;

			int32_t noteSize = n - p;
			if ( noteSize > 127 ) noteSize = 127;
			gbmemcpy(h->m_note, p, noteSize);
			*p++ = '\0'; // NULL terminate for atoip

			if(strstr(h->m_note, "noquery")) {
				h->m_queryEnabled = false;
				num_noquery++;
			}
			if(strstr(h->m_note, "nospider")) {
				h->m_spiderEnabled = false;
				num_nospider++;
			}
		} else {
			*p = '\0';
		}

		// get max group number
		if ( h->m_type == HT_GRUNT )
			numGrunts++;

		// skip line now
		while ( *p && *p != '\n' )
			p++;

		// ensure they're in proper order without gaps
		if ( h->m_type==HT_GRUNT && h->m_hostId != i ) {
		     g_errno = EBADHOSTID; 
		     log(LOG_WARN, "conf: Unordered hostId of %" PRId32", should be %" PRId32" in %s line %" PRId32".",
		         h->m_hostId,i,filename,line);
			return false;
		}

		// and working dir
		if ( wdirlen > 255 ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "conf: Host working dir too long in %s line %" PRId32".", filename, line);
			return false;
		}
		if ( wdirlen <= 0 ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "conf: No working dir supplied in %s line %" PRId32".", filename, line);
			return false;
		}
		// make sure it is legit
		if ( wdir[0] != '/' ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "conf: working dir must start with / in %s line %" PRId32, filename, line);
			return false;
		}

		// take off slash if there
		if ( wdir[wdirlen-1]=='/' ) {
			wdir[--wdirlen]='\0';
		}

		// get real path (no symlinks symbolic links)
		// only if on same host, which we determine based on the IP-address.
		if ( ip_distance(h->m_ip)==ip_distance_ourselves ) {
			int32_t tlen = readlink ( wdir , tmp , 250 );
			// if we got the actual path, copy that over
			if ( tlen != -1 ) {
				// wdir currently references into the 
				// hosts.conf buf so don't store the expanded
				// directory into there
				wdir = tmp;
				wdirlen = tlen;
			}
		}

		// add slash if none there
		if ( wdir[wdirlen-1] !='/' ) wdir[wdirlen++] = '/';
			
		// don't breach Host::m_dir[256] buffer
		if ( wdirlen >= 256 ) {
			log(LOG_WARN, "conf: working dir %s is too long, >= 256 chars.", wdir);
			return false;
		}

		// copy it over
		memcpy(m_hosts[i].m_dir, wdir, wdirlen);
		m_hosts[i].m_dir[wdirlen] = '\0';
		memcpy(m_hosts[i].m_mergeDir, mdir, mdirlen);
		m_hosts[i].m_mergeDir[mdirlen] = '\0';
		memcpy(m_hosts[i].m_mergeLockDir, ldir, ldirlen);
		m_hosts[i].m_mergeLockDir[ldirlen] = '\0';

		m_hosts[i].m_lastResponseReceiveTimestamp = 0;
		m_hosts[i].m_lastRequestSendTimestamp = 0;

		m_hosts[i].m_runtimeInformation.m_dailyMergeCollnum = -1;

		// point to next one
		i++;
	}

	m_numTotalHosts = i;

	// BR 20160313: Sanity check. I doubt the striping functionality works with an odd mix 
	// of noquery and nospider hosts. Make sure the number of each kind is the same for now.
	if (num_nospider && num_noquery && num_nospider != num_noquery) {
		g_errno = EBADENGINEER;
		log(LOG_ERROR,"Number of nospider and noquery hosts must match in hosts.conf");
		return false;
	}

	// # of mirrors is zero if no mirrors,
	// if it is 1 then each host has ONE MIRROR host
	if ( numMirrors == 0 )
		indexSplits = numGrunts;
	if ( numMirrors > 0 )
		indexSplits = numGrunts / (numMirrors+1);

	if ( indexSplits == 0 ) {
		g_errno = EBADENGINEER;
		log("admin: need num-mirrors: xxx or "
		    "index-splits: xxx directive "
		    "in hosts.conf");
		return false;
	}

	numMirrors = (numGrunts / indexSplits) - 1 ;

	if ( numMirrors < 0 ) {
		g_errno = EBADENGINEER;
		log("admin: need num-mirrors: xxx or "
		    "index-splits: xxx directive "
		    "in hosts.conf (2)");
		return false;
	}

	m_indexSplits = indexSplits;

	m_numShards = numGrunts / (numMirrors+1);

	//
	// set Host::m_shardNum
	//
	for ( int32_t i = 0 ; i < numGrunts ; i++ ) {
		Host *h = &m_hosts[i];
		h->m_shardNum = i % indexSplits;
	}

	// assign spare hosts
	if ( m_numSpareHosts > MAX_SPARES ) {
		log ( LOG_WARN, "conf: Number of spares (%" PRId32") exceeds max of %i, "
		      "truncating.", m_numSpareHosts, MAX_SPARES );
		m_numSpareHosts = MAX_SPARES;
	}
	for ( i = 0; i < m_numSpareHosts; i++ ) {
		m_spareHosts[i] = &m_hosts[m_numHosts + i];
	}
	
	// assign proxy hosts
	if ( m_numProxyHosts > MAX_PROXIES ) {
		log ( LOG_WARN, "conf: Number of proxies (%" PRId32") exceeds max of %i, "
		      "truncating.", m_numProxyHosts, MAX_PROXIES );
		g_process.shutdownAbort(true);
		m_numProxyHosts = MAX_PROXIES;
	}
	for ( i = 0; i < m_numProxyHosts; i++ ) {
		m_proxyHosts[i] = &m_hosts[m_numHosts + m_numSpareHosts + i];
		m_proxyHosts[i]->m_isProxy = true;
		// sanity
		if ( m_proxyHosts[i]->m_type == 0  ) { g_process.shutdownAbort(true); }
	}

	// log discovered hosts
	log ( LOG_INFO, "conf: Discovered %" PRId32" hosts and %" PRId32" spares and "
	      "%" PRId32" proxies.",m_numHosts, m_numSpareHosts, m_numProxyHosts );

	// if we have m_numShards we must have 
	int32_t hostsPerShard  = m_numHosts / m_numShards;
	// must be exact fit
	if ( hostsPerShard * m_numShards != m_numHosts ) {
		g_errno = EBADENGINEER;
		log(LOG_WARN, "conf: Bad number of hosts for %" PRId32" shards in hosts.conf.",m_numShards);
		return false;
	}
	// count number of hosts in each shard
	for ( i = 0 ; i < m_numShards ; i++ ) {
		int32_t count = 0;
		for ( int32_t j = 0 ; j < m_numHosts ; j++ )
			if ( m_hosts[j].m_shardNum == (uint32_t)i ) 
				count++;
		if ( count != hostsPerShard ) {
			g_errno = EBADENGINEER;
			log(LOG_WARN, "conf: Number of hosts in each shard in %s is not equal.",filename);
			return false;
		}
	}

	// now sort hosts by shard # then HOST id (both ascending order)
	gbsort ( m_hosts , m_numHosts , sizeof(Host), cmp );

	// . set m_shards array
	// . m_shards[i] is the first host in shardId "i"
	// . any other hosts w/ same shardId immediately follow it
	// . loop through each shard
	for ( i = 0 ; i < m_numShards ; i++ ) {
		int32_t j;
		for ( j = 0 ; j < m_numHosts ; j++ ) 
			if ( m_hosts[j].m_hostId == i ) break;
		// this points to list of all hosts in shard #j since
		// we sorted m_hosts by shardId
		m_shards[i] = &m_hosts[j];
	}
	// . set m_hostPtrs now so Hostdb::getHost() works
	// . the hosts are sorted by shard first so we must be careful
	for ( i = 0 ; i < m_numHosts ; i++ ) {
		int32_t j = m_hosts[i].m_hostId;
		m_hostPtrs[j] = &m_hosts[i];
	}
	// reset this count to 1, 1 counts for ourselves
	if(proxyHost) {
		m_numProxyAlive = 1;
	} else {
		m_numHostsAlive = 1;
	}
	// reset ping/stdDev times
	for ( int32_t i = 0 ; i < m_numHosts ; i++ ) {
		m_hosts[i].m_preferEth      = 0;
	}

	// get IPs of this server. last entry is 0.
	int32_t *localIps = getLocalIps();
	if ( ! localIps ) {
		log(LOG_WARN, "conf: Failed to get local IP address. Exiting.");
		return false;
	}

	// if no cwd, then probably calling 'gb inject foo.warc <hosts.conf>'
	if ( ! cwd ) {
		log("hosts: missing cwd");
		return true;
	}

	if (initMyHost) {
		// now get host based on cwd and ip
		Host *host = getHost2(cwd, localIps);

		// now set m_myIp, m_myPort, m_myPort2 and m_myMachineNum
		if (proxyHost)
			host = getProxy2(cwd, localIps); //hostId );
		if (!host) {
			log(LOG_WARN, "conf: Could not find host with path %s and local ip in %s", cwd, filename);
			return false;
		}
		m_myIp = host->m_ip;    // internal IP
		m_myPort = host->m_port;  // low priority udp port
		m_myHost = host;

		//we are alive, obviously
		m_myHost->m_isAlive = true;

		//we are the lowest repair mode host until we know better
		m_minRepairModeHost = m_myHost;

		// THIS hostId
		m_myHostId = m_myHost->m_hostId;
	}

	//in single-instance setups the hosts.conf CRC is always OK
	if(m_numHosts==1) {
		m_hostsConfInDisagreement = false;
		m_hostsConfInAgreement = true;
	}

	// set hosts per shard (mirror group)
	m_numHostsPerShard = m_numHosts / m_numShards;

	// set m_stripe (aka m_twinNum) for each host
	for ( int32_t i = 0 ; i < m_numHosts ; i++ ) {
		// get this host
		Host *h = &m_hosts[i];
		// get his shard, array of hosts
		Host *shard = getShard ( h->m_shardNum );
		// how many hosts in the shard?
		int32_t ng = getNumHostsPerShard();
		// hosts in shard should be sorted by hostid i think, anyway,
		// they *need* to be. see above, hosts are in order in the
		// m_hosts[] array by shard then by hostId, so we should be
		// good to go.
		for ( int32_t j = 0 ; j < ng ; j++ ) {
			if ( &shard[j] != h ) continue;
			h->m_stripe = j;
			break;
		}
	}

	// BR 20160316: Make sure noquery hosts are not used when dividing
	// docIds for querying (Msg39)
	m_numStripeHostsPerShard = m_numHostsPerShard;
	if( m_numStripeHostsPerShard > 1 )
	{
		// Make sure we don't include noquery hosts
		m_numStripeHostsPerShard = (m_numHosts - num_noquery) / m_numShards;
	}

	if (initMyHost) {
		// get THIS host
		Host *h = getHost(m_myHostId);
		if (proxyHost)
			h = getProxy(m_myHostId);
		if (!h) {
			log(LOG_WARN, "conf: HostId %" PRId32" not found in %s.", m_myHostId, filename);
			return false;
		}
		// set m_dir to THIS host's working dir
		strncpy(m_dir, h->m_dir, sizeof(m_dir));
		m_dir[sizeof(m_dir) - 1] = '\0';

		// likewise, set m_htmlDir to this host's html dir
		snprintf(m_httpRootDir, sizeof(m_httpRootDir), "%shtml/", m_dir);
		m_httpRootDir[sizeof(m_httpRootDir) - 1] = '\0';

		snprintf(m_logFilename, sizeof(m_logFilename), "%slog%03" PRId32, m_dir, m_myHostId);
		m_logFilename[sizeof(m_logFilename) - 1] = '\0';
	}

	if ( ! g_conf.m_runAsDaemon &&
	     ! g_conf.m_logToFile )
		sprintf(m_logFilename,"/dev/stderr");


	int32_t gcount = 0;
	for ( int32_t i = 0 ; i < MAX_KSLOTS && m_numHosts ; i++ ) {
		// now just map to the shard # not the groupId... simpler...
		m_map[i] = gcount % m_numShards;
		// inc it
		gcount++;
	}

	if (initMyHost) {
		// set our group
		m_myShard = getShard(m_myHost->m_shardNum);
	}

	//calculate CRC once and ofr all
	getCRC();

	// has the hosts
	return hashHosts(initMyHost);
}


bool Hostdb::hashHosts(bool initMyHost) {
	// this also holds g_hosts2 as well as g_hosts so we cannot preallocate
	for ( int32_t i = 0 ; i < m_numHosts ; i++ ) {
		Host *h = &m_hosts[i];
		int32_t ip;
		ip = h->m_ip;
		if ( ! hashHost ( 1,h,ip, h->m_port     )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpPort )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpsPort)) return false;
		// . only hash this if not already in there
		// . just used to see if ip is in the network (local)
		if ( ! hashHost ( 1 , h , ip, 0 )) return false;

		// only hash shotgun ip if different
		if ( h->m_ip == h->m_ipShotgun ) continue;

		ip = h->m_ipShotgun;
		if ( ! hashHost ( 1,h,ip, h->m_port     )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpPort )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpsPort)) return false;
		// . only hash this if not already in there
		// . just used to see if ip is in the network (local)
		if ( ! hashHost ( 1 , h , ip, 0 )) return false;
	}

	// . hash loopback ip to point to us
	// . udpserver needs this?
	// . only do this if they did not already specify a 127.0.0.1 in
	//   the hosts.conf i guess
	if (initMyHost) {
		int32_t lbip = atoip("127.0.0.1");
		Host *hxx = getUdpHost(lbip, m_myHost->m_port);
		// only do this if not explicitly assigned to 127.0.0.1 in hosts.conf
		if (!hxx && (int32_t)m_myHost->m_ip != lbip) {
			int32_t loopbackIP = atoip("127.0.0.1", 9);
			if (!hashHost(1, m_myHost, loopbackIP, m_myHost->m_port))
				return false;
		}
	}

	// and the proxies as well
	for ( int32_t i = 0 ; i < m_numProxyHosts ; i++ ) {
		Host *h = getProxy(i);
		int32_t ip;
		ip = h->m_ip;
		if ( ! hashHost ( 1,h,ip, h->m_port     )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpPort )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpsPort)) return false;
		// . only hash this if not already in there
		// . just used to see if ip is in the network (local)
		if ( ! hashHost ( 1 , h , ip, 0 )) return false;

		// only hash shotgun ip if different
		if ( h->m_ip == h->m_ipShotgun ) continue;

		ip = h->m_ipShotgun;
		if ( ! hashHost ( 1,h,ip, h->m_port     )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpPort )) return false;
		if ( ! hashHost ( 0,h,ip, h->m_httpsPort)) return false;
		// . only hash this if not already in there
		// . just used to see if ip is in the network (local)
		if ( ! hashHost ( 1 , h , ip, 0 )) return false;
	}

	// verify g_hostTableUdp
	for ( int32_t i = 0 ; i < m_numHosts ; i++ ) {
		// get the ith host
		Host *h = &m_hosts[i];
		Host *h2 = getUdpHost ( h->m_ip , h->m_port );
		if ( h != h2 ) {
			log( LOG_WARN, "db: Host lookup failed for hostId %i.", h->m_hostId );
			return false;
		}

		h2 = getUdpHost ( h->m_ipShotgun , h->m_port );
		if ( h != h2 ) {
			log( LOG_WARN, "db: Host lookup2 failed for hostId %" PRId32".", h->m_hostId );
			return false;
		}
		if ( ! isIpInNetwork ( h->m_ip ) ) {
			log( LOG_WARN, "db: Host lookup5 failed for hostId %" PRId32".", h->m_hostId );
			return false;
		}
	}

	// verify g_hostTableTcp
	for ( int32_t i = 0 ; i < m_numHosts ; i++ ) {
		// get the ith host
		Host *h = &m_hosts[i];
		Host *h2 ;
		h2 = getTcpHost ( h->m_ip , h->m_httpPort );
		if ( h != h2 ) {
			char ipbuf[16];
			log( LOG_WARN, "db: Host lookup3 failed for hostId %" PRId32". ip=%s port=%hu",
			     h->m_hostId, iptoa(h->m_ip,ipbuf), h->m_httpPort );
			return false;
		}
		h2 = getTcpHost ( h->m_ip , h->m_httpsPort );
		if ( h != h2 ) {
			log( LOG_WARN, "db: Host lookup4 failed for hostId %" PRId32".", h->m_hostId );
			return false;
		}
	}

	return true;
}

bool Hostdb::hashHost (	bool udp , Host *h , uint32_t ip , uint16_t port ) {
	Host *hh = NULL;
	if ( udp ) hh = getUdpHost ( ip , port );

	if ( hh && port ) { 
		char ipbuf[16];
		log(LOG_WARN, "db: Must hash hosts.conf first, then hosts2.conf.");
		log(LOG_WARN, "db: or there is a repeated ip/port in hosts.conf.");
		log(LOG_WARN, "db: repeated host ip=%s port=%" PRId32" "
		    "name=%s",iptoa(ip,ipbuf),(int32_t)port,h->m_hostname);
		return false;//g_process.shutdownAbort(true);
	}

	HashTableX *t;
	if ( udp ) t = &g_hostTableUdp;
	else       t = &g_hostTableTcp;
	// initialize the table?
	if ( !t->isInitialized() ) {
		t->set ( 8 , sizeof(char *),16,NULL,0,false,"hostbl");
	}
	// get his key
	uint64_t key = 0;
	// masking the low bits of the ip is not good because it is
	// the same for every host! so reverse the key to get good hash
	char *dst = (char *)&key;
	char *src = (char *)&ip;
	dst[0] = src[3];
	dst[1] = src[2];
	dst[2] = src[1];
	dst[3] = src[0];
	// port too
	char *src2 = (char *)&port;
	dst[4] = src2[1];
	dst[5] = src2[0];
	// look it up
	int32_t slot = t->getSlot ( &key );
	// see if there is a collision
	Host *old = NULL;
	if ( slot >= 0 ) {
		// ports of 0 mean we are just adding an ip, and we can
		// have multiple hosts on the same ip. this call was just
		// to make isIpInNetwork() function work.
		if ( port == 0 ) return true;
		old = *(Host **)t->getValueFromSlot(slot);
		log(LOG_WARN, "db: Got collision between hostId %" PRId32" and %" PRId32"(proxy=%" PRId32"). "
			"Both have same ip/port. Does hosts.conf match hosts2.conf?",
		    old->m_hostId,h->m_hostId,(int32_t)h->m_isProxy);
		return false;
	}
	// add the new key with a ptr to host using m_port
	return t->addKey ( &key , &h ); // (uint32_t)h ) ;
}

int32_t Hostdb::getHostId ( uint32_t ip , uint16_t port ) {
	Host *h = getUdpHost ( ip , port );
	if ( ! h ) return -1;
	return h->m_hostId;
}

Host *Hostdb::getHostByIp ( uint32_t ip ) {
	return getHostFromTable ( true , ip , 0 );	
}

// . get Host entry from ip/port
// . port defaults to 0 for no port
Host *Hostdb::getUdpHost ( uint32_t ip , uint16_t port ) {
	return getHostFromTable ( true , ip , port );
}	

// . get Host entry from ip/port
// . port defaults to 0 for no port
Host *Hostdb::getTcpHost ( uint32_t ip , uint16_t port ) {
	return getHostFromTable ( false , ip , port );
}	

bool Hostdb::isIpInNetwork ( uint32_t ip ) {
	// use port of 0
	if ( getHostByIp ( ip ) ) return true;
	// not found
	return false;
}

// . get Host entry from ip/port
// . this works on proxy hosts as well!
// . use a port of 0 if we should disregard port
Host *Hostdb::getHostFromTable ( bool udp , uint32_t ip , uint16_t port ) {
	HashTableX *t;
	if ( udp ) t = &g_hostTableUdp;
	else       t = &g_hostTableTcp;
	// reset key
	uint64_t key = 0;
	// masking the low bits of the ip is not good because it is
	// the same for every host! so reverse the key to get good hash
	char *dst = (char *)&key;
	char *src = (char *)&ip;
	dst[0] = src[3];
	dst[1] = src[2];
	dst[2] = src[1];
	dst[3] = src[0];
	// port too
	char *src2 = (char *)&port;
	dst[4] = src2[1];
	dst[5] = src2[0];
	// look it up
	int32_t slot = t->getSlot ( &key );
	// return NULL if not found
	if ( slot < 0 ) return NULL;
	return *(Host **) t->getValueFromSlot ( slot );
}

// . this is used by gbsort() above
// . sorts Hosts by their shard
static int cmp (const void *v1, const void *v2) {
	Host *h1 = (Host *)v1;
	Host *h2 = (Host *)v2;
	// return if shards differ
	if ( h1->m_shardNum < h2->m_shardNum ) return -1; 
	if ( h1->m_shardNum > h2->m_shardNum ) return  1;
	// otherwise sort by hostId
	return h1->m_hostId - h2->m_hostId;
}

#include "Stats.h"

bool Hostdb::isShardDead(int32_t shardNum) const {
	// how many seconds since our main process was started?
	// i guess all nodes initially appear dead, so
	// compensate for that.
	long long now = gettimeofdayInMilliseconds();
	long elapsed = (now - g_stats.m_startTime) ;/// 1000;
	if ( elapsed < 60*1000 ) return false; // try 60 secs now

	const Host *shard = getShard(shardNum);
	//Host *live = NULL;
	for ( int32_t i = 0 ; i < m_numHostsPerShard ; i++ ) {
		if(!isDead(shard[i].m_hostId))
			return false;
	}
	return true;
}


int32_t Hostdb::getHostIdWithSpideringEnabled ( uint32_t shardNum, bool answerRequired ) {
	Host *hosts = getShard ( shardNum);
	int32_t numHosts = getNumHostsPerShard();

	int32_t hostNum = 0;
	int32_t numTried = 0;
	while( !hosts [ hostNum ].m_spiderEnabled && numTried < numHosts ) {
		hostNum = (hostNum+1) % numHosts;
		numTried++;
	}
	if( !hosts [ hostNum ].m_spiderEnabled) {
		if( answerRequired ) {
			log("build: cannot spider when entire shard has nospider enabled");
			g_process.shutdownAbort(true);
		}
		else {
			return -1;
		}
	}
	return hosts[hostNum].m_hostId;
}


Host *Hostdb::getHostWithSpideringEnabled ( uint32_t shardNum ) {
	Host *hosts = getShard ( shardNum);
	int32_t numHosts = getNumHostsPerShard();

	int32_t hostNum = 0;
	int32_t numTried = 0;
	while( !hosts [ hostNum ].m_spiderEnabled && numTried < numHosts ) {
		hostNum = (hostNum+1) % numHosts;
		numTried++;
	}
	if( !hosts [ hostNum ].m_spiderEnabled) {
		log("build: cannot spider when entire shard has nospider enabled");
		g_process.shutdownAbort(true);
	}
	return &hosts [ hostNum ];
}


bool Hostdb::mayWeSendRequestToHost(const Host *host, msg_type_t /*msgType*/) {
	if(!getMyHost()->m_spiderEnabled && host->m_spiderEnabled && !g_conf.m_queryHostToSpiderHostFallbackAllowed)
		return false;
	if(!getMyHost()->m_queryEnabled && host->m_queryEnabled && !g_conf.m_spiderHostToQueryHostFallbackAllowed)
		return false;
	return true;
}

// if niceness 0 can't pick noquery host/ must pick spider host.
// if niceness 1 can't pick nospider host/ must pick query host.
// Used to select based on PingInfo::m_udpSlotsInUseIncoming but that information is not exchanged often enough to
// be even remotely accurate with any realistic number of shards.
Host *Hostdb::getLeastLoadedInShard ( uint32_t shardNum , char niceness ) {
	int32_t minOutstandingRequestsIndex = -1;
	Host *shard = getShard ( shardNum );
	Host *bestDead = NULL;
	ScopedLock sl(m_mtxPinginfo);
	for(int32_t i = 0; i < m_numHostsPerShard; i++) {
		Host *hh = &shard[i];
		// don't pick a 'no spider' host if niceness is 1
		if ( niceness >  0 && ! hh->m_spiderEnabled ) continue;
		// don't pick a 'no query' host if niceness is 0
		if ( niceness == 0 && ! hh->m_queryEnabled  ) continue;
		if ( ! bestDead ) bestDead = hh;
		if(isDead_unlocked(hh)) continue;

		minOutstandingRequestsIndex = i;
	}
	// we should never return a nospider/noquery host depending on
	// the niceness, so return bestDead
	if(minOutstandingRequestsIndex == -1) return bestDead;//shard;

	return &shard[minOutstandingRequestsIndex];
}

// if all are dead just return host #0
Host *Hostdb::getFirstAliveHost() {
	for ( int32_t i = 0 ; i < m_numHosts ; i++ )
		// if host #i is alive, return her
		if ( ! isDead ( i ) ) return getHost(i);
	// if all are dead just return host #0
	return getHost(0);
}

bool Hostdb::hasDeadHost() const {
	for ( int32_t i = 0 ; i < m_numHosts ; i++ )
		if ( isDead ( i ) ) return true;
	return false;
}

bool Hostdb::hasDeadHostCached() const {
	time_t now = time(NULL);
	static std::atomic<bool> s_hasDeadHost(hasDeadHost());
	static std::atomic<time_t> s_updatedTime(now);

	if ((now - s_updatedTime) >= g_conf.m_spiderDeadHostCheckInterval) {
		s_updatedTime = now;
		s_hasDeadHost = hasDeadHost();
	}

	return s_hasDeadHost;
}

int Hostdb::getNumHostsDead() const {
	int count=0;
	for(int32_t i = 0; i < m_numHosts; i++)
		if(isDead(i))
			count++;
	return count;
}

bool Hostdb::isDead(int32_t hostId) const {
	const Host *h = getHost(hostId);
	return isDead ( h );
}

bool Hostdb::isDead(const Host *h) const {
	ScopedLock sl(m_mtxPinginfo);
	return isDead_unlocked(h);
}

bool Hostdb::isDead_unlocked(const Host *h) const {
	if(h->m_retired)
		return true; // retired means "don't use it", so it is essentially dead
	if(m_myHost == h)
		return false; //we are not dead
	return !h->m_isAlive;
}

int64_t Hostdb::getNumGlobalRecs ( ) {
	int64_t n = 0;
	for ( int32_t i = 0 ; i < m_numHosts ; i++ )
		n += getHost ( i )->m_runtimeInformation.m_totalDocsIndexed;
	return n / m_numHostsPerShard;
}

bool Hostdb::setNote ( int32_t hostId, const char *note, int32_t noteLen ) {
	// replace the note on the host
	if ( noteLen > 125 ) noteLen = 125;
	Host *h = getHost ( hostId );
	if ( !h ) return true;
	gbmemcpy(h->m_note, note, noteLen);
	h->m_note[noteLen] = '\0';
	// write this hosts conf out
	return saveHostsConf();
}

bool Hostdb::setSpareNote ( int32_t spareId, const char *note, int32_t noteLen ) {
	// replace the note on the host
	if ( noteLen > 125 ) noteLen = 125;
	Host *h = getSpare ( spareId );
	if ( !h ) return true;
	gbmemcpy(h->m_note, note, noteLen);
	h->m_note[noteLen] = '\0';
	// write this hosts conf out
	return saveHostsConf();
}

bool Hostdb::replaceHost ( int32_t origHostId, int32_t spareHostId ) {
	Host *oldHost = getHost(origHostId);
	Host *spareHost = getSpare(spareHostId);
	if ( !oldHost || !spareHost ) {
		log(LOG_WARN, "init: Bad Host or Spare given. Aborting.");
		return false;
	}
	// host must be dead
	if ( !isDead(oldHost) ) {
		log(LOG_WARN, "init: Cannot replace live host. Aborting.");
		return false;
	}


	Host tmp;
	gbmemcpy ( &tmp , oldHost , sizeof(Host) );
	gbmemcpy ( oldHost , spareHost , sizeof(Host) );
	gbmemcpy ( spareHost , &tmp , sizeof(Host) );

	// however, these values need to change
	oldHost->m_hostId      = origHostId;
	oldHost->m_shardNum    = spareHost->m_shardNum;
	oldHost->m_stripe      = spareHost->m_stripe;
	oldHost->m_isProxy     = spareHost->m_isProxy;
	oldHost->m_type        = HT_SPARE;

	// and the new spare gets a new hostid too
	spareHost->m_hostId = spareHostId;

	// reset these stats
	oldHost->m_runtimeInformation.m_totalDocsIndexed         = 0;
	oldHost->m_errorReplies        = 0;
	oldHost->m_dgramsTo            = 0;
	oldHost->m_dgramsFrom          = 0;
	oldHost->m_totalResends        = 0;
	oldHost->m_etryagains          = 0;
	oldHost->m_splitsDone          = 0;
	oldHost->m_splitTimes          = 0;

	// write this hosts conf out
	saveHostsConf();
	//
	// . now we need to replace the ips and ports in the hash tables
	//   just clear the hash tables and rehash
	// 
	g_hostTableUdp.clear();
	g_hostTableTcp.clear();
	// now restock everything
	hashHosts();

	// replace ips in udp server
	g_udpServer.replaceHost ( spareHost, oldHost );
	return true;
}

// use the ip that is not dead, prefer eth0
int32_t Hostdb::getBestIp(const Host *h) {
	//used to examin ping times and chose between normal Ip and "shotgun" ip
	return h->m_ip;
}

// . "h" is from g_hostdb2, the "external" cluster
// . should we send to its primary or shotgun ip?
// . this returns which ip we should send to
int32_t Hostdb::getBestHosts2IP(const Host *h) {
	if(ip_distance(h->m_ip) <= ip_distance(h->m_ipShotgun))
		return h->m_ip;
	else
		return h->m_ipShotgun;
}


void Hostdb::updateAliveHosts(const int32_t alive_hosts_ids[], size_t n) {
	ScopedLock sl(m_mtxPinginfo);
	for(int32_t i=0; i<m_numHosts; i++)
		m_hosts[i].m_isAlive = false;
	m_myHost->m_isAlive = true;
	for(size_t i=0; i<n; i++) {
		int32_t hostid = alive_hosts_ids[i];
		if(hostid>=0 && hostid<m_numHosts)
			m_hostPtrs[hostid]->m_isAlive = true;
	}
	//update m_numHostsAlive
	m_numHostsAlive = 0;
	for(int32_t i=0; i<m_numHosts; i++)
		if(m_hosts[i].m_isAlive)
			m_numHostsAlive++;
}

void Hostdb::updateHostRuntimeInformation(int hostId, const HostRuntimeInformation &hri) {
	if(hostId<0)
		gbshutdownLogicError();
	if(hostId>=m_numHosts)
		return; //out-of-sync hosts.conf ?
	ScopedLock sl(m_mtxPinginfo);
	bool crc_changed = m_hostPtrs[hostId]->m_runtimeInformation.m_hostsConfCRC != hri.m_hostsConfCRC;
	bool repairmode_changed = m_hostPtrs[hostId]->m_runtimeInformation.m_repairMode != hri.m_repairMode;
	m_hostPtrs[hostId]->m_runtimeInformation = hri;
	m_hostPtrs[hostId]->m_runtimeInformation.m_valid = true;
	if(crc_changed) {
		//recalculate m_hostsConfInAgreement and m_hostsConfInDisagreement
		m_hostsConfInDisagreement = false;
		m_hostsConfInAgreement = false;
		// if we haven't received crc from all hosts then we will not set either flag.
		int32_t agreeCount = 0;
		for(int i = 0; i < getNumGrunts(); i++) {
			// skip if not received yet
			if(m_hosts[i].isHostsConfCRCKnown()) {
				if(!m_hosts[i].hasSameHostsConfCRC()) {
					m_hostsConfInDisagreement = true;
					break;
				}
				agreeCount++;
			}
		}

		// if all in agreement, set this flag
		if(agreeCount == getNumGrunts()) {
			m_hostsConfInAgreement = true;
		}
	}
	if(repairmode_changed) {
		//recalculate m_minRepairMode/m_minRepairModeBesides0/m_minRepairModeHost
		const Host *newMinHost = NULL;
		char newMin = -1;
		char newMin0 = -1;
		for(int i=0; i<m_numHosts; i++) {
			if(newMin==-1 || m_hosts[i].m_runtimeInformation.m_repairMode < newMin) {
				newMinHost = &(m_hosts[i]);
				newMin = m_hosts[i].m_runtimeInformation.m_repairMode;
			}
			if(newMin0==-1 || (m_hosts[i].m_runtimeInformation.m_repairMode!=0 && m_hosts[i].m_runtimeInformation.m_repairMode<newMin0))
				newMin0 = m_hosts[i].m_runtimeInformation.m_repairMode;
		}
		m_minRepairMode = newMin;
		m_minRepairModeBesides0 = newMin0;
		m_minRepairModeHost = newMinHost;
	}
}


void Hostdb::setOurFlags() {
	m_myHost->m_runtimeInformation.m_flags = getOurHostFlags();
	m_myHost->m_runtimeInformation.m_valid = true;
}

void Hostdb::setOurTotalDocsIndexed() {
	m_myHost->m_runtimeInformation.m_totalDocsIndexed = g_process.getTotalDocsIndexed();
	
}

// assume to be from posdb here
uint32_t Hostdb::getShardNumByTermId(const void *k) const {
	return m_map [(*(uint16_t *)((char *)k + 16))>>3];
}

// . if false, we don't split index and date lists, other dbs are unaffected
// . this obsolets the g_*.getGroupId() functions
// . this allows us to have any # of groups in a stripe, not just power of 2
// . now we can use 3 stripes of 96 hosts each so spiders will almost never
//   go down
uint32_t Hostdb::getShardNum(rdbid_t rdbId, const void *k) const {
	switch(rdbId) {
		case RDB_POSDB:
		case RDB2_POSDB2:
			if( Posdb::isShardedByTermId ( k ) ) {
				// based on termid NOT docid!!!!!!
				// good for page checksums so we only have to do disk
				// seek on one shard, not all shards.
				// use top 13 bits of key.
				return m_map [(*(uint16_t *)((char *)k + 16))>>3];
			} else {
				uint64_t d = Posdb::getDocId ( k );
				return m_map [ ((d>>14)^(d>>7)) & (MAX_KSLOTS-1) ];
			}

		case RDB_LINKDB:
		case RDB2_LINKDB2:
			// sharded by part of linkee sitehash32
			return m_map [(*(uint16_t *)((char *)k + 26))>>3];

		case RDB_TITLEDB:
		case RDB2_TITLEDB2: {
			uint64_t d = Titledb::getDocId ( (key96_t *)k );
			return m_map [ ((d>>14)^(d>>7)) & (MAX_KSLOTS-1) ];
		}

		case RDB_SPIDERDB_DEPRECATED:
		case RDB2_SPIDERDB2_DEPRECATED:
		case RDB_SPIDERDB_SQLITE:
		case RDB2_SPIDERDB2_SQLITE: {
			int32_t firstIp = Spiderdb::getFirstIp((key128_t *)k);
			// do what Spider.h getGroupId() used to do so we are
			// backwards compatible
			uint32_t h = (uint32_t)hash32h(firstIp,0x123456);
			// use that for getting the group
			return m_map [ h & (MAX_KSLOTS-1)];
		}

		case RDB_CLUSTERDB:
		case RDB2_CLUSTERDB2: {
			uint64_t d = Clusterdb::getDocId ( k );
			return m_map [ ((d>>14)^(d>>7)) & (MAX_KSLOTS-1) ];
		}

		case RDB_TAGDB:
		case RDB2_TAGDB2:
			return m_map [(*(uint16_t *)((char *)k + 10))>>3];

		case RDB_DOLEDB:
			// HACK:!!!!!!  this is a trick!!! it is us!!!
			//return m_myHost->m_groupId;
			return m_myHost->m_shardNum;

		default:
			// core -- must be provided
			g_process.shutdownAbort(true);
	}
}

uint32_t Hostdb::getShardNumFromDocId(int64_t d) const {
	return m_map [ ((d>>14)^(d>>7)) & (MAX_KSLOTS-1) ];
}

Host *Hostdb::getBestSpiderCompressionProxy ( int32_t *key ) {
	static int32_t s_numTotal = 0;
	static int32_t s_numAlive = 0;
	static Host *s_alive[64];
	static Host *s_lastResort = NULL;
	static bool s_aliveValid = false;

	if ( ! s_aliveValid ) {
		// come up to "redo" from below if a host goes dead
	redo:
		s_aliveValid = true;
		for ( int32_t i = 0 ; i < m_numProxyHosts ; i++ ) {
			Host *h = getProxy(i);
			if ( ! (h->m_type & HT_SCPROXY ) ) continue;
			// if all dead use this
			s_lastResort = h;
			// count towards total even if not alive
			s_numTotal++;
			// now must be alive
			if ( isDead(h) ) continue;
			// stop to avoid breach
			if ( s_numAlive >= 64 ) { g_process.shutdownAbort(true); }
			// add it otherwise
			s_alive[s_numAlive++] = h;
		}
	}

	// if no scproxy in hosts.conf return NULL
	if ( s_numTotal == 0 ) return NULL;

	// if none alive, use last resort, a non-null dead host
	if ( s_numAlive == 0 ) return s_lastResort;

	// pick one based on the key
	int32_t ni = hash32((char *)key , 4 ) % s_numAlive;
	// get it
	Host *h = s_alive[ni];
	// if dead, recompute alive[] table and try again!
	if ( isDead(h) ) goto redo;
	// got a live one
	return h;
}

int32_t Hostdb::getCRC ( ) {
	if ( m_crcValid ) return m_crc;
	// hash up all host entries, just the grunts really.
	const int num_grunts = getNumGrunts();
	SafeBuf str;
	for ( int32_t i = 0 ; i < num_grunts ; i++ ) {
		Host *h = &m_hosts[i];
		char ipbuf[16];
		// dns client port not so important
		str.safePrintf("%" PRId32",", i);
		str.safePrintf("%s," , iptoa(h->m_ip,ipbuf));
		str.safePrintf("%s," , iptoa(h->m_ipShotgun,ipbuf));
		str.safePrintf("%" PRId32",", (int32_t)h->m_httpPort);
		str.safePrintf("%" PRId32",", (int32_t)h->m_httpsPort);
		str.safePrintf("%" PRId32",", (int32_t)h->m_port);
		str.pushChar('\n');
	}
	str.nullTerm();

	m_crc = hash32n ( str.getBufStart() );

	// make sure it is legit
	if ( m_crc == 0 ) m_crc = 1;

	m_crcValid = true;

	log(LOG_INFO,"conf: hosts.conf CRC calculated as %d based on %d grunts", m_crc, num_grunts);
	return m_crc;
}


static void printHostsConfPreamble(SafeBuf *sb, int numMirrors, int indexSplits) {
	sb->safePrintf("# The Gigablast host configuration file.\n");
	sb->safePrintf("# Tells us what hosts are participating in the distributed search engine.\n");

	sb->safePrintf("\n\n");
	sb->safePrintf("# How many mirrors do you want? If this is 0 then your data\n");
	sb->safePrintf("# will NOT be replicated. If it is 1 then each host listed\n");
	sb->safePrintf("# below will have one host that mirrors it, thereby decreasing\n");
	sb->safePrintf("# total index capacity, but increasing redundancy. If this is\n");
	sb->safePrintf("# 1 then the first half of hosts will be replicated by the\n");
	sb->safePrintf("# second half of the hosts listed below.\n");
	sb->safePrintf("\n");
	if(numMirrors>=0)
		sb->safePrintf("num-mirrors: %d\n",numMirrors);
	else
		sb->safePrintf("#num-mirrors: 0\n");
	sb->safePrintf("\n");
	
	sb->safePrintf("# Alternatively, how many shards(splits) are used?\n");
	if(indexSplits>0)
		sb->safePrintf("index-splits: %d\n",indexSplits);
	else
		sb->safePrintf("#index-splits: 1\n");
	
	sb->safePrintf("\n\n");
	sb->safePrintf("# List of hosts. Limited to 512 from MAX_HOSTS in Hostdb.h. Increase that\n");
	sb->safePrintf("# if you want more.\n");
	sb->safePrintf("#\n");

	sb->safePrintf("# Format:\n");
	sb->safePrintf("#\n");
	sb->safePrintf("# first   column: hostID (starts at 0 and increments from there)\n");
	sb->safePrintf("# second  column: the port used by the client DNS algorithms\n");
	sb->safePrintf("# third   column: port that HTTPS listens on\n");
	sb->safePrintf("# fourth  column: port that HTTP  listens on\n");
	sb->safePrintf("# fifth   column: port that udp server listens on\n");
	sb->safePrintf("# sixth   column: IP address or hostname that has an IP address in /etc/hosts\n");
	sb->safePrintf("# seventh column: like sixth column but for secondary ethernet port. Can be the same as the sixth column.\n");
	sb->safePrintf("# eigth   column: Working directory\n");
	sb->safePrintf("# ninth   column: An optional merge directory override\n");
	sb->safePrintf("# tenth   column: An optional text note that will display in the hosts table for this host.\n");

	sb->safePrintf("\n\n");
	sb->safePrintf("#\n");
	sb->safePrintf("# Example of a four-node distributed search index running on a single\n");
	sb->safePrintf("# server with four cores. The working directories are /home/mwells/hostN/.\n");
	sb->safePrintf("# The 'gb' binary resides in the working directories. We have to use\n");
	sb->safePrintf("# different ports for each gb instance since they are all on the same\n");
	sb->safePrintf("# server.\n");
	sb->safePrintf("#\n");

	sb->safePrintf("#\n");
	sb->safePrintf("#0 5998 7000 8000 9000 192.0.2.4 192.0.2.5 /home/mwells/host0/\n");
	sb->safePrintf("#1 5997 7001 8001 9001 192.0.2.4 192.0.2.5 /home/mwells/host1/\n");
	sb->safePrintf("#2 5996 7002 8002 9002 192.0.2.4 192.0.2.5 /home/mwells/host2/\n");
	sb->safePrintf("#3 5995 7003 8003 9003 192.0.2.4 192.0.2.5 /home/mwells/host3/\n");

	sb->safePrintf("\n");
	sb->safePrintf("# A four-node cluster with different merge dir:\n");
	sb->safePrintf("#0 5998 7000 8000 9000 192.0.2.4 192.0.2.5 /home/mwells/host0/ /mnt/merge/host0/\n");
	sb->safePrintf("#1 5997 7001 8001 9001 192.0.2.4 192.0.2.5 /home/mwells/host1/ /mnt/merge/host1/\n");
	sb->safePrintf("#2 5996 7002 8002 9002 192.0.2.4 192.0.2.5 /home/mwells/host2/ /mnt/merge/host2/\n");
	sb->safePrintf("#3 5995 7003 8003 9003 192.0.2.4 192.0.2.5 /home/mwells/host3/ /mnt/merge/host3/\n");

	sb->safePrintf("\n");
	sb->safePrintf("# A four-node cluster on four different servers:\n");
	sb->safePrintf("#0 5998 7000 8000 9000 192.0.2.4 192.0.2.5 /home/mwells/gigablast/\n");
	sb->safePrintf("#1 5998 7000 8000 9000 192.0.2.6 192.0.2.7 /home/mwells/gigablast/\n");
	sb->safePrintf("#2 5998 7000 8000 9000 192.0.2.8 192.0.2.9 /home/mwells/gigablast/\n");
	sb->safePrintf("#3 5998 7000 8000 9000 192.0.2.10 192.0.2.11 /home/mwells/gigablast/\n");
	sb->safePrintf("\n\n");
	sb->safePrintf("#\n");
	sb->safePrintf("# Example of an eight-node cluster.\n");
	sb->safePrintf("# Each line represents a single gb process with dual ethernet ports\n");
	sb->safePrintf("# whose IP addresses are in /etc/hosts under se0, se0b, se1, se1b, ...\n");
	sb->safePrintf("#\n");
	sb->safePrintf("#0 5998 7000 8000 9000 se0 se0b /home/mwells/gigablast/\n");
	sb->safePrintf("#1 5998 7000 8000 9000 se1 se1b /home/mwells/gigablast/\n");
	sb->safePrintf("#2 5998 7000 8000 9000 se2 se2b /home/mwells/gigablast/\n");
	sb->safePrintf("#3 5998 7000 8000 9000 se3 se3b /home/mwells/gigablast/\n");
	sb->safePrintf("#4 5998 7000 8000 9000 se4 se4b /home/mwells/gigablast/\n");
	sb->safePrintf("#5 5998 7000 8000 9000 se5 se5b /home/mwells/gigablast/\n");
	sb->safePrintf("#6 5998 7000 8000 9000 se6 se6b /home/mwells/gigablast/\n");
	sb->safePrintf("#7 5998 7000 8000 9000 se7 se7b /home/mwells/gigablast/\n");
}


bool Hostdb::createHostsConf(const char *cwd) {
	fprintf(stderr,"Creating %shosts.conf\n", cwd);
	
	SafeBuf sb;
	printHostsConfPreamble(&sb, 0, -1);

	sb.safePrintf("\n");
	sb.safePrintf("\n");
	sb.safePrintf("0 5998 7000 8000 9000 127.0.0.1 127.0.0.1 %s\n", cwd);


	log("%shosts.conf does not exist, creating it", cwd);
	int rc = sb.save(cwd, "hosts.conf");
	return rc>=0;
}


bool Hostdb::saveHostsConf() {
	SafeBuf sb;
	//we don't recalculate "numMirrors" so that is always transformed into numsplits. Hrmpf.
	printHostsConfPreamble(&sb, -1, m_indexSplits);
	
	for(int32_t i = 0; i < m_numTotalHosts; i++) {
		Host *h;
		if(i < m_numHosts)
			h = getHost(i);
		else if(i < m_numHosts + m_numSpareHosts)
			h = getSpare(i - m_numHosts);
		else
			h = getProxy(i - m_numHosts - m_numSpareHosts);

		// generate the host id
		if(i >= m_numHosts + m_numSpareHosts)
			sb.safePrintf("proxy ");
		else if(i >= m_numHosts )
			sb.safePrintf("spare ");
		else
			sb.safePrintf("%03d ", i);
		
		sb.safePrintf("%5d %5d %5d %5d %s %s %s %s %s %s\n",
		              h->m_dnsClientPort,
			      h->getInternalHttpsPort(),
			      h->getInternalHttpPort(),
			      h->m_port,
			      h->m_hostname, h->m_hostname2,
			      h->m_dir,
			      h->m_mergeDir,
			      h->m_mergeLockDir,
			      h->m_note);
	}
	if(sb.save(m_dir,"hosts.conf")>=0)
		return true;
	else
		return false;
}


static int32_t s_localIps[20];
int32_t *getLocalIps ( ) {
	static bool s_valid = false;
	if ( s_valid ) return s_localIps;
	s_valid = true;
	struct ifaddrs *ifap = NULL;
	if ( getifaddrs( &ifap ) < 0 ) {
		log("hostdb: getifaddrs: %s.",mstrerror(errno));
		return NULL;
	}
	int32_t ni = 0;
	// store loopback just in case
	int32_t loopback = atoip("127.0.0.1");
	s_localIps[ni++] = loopback;
	for(ifaddrs *p = ifap; p && ni < 18 ; p = p->ifa_next) {
		if ( ! p->ifa_addr ) continue;
		if(p->ifa_addr->sa_family != AF_INET)
			continue;
		struct sockaddr_in *xx = (sockaddr_in *)(void*)p->ifa_addr;
		int32_t ip = xx->sin_addr.s_addr;
		// skip if loopback we stored above
		if ( ip == loopback ) continue;
		// skip bogus ones
		if ( (uint32_t)ip <= 10 ) continue;
		// show it
		//log("host: detected local ip %s",iptoa(ip));
		// otherwise store it
		s_localIps[ni++] = ip;
	}
	// mark the end of it
	s_localIps[ni] = 0;
	// free that memore
	freeifaddrs ( ifap );
	// return the static buffer
	return s_localIps;
}


Host *Hostdb::getHost2 ( const char *cwd , int32_t *localIps ) {
	for ( int32_t i = 0 ; i < m_numHosts ; i++ ) {
		Host *h = &m_hosts[i];
		// . get the path. guaranteed to end in '/'
		//   as well as cwd!
		// . if the gb binary does not reside in the working dir
		//   for this host, skip it, it's not our host
		if ( strcmp(h->m_dir,cwd) != 0 ) continue;
		// now it must be our ip as well!
		int32_t *ipPtr = localIps;
		for ( ; *ipPtr ; ipPtr++ ) 
			// return the host if it also matches the ip!
			if ( (int32_t)h->m_ip == *ipPtr ) return h;
	}
	// what, no host?
	return NULL;
}

Host *Hostdb::getProxy2 ( const char *cwd , int32_t *localIps ) {
	for ( int32_t i = 0 ; i < m_numProxyHosts ; i++ ) {
		Host *h = getProxy(i);
		if ( ! (h->m_type & HT_PROXY ) ) continue;
		// . get the path. guaranteed to end in '/'
		//   as well as cwd!
		// . if the gb binary does not reside in the working dir
		//   for this host, skip it, it's not our host
		if ( strcmp(h->m_dir,cwd) != 0 ) continue;
		// now it must be our ip as well!
		int32_t *ipPtr = localIps;
		for ( ; *ipPtr ; ipPtr++ ) 
			// return the host if it also matches the ip!
			if ( (int32_t)h->m_ip == *ipPtr ) return h;
	}
	// what, no host?
	return NULL;
}
