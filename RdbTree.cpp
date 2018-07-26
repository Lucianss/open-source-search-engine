#include "RdbTree.h"
#include "Collectiondb.h"
#include "JobScheduler.h"
#include "Mem.h"
#include "RdbMem.h"
#include "BigFile.h"
#include "RdbList.h"
#include "Spider.h"
#include "Process.h"
#include "Conf.h"
#include "ScopedLock.h"
#include <fcntl.h>


RdbTree::RdbTree () {
	m_collnums= NULL;
	m_keys    = NULL;
	m_data    = NULL;
	m_sizes   = NULL;
	m_left    = NULL;
	m_right   = NULL;
	m_parents = NULL;
	m_depth   = NULL;
	m_headNode      = -1;
	m_numNodes      =  0;
	m_numUsedNodes  =  0;
	m_memAllocated  =  0;
	m_memOccupied   =  0;
	m_nextNode      =  0;
	m_minUnusedNode =  0; 
	m_fixedDataSize = -1; // variable dataSize, depends on individual node
	m_needsSave     = false;
	m_pickRight     = false;

	// PVS-Studio
	memset(m_dir, 0, sizeof(m_dir));
	memset(m_memTag, 0, sizeof(m_memTag));
	m_state = NULL;
	m_callback = NULL;
	m_ownData = false;
	m_overhead = 0;
	m_maxMem = 0;
	m_allocName = NULL;
	m_bytesWritten = 0;
	m_bytesRead = 0;
	m_errno = 0;
	m_ks = 0;
	m_corrupt = 0;
	
	// before resetting... we have to set this so clear() won't breach buffers
	m_rdbId = -1;

	reset_unlocked();
}

RdbTree::~RdbTree ( ) {
	reset_unlocked();
}


// "memMax" includes records plus the overhead
bool RdbTree::set(int32_t fixedDataSize, int32_t maxNumNodes, int32_t memMax, bool ownData,
                  const char *allocName, const char *dbname, char keySize, char rdbId) {
	ScopedLock sl(m_mtx);

	reset_unlocked();
	m_fixedDataSize   = fixedDataSize; 
	m_maxMem          = memMax;
	m_ownData         = ownData;
	m_allocName       = allocName;
	m_ks              = keySize;

	m_needsSave       = false;

	m_dbname[0] = '\0';
	if ( dbname ) {
		int32_t dlen = strlen(dbname);
		if ( dlen > 30 ) dlen = 30;
		memcpy(m_dbname,dbname,dlen);
		m_dbname[dlen] = '\0';
	}

	// a malloc tag, must be LESS THAN 16 bytes including the NULL
	char *p = m_memTag;
	memcpy  ( p , "RdbTree" , 7 ); p += 7;
	if ( dbname ) strncpy ( p , dbname    , 8 );
	p += 8;

	*p++ = '\0';
	// set rdbid
	m_rdbId = rdbId; // -1;
	// sanity
	if ( rdbId < -1       ) { g_process.shutdownAbort(true); }
	if ( rdbId >= RDB_END ) { g_process.shutdownAbort(true); }

	// adjust m_maxMem to virtual infinity if it was -1
	if ( m_maxMem < 0 ) m_maxMem = 0x7fffffff;
	// . compute each node's memory overhead
	// . size of a key/left/right/parent
	m_overhead = (m_ks + 4*3 );
	// include collection number, currently an uint16_t
	m_overhead += sizeof(collnum_t);
	// if we're a non-zero data length include a dataptr (-1 means variabl)
	if ( m_fixedDataSize !=  0 ) m_overhead += 4;
	// include dataSize if our dataSize is variable (-1)
	if ( m_fixedDataSize == -1 ) m_overhead += 4;
	// if we're balanced include 1 byte per node for the depth
	m_overhead += 1;
	if( maxNumNodes == -1) {
		maxNumNodes = m_maxMem / m_overhead;
		if(maxNumNodes > 10000000) maxNumNodes = 10000000;
	}
	// allocate the nodes
	return growTree_unlocked(maxNumNodes);
}

void RdbTree::reset() {
	ScopedLock sl(m_mtx);
	reset_unlocked();
}

void RdbTree::reset_unlocked() {
	// make sure string is NULL temrinated. this strlen() should 
	if ( m_numNodes > 0 && 
	     m_dbname[0] &&
	     // don't be spammy we can have thousands of these, one per coll
	     strcmp(m_dbname,"waitingtree") != 0 )
		log(LOG_INFO,"db: Resetting tree for %s.",m_dbname);

	// liberate all the nodes
	clear_unlocked();

	// do not require saving after a reset
	m_needsSave = false;
	// now free all the overhead structures of this tree
	int32_t n = m_numNodes;
	// free array of collectio numbers (shorts for now)
	if ( m_collnums) mfree ( m_collnums, sizeof(collnum_t) *n,m_allocName);
	// free the array of keys
	if ( m_keys  ) mfree ( m_keys  , m_ks      * n , m_allocName ); 
	// free the data ptrs
	if ( m_data  ) mfree ( m_data  , sizeof(char *) * n , m_allocName ); 
	// free the array of dataSizes
	if ( m_sizes ) mfree ( m_sizes , n * 4              , m_allocName ); 
	// free the sorted node #'s
	if ( m_left    ) mfree ( m_left    , n * 4 ,m_allocName);
	if ( m_right   ) mfree ( m_right   , n * 4 ,m_allocName);
	if ( m_parents ) mfree ( m_parents , n * 4 ,m_allocName);
	if ( m_depth   ) mfree ( m_depth   , n     ,m_allocName);
	m_collnums      = NULL; 
	m_keys          = NULL; 
	m_data          = NULL;
	m_sizes         = NULL;
	m_left          = NULL;
	m_right         = NULL;
	m_parents       = NULL;
	m_depth         = NULL;
	// tree description vars
	m_headNode      = -1;
	m_numNodes      =  0;
	m_numUsedNodes  =  0;
	m_memAllocated  =  0;
	m_memOccupied   =  0;
	m_nextNode      =  0;
	m_minUnusedNode =  0; 
	m_fixedDataSize = -1; // variable dataSize, depends on individual node
	// clear counts
	m_numNegativeKeys = 0;
	m_numPositiveKeys = 0;
	m_isSaving        = false;
}

void RdbTree::delColl ( collnum_t collnum ) {
	ScopedLock sl(m_mtx);

	m_needsSave = true;
	const char *startKey = KEYMIN();
	const char *endKey   = KEYMAX();
	deleteNodes_unlocked(collnum, startKey, endKey, true/*freeData*/) ;
}

int32_t RdbTree::clear() {
	ScopedLock sl(m_mtx);
	return clear_unlocked();
}

// . this just makes all the nodes available for occupation (liberates them)
// . it does not free this tree's control structures
// . returns # of occupied nodes we liberated
int32_t RdbTree::clear_unlocked( ) {
	if ( m_numUsedNodes > 0 ) m_needsSave = true;
	// the liberation count
	int32_t count = 0;
	// liberate all of our nodes
	int32_t dataSize = m_fixedDataSize;
	for ( int32_t i = 0 ; i < m_minUnusedNode ; i++ ) {
		// skip node if parents is -2 (unoccupied)
		if ( m_parents[i] == -2 ) continue;
		// we no longer count the overhead of this node as occupied
		m_memOccupied -= m_overhead;
		// make the ith node available for occupation
		m_parents[i] = -2;
		// keep count
		count++;
		// continue if we have no data to free
		if ( ! m_data ) continue;
		// read dataSize from m_sizes[i] if it's not fixed
		if ( m_fixedDataSize == -1 ) dataSize = m_sizes[i];
		// free the data being pointed to
		if ( m_ownData ) mfree ( m_data[i] , dataSize ,m_allocName);
		// adjust our reported memory usage
		m_memAllocated -= dataSize;
		m_memOccupied -= dataSize;
	}
	// reset all these
	m_headNode      = -1;
	m_numUsedNodes  =  0;
	m_nextNode      =  0;
	m_minUnusedNode =  0; 
	// clear counts
	m_numNegativeKeys = 0;
	m_numPositiveKeys = 0;


	// clear tree counts for all collections!
	int32_t nc = g_collectiondb.getNumRecs();
	// BUT only if we are an Rdb::m_tree!!!
	if ( m_rdbId == -1 ) nc = 0;
	// otherwise, we overwrite stuff in CollectionRec we shouldn't
	for ( int32_t i = 0 ; i < nc ; i++ ) {
		CollectionRec *cr = g_collectiondb.getRec(i);
		if ( ! cr ) continue;
		if ( m_rdbId < 0 ) continue;
		//if (((unsigned char)m_rdbId)>=RDB_END){g_process.shutdownAbort(true); }
		cr->m_numNegKeysInTree[(unsigned char)m_rdbId] = 0;
		cr->m_numPosKeysInTree[(unsigned char)m_rdbId] = 0;
	}

	return count;
}

bool RdbTree::getNode(collnum_t collnum, const char *key) const {
	ScopedLock sl(m_mtx);
	return (getNode_unlocked(collnum, key) >= 0);
}

// . used by cache 
// . wrapper for getNode_unlocked()
int32_t RdbTree::getNode_unlocked(collnum_t collnum, const char *key) const {
	m_mtx.verify_is_locked();

	int32_t i = m_headNode;

	// get the node (about 4 cycles per loop, 80cycles for 1 million items)
	while (i != -1) {
		if (collnum < m_collnums[i]) {
			i = m_left[i];
			continue;
		}

		if (collnum > m_collnums[i]) {
			i = m_right[i];
			continue;
		}

		int cmp = KEYCMP(key, 0, m_keys, i, m_ks);
		if (cmp < 0) {
			i = m_left[i];
			continue;
		}

		if (cmp > 0) {
			i = m_right[i];
			continue;
		}

		return i;
	}

	return -1;
}

// . returns node # whose key is >= "key"
// . returns -1 if none
// . used by RdbTree::getList()
// . TODO: spiderdb stores records by time so our unbalanced tree really hurts
//         us for that.
// . TODO: keep a m_lastStartNode and start from that since it tends to only
//         increase startKey via Msg3. if the key at m_lastStartNode is <=
//         the provided key then we did well.
int32_t RdbTree::getNextNode_unlocked(collnum_t collnum, const char *key) const {
	m_mtx.verify_is_locked();

	// return -1 if no non-empty nodes in the tree
	if ( m_headNode < 0 ) return -1;
	// get the node (about 4 cycles per loop, 80cycles for 1 million items)
	int32_t parent;
	int32_t i = m_headNode ;

	while ( i != -1 ) {
		parent = i;
		if ( collnum < m_collnums[i] ) { i = m_left [i]; continue;}
		if ( collnum > m_collnums[i] ) { i = m_right[i]; continue;}
		int cmp = KEYCMP(key,0,m_keys,i,m_ks);
		if (cmp<0) { i = m_left [i]; continue;}
		if (cmp>0) { i = m_right[i]; continue;}
		return i;
        }
	if ( m_collnums [ parent ] >  collnum ) return parent;
	if ( m_collnums [ parent ] == collnum && //m_keys [ parent ] > key ) 
	     KEYCMP(m_keys,parent,key,0,m_ks)>0 )
		return parent;
	return getNextNode_unlocked(parent);
}

int32_t RdbTree::getFirstNode_unlocked() const {
	m_mtx.verify_is_locked();

	const char *k = KEYMIN();
	return getNextNode_unlocked(0, k);
}

int32_t RdbTree::getLastNode_unlocked() const {
	m_mtx.verify_is_locked();

	const char *k = KEYMAX();
	return getPrevNode_unlocked((collnum_t)0x7fff, k);
}

// . get the node whose key is <= "key"
// . returns -1 if none
int32_t RdbTree::getPrevNode_unlocked(collnum_t collnum, const char *key) const {
	m_mtx.verify_is_locked();

	// return -1 if no non-empty nodes in the tree
	if ( m_headNode < 0  ) return -1;
	// get the node (about 4 cycles per loop, 80cycles for 1 million items)
	int32_t parent;
	int32_t i = m_headNode ;
	while ( i != -1 ) {
		parent = i;
		if ( collnum < m_collnums[i] ) { i = m_left [i]; continue;}
		if ( collnum > m_collnums[i] ) { i = m_right[i]; continue;}
		int cmp = KEYCMP(key,0,m_keys,i,m_ks);
		if ( cmp<0) {i=m_left [i];continue;}
		if ( cmp>0) {i=m_right[i];continue;}
		return i;
        }
	if ( m_collnums [ parent ] <  collnum ) return parent;
	if ( m_collnums [ parent ] == collnum && //m_keys [ parent ] < key ) 
	     KEYCMP(m_keys,parent,key,0,m_ks) < 0 ) return parent;
	return getPrevNode_unlocked(parent);
}

const char* RdbTree::getKey_unlocked(int32_t node) const {
	m_mtx.verify_is_locked();

	return &m_keys[node*m_ks];
}

const char* RdbTree::getData(collnum_t collnum, const char *key) const {
	ScopedLock sl(m_mtx);
	int32_t n = getNode_unlocked(collnum, key);
	if (n < 0) return NULL;
	return m_data[n];
};

int32_t RdbTree::getDataSize_unlocked(int32_t node) const {
	m_mtx.verify_is_locked();

	if (m_fixedDataSize == -1) {
		return m_sizes[node];
	}
	return m_fixedDataSize;
}

// . "i" is the previous node number
// . we could eliminate m_parents[] array if we limited tree depth!
// . 24 cycles to get the first kid
// . averages around 50 cycles per call probably
// . 8 cycles are spent entering/exiting this subroutine (inline it? TODO)
int32_t RdbTree::getNextNode_unlocked(int32_t i) const {
	m_mtx.verify_is_locked();

	// cruise the kids if we have a right one
	if ( m_right[i] >= 0 ) {
		// go to the right kid
		i = m_right [ i ];
		// now go left as much as we can
		while ( m_left [ i ] >= 0 ) i = m_left [ i ];
		// return that node (it's a leaf or has one right kid)
		return i;
	}
	// now keep getting parents until one has a key bigger than i's key
	int32_t p = m_parents[i];
	// if parent is negative we're done
	if ( p < 0 ) return -1;
	// if we're the left kid of the parent, then the parent is the
	// next biggest node
	if ( m_left[p] == i ) return p;
	// otherwise keep getting the parent until it has a bigger key
	// or until we're the LEFT kid of the parent. that's better
	// cuz comparing keys takes longer. loop is 6 cycles per iteration.
	while ( p >= 0  &&  (m_collnums[p] < m_collnums[i] ||
			     ( m_collnums[p] == m_collnums[i] && 
			       KEYCMP(m_keys,p,m_keys,i,m_ks) < 0 )) )
		p = m_parents[p];
	// p will be -1 if none are left
	return p;
}

// . "i" is the next node number
int32_t RdbTree::getPrevNode_unlocked(int32_t i) const {
	m_mtx.verify_is_locked();

	// cruise the kids if we have a left one
	if ( m_left[i] >= 0 ) {
		// go to the left kid
		i = m_left [ i ];
		// now go right as much as we can
		while ( m_right [ i ] >= 0 ) i = m_right [ i ];
		// return that node (it's a leaf or has one left kid)
		return i;
	}
	// now keep getting parents until one has a key bigger than i's key
	int32_t p = m_parents[i];
	// if we're the right kid of the parent, then the parent is the
	// next least node
	if ( m_right[p] == i ) return p;
	// keep getting the parent until it has a bigger key
	// or until we're the RIGHT kid of the parent. that's better
	// cuz comparing keys takes longer. loop is 6 cycles per iteration.
	while ( p >= 0  &&  (m_collnums[p] > m_collnums[i] ||
			     ( m_collnums[p] == m_collnums[i] && 
			       KEYCMP(m_keys,p,m_keys,i,m_ks) > 0 )) )
		p = m_parents[p];
	// p will be -1 if none are left
	return p;
}

bool RdbTree::addKey(const void *key) {
	ScopedLock sl(m_mtx);
	return (addKey_unlocked(key) >= 0);
}

int32_t RdbTree::addKey_unlocked(const void *key) {
	m_mtx.verify_is_locked();

	return addNode_unlocked ( 0,(const char *)key,NULL,0);
}

bool RdbTree::addNode(collnum_t collnum, const char *key, char *data, int32_t dataSize) {
	ScopedLock sl(m_mtx);
	return (addNode_unlocked(collnum, key, data, dataSize) >= 0);
}

// . returns -1 if we coulnd't allocate the new space and sets g_errno to ENOMEM
//   or ETREENOGROW, ...
// . returns node # we added it to on success
// . this will replace any current node with the same key
// . sets retNode to the node we added the data to (used internally)
// . negative dataSizes should be interpreted as 0
// . probably about 120 cycles per add means we can add 2 million per sec
// . NOTE: does not check to see if it will exceed m_maxMem
int32_t RdbTree::addNode_unlocked ( collnum_t collnum , const char *key , char *data , int32_t dataSize ) {
	m_mtx.verify_is_locked();

	// if there's no more available nodes, error out
	if ( m_numUsedNodes >= m_numNodes) { g_errno = ENOMEM; return -1; }
	// we need to be saved now
	m_needsSave = true;

	// sanity check - no empty positive keys for doledb
	if ( m_rdbId == RDB_DOLEDB && dataSize == 0 && (key[0]&0x01) == 0x01){
		g_process.shutdownAbort(true); }

	// for posdb
	if ( m_ks == 18 &&(key[0] & 0x06) ) {g_process.shutdownAbort(true); }

	// set up vars
	int32_t iparent ;
	int32_t rightGuy;
	// this is -1 iff there are no nodes used in the tree
	int32_t i = m_headNode;
	// . find the parent of node i and call it "iparent"
	// . if a node exists with our key then replace it
	while ( i != -1 ) {
		iparent = i;
		if ( collnum < m_collnums[i] ) { i = m_left [i]; continue;}
		if ( collnum > m_collnums[i] ) { i = m_right[i]; continue;}
		int cmp = KEYCMP(key, 0, m_keys, i, m_ks);
		if (cmp < 0) {
			i = m_left[i];
		} else if (cmp > 0) {
			i = m_right[i];
		} else {
			goto replaceIt;
		}
	}

	// . this overhead is key/left/right/parent
	// . we inc it by the data and sizes array if we need to below
	m_memOccupied += m_overhead;
	// point i to the next available node
	i = m_nextNode;
	// debug msg
	//if ( m_dbname && m_dbname[0]=='t' && dataSize >= 4 )
	//	logf(LOG_DEBUG,
	//	     "adding node #%" PRId32" with data ptr at %" PRIx32" "
	//	     "and data size of %" PRId32" into a list.",
	//	     i,data,dataSize);
	// if we're the first node we become the head node and our parent is -1
	if ( m_numUsedNodes == 0 ) {
		m_headNode =  i;
		iparent    = -1;
		// ensure these are right
		m_numNegativeKeys = 0;
		m_numPositiveKeys = 0;
		// we only use these stats for Rdb::m_trees for a 
		// PER COLLECTION count, since there can be multiple 
		// collections using the same Rdb::m_tree!
		// crap, when fixing a tree this will segfault because
		// m_recs[collnum] is NULL.
		if ( m_rdbId >= 0 && g_collectiondb.getRec(collnum) ) {
			//if( ((unsigned char)m_rdbId)>=RDB_END){g_process.shutdownAbort(true); }
			g_collectiondb.getRec(collnum)->
				m_numNegKeysInTree[(unsigned char)m_rdbId] =0;
			g_collectiondb.getRec(collnum)->
				m_numPosKeysInTree[(unsigned char)m_rdbId] =0;
		}
	}

	// stick ourselves in the next available node, "m_nextNode"
	//m_keys    [ i ] = key;
	KEYSET ( &m_keys[i*m_ks] , key , m_ks );
	m_parents [ i ] = iparent;
	// save collection number now, too
	m_collnums [ i ] = collnum;
	// add the key
	// set the data and size only if we need to
	if ( m_fixedDataSize != 0 ) {
		m_data [ i ] = data;
		// ack used and occupied mem
		if ( m_fixedDataSize >= 0 ) {
			m_memAllocated += m_fixedDataSize;
			m_memOccupied += m_fixedDataSize;
		}
		else {
			m_memAllocated += dataSize ; 
			m_memOccupied += dataSize ;
		}
		// we may have a variable size of data as well
		if ( m_fixedDataSize == -1 ) m_sizes [ i ] = dataSize;
	}
	// make our parent, if any, point to us
	if ( iparent >= 0 ) {
		if      ( collnum < m_collnums[iparent] ) 
			m_left [iparent] = i;
		else if (collnum==m_collnums[iparent]&&//key<m_keys[iparent] ) 
			 KEYCMP(key,0,m_keys,iparent,m_ks)<0 )
			m_left [iparent] = i;
		else    
			m_right[iparent] = i;
	}
	// . the right kid of an empty node is used as a linked list of
	//   empty nodes formed by deleting nodes
	// . we keep the linked list so we can re-used these vacated nodes
	rightGuy = m_right [ i ];
	// our kids are -1 (none)
	m_left  [ i ] = -1;
	m_right [ i ] = -1;
	// . if we weren't recycling a node then advance to next
	// . m_minUnusedNode is the lowest node number that was never filled
	//   at any one time in the past
	// . you might call it the brand new housing district
	if ( m_nextNode == m_minUnusedNode ) {m_nextNode++; m_minUnusedNode++;}
	// . otherwise, we're in a linked list of vacated used houses
	// . we have a linked list in the right kid
	// . make sure the new head doesn't have a left
	else {
		// point m_nextNode to the next available used house, if any
		if ( rightGuy >= 0 ) m_nextNode = rightGuy;
		// otherwise point it to the next brand new house (TODO:REMOVE)
		// this is an error, try to fix the tree
		else {
			log(LOG_WARN, "db: Encountered corruption in tree while trying to add a record. "
				"You should replace your memory sticks.");
			if ( !fixTree_unlocked() ) {
				g_process.shutdownAbort(true);
			}
		}
	}
	// we have one more used node
	increaseNodeCount_unlocked(collnum, key);

	// our depth is now 1 since we're a leaf node
	// (we include ourself)
	m_depth [ i ] = 1;

	// . reset depths starting at i's parent and ascending the tree
	// . will balance if child depths differ by 2 or more
	setDepths_unlocked(iparent);

	// return the node number of the node we occupied
	return i; 

	// come here to replace node i with the new data/dataSize
 replaceIt:
	// debug msg
	//fprintf(stderr,"replaced it!\n");
	// if we don't support any data then we're done
	if ( m_fixedDataSize == 0 ) {
		return i;
	}
	// get dataSize
	int32_t oldDataSize = m_fixedDataSize;
	// if datasize was 0 cuz it was a negative key, fix that for
	// calculating m_memOccupied
	if ( m_fixedDataSize >= 0 ) dataSize = m_fixedDataSize;
	if ( m_fixedDataSize < 0  ) oldDataSize = m_sizes[i];
	// free i's data
	if ( m_data[i] && m_ownData ) 
		mfree ( m_data[i] , oldDataSize ,m_allocName);
	// decrease mem occupied and increase by new size
	m_memOccupied -= oldDataSize;
	m_memOccupied += dataSize;
	m_memAllocated -= oldDataSize;
	m_memAllocated += dataSize;
	// otherwise set the data
	m_data [ i ] = data;
	// set the size if we need to as well
	if ( m_fixedDataSize < 0 ) m_sizes [ i ] = dataSize;
	return i;
}

bool RdbTree::deleteNode(collnum_t collnum, const char *key, bool freeData) {
	ScopedLock sl(m_mtx);
	return deleteNode_unlocked(collnum, key, freeData);
}

bool RdbTree::deleteNode_unlocked(collnum_t collnum, const char *key, bool freeData) {
	int32_t node = getNode_unlocked(collnum, key);
	if (node == -1) {
		return false;
	}

	return deleteNode_unlocked(node, freeData);
}

// delete all nodes with keys in [startKey,endKey]
void RdbTree::deleteNodes_unlocked(collnum_t collnum, const char *startKey, const char *endKey, bool freeData) {
	m_mtx.verify_is_locked();

	int32_t node = getNextNode_unlocked(collnum, startKey);
	while ( node >= 0 ) {
		//int32_t next = getNextNode_unlocked ( node );
		if ( m_collnums[node] != collnum ) break;
		//if ( m_keys    [node] > endKey   ) return;
		if ( KEYCMP(m_keys,node,endKey,0,m_ks) > 0 ) break;
		deleteNode_unlocked(node, freeData);
		// rotation in setDepths_unlocked() will cause him to be replaced
		// with one of his kids, unless he's a leaf node
		//node = next;
		node = getNextNode_unlocked(collnum, startKey);
	}
}

void RdbTree::increaseNodeCount_unlocked(collnum_t collNum, const char *key) {
	m_mtx.verify_is_locked();

	m_numUsedNodes++;

	if (KEYNEG(key)) {
		m_numNegativeKeys++;
		if (m_rdbId >= 0) {
			CollectionRec *cr = g_collectiondb.getRec(collNum);
			if (cr) {
				cr->m_numNegKeysInTree[(unsigned char)m_rdbId]++;
			}
		}
	} else {
		m_numPositiveKeys++;
		if (m_rdbId >= 0) {
			CollectionRec *cr = g_collectiondb.getRec(collNum);
			if (cr) {
				cr->m_numPosKeysInTree[(unsigned char)m_rdbId]++;
			}
		}
	}
}

void RdbTree::decreaseNodeCount_unlocked(collnum_t collNum, const char *key) {
	m_mtx.verify_is_locked();

	// we have one less used node
	m_numUsedNodes--;

	// update sign counts
	if (KEYNEG(key)) {
		m_numNegativeKeys--;
		if (m_rdbId >= 0) {
			CollectionRec *cr = g_collectiondb.getRec(collNum);
			if (cr) {
				cr->m_numNegKeysInTree[(unsigned char)m_rdbId]--;
			}
		}
	} else {
		m_numPositiveKeys--;
		if (m_rdbId >= 0) {
			CollectionRec *cr = g_collectiondb.getRec(collNum);
			if (cr) {
				cr->m_numPosKeysInTree[(unsigned char)m_rdbId]--;
			}
		}
	}
}

// . now replace node #i with node #j
// . i should not equal j at this point
bool RdbTree::replaceNode_unlocked(int32_t i, int32_t j) {
	m_mtx.verify_is_locked();

	// . j's parent should take j's one kid
	// . that child should likewise point to j's parent
	// . j should only have <= 1 kid now because of our algorithm above
	// . if j's parent is i then j keeps his kid
	int32_t jparent = m_parents[j];
	if ( jparent != i ) {
		// parent:    if j is my left  kid, then i take j's right kid
		// otherwise, if j is my right kid, then i take j's left kid
		if ( m_left [ jparent ] == j ) {
			m_left  [ jparent ] = m_right [ j ];
			if (m_right[j]>=0) m_parents [ m_right[j] ] = jparent;
		}
		else {
			m_right [ jparent ] = m_left   [ j ];
			if (m_left [j]>=0) m_parents [ m_left[j] ] = jparent;
		}
	}

	// . j inherits i's children (providing i's child is not j)
	// . those children's parent should likewise point to j
	if ( m_left [i] != j ) {
		m_left [j] = m_left [i];
		if ( m_left[j] >= 0 ) m_parents[m_left [j]] = j;
	}
	if ( m_right[i] != j ) {
		m_right[j] = m_right[i];
		if ( m_right[j] >= 0 ) m_parents[m_right[j]] = j;
	}
	// j becomes the kid of i's parent, if any
	int32_t iparent = m_parents[i];
	if ( iparent >= 0 ) {
		if   ( m_left[iparent] == i ) m_left [iparent] = j;
		else                          m_right[iparent] = j;
	}
	// iparent may be -1
	m_parents[j] = iparent;

	// if i was the head node now j becomes the head node
	if ( m_headNode == i ) m_headNode = j;

	// . i joins the linked list of available used homes
	// . put it at the head of the list
	// . "m_nextNode" is the head node of the linked list
	m_right[i]   = m_nextNode;
	m_nextNode   = i;
	// . i's parent should be -2 so we know it's unused in case we're
	//   stepping through the nodes linearly for dumping in RdbDump
	// . used in getListUnordered()
	m_parents[i] = -2;
	// we have one less used node
	decreaseNodeCount_unlocked(m_collnums[i], getKey_unlocked(i));

	// our depth becomes that of the node we replaced, unless moving j
	// up to i decreases the total depth, in which case setDepths_unlocked() fixes
	m_depth [ j ] = m_depth [ i ];
	// . recalculate depths starting at old parent of j
	// . stops at the first node to have the correct depth
	// . will balance at pivot nodes that need it
	if ( jparent != i ) setDepths_unlocked(jparent);
	else setDepths_unlocked(j);
	// TODO: register growTree with g_mem to free on demand

	return true;
}

// . deletes node i from the tree
// . i's parent should point to i's left or right kid
// . if i has no parent then his left or right kid becomes the new top node
bool RdbTree::deleteNode_unlocked(int32_t i, bool freeData) {
	m_mtx.verify_is_locked();

	// watch out for double deletes
	if ( m_parents[i] == -2 ) {
		log(LOG_LOGIC,"db: Caught double delete.");
		return false;
	}

	// we need to be saved now
	m_needsSave = true;

	// we have one less occupied node
	m_memOccupied -= m_overhead;
	// . free it now iff "freeIt" is true (default is true)
	// . m_data can be a NULL array if m_fixedDataSize is fixed to 0
	if ( /*freeData &&*/ m_data ) {
		int32_t dataSize = m_fixedDataSize;
		if ( dataSize == -1 ) dataSize = m_sizes[i];
		if ( m_ownData ) mfree ( m_data [i] , dataSize ,m_allocName);
		m_memAllocated -= dataSize;
		m_memOccupied -= dataSize;
	}

	// j will be the node that replace node #i
	int32_t j = i;
	// . now find a node to replace node #i
	// . get a node whose key is just to the right or left of i's key
	// . get i's right kid
	// . then get that kid's LEFT MOST leaf-node descendant
	// . this little routine is stolen from getNextNode_unlocked(i)
	// . try to pick a kid from the right the same % of time as from left
	if ( ( m_pickRight     && m_right[j] >= 0 ) || 
	     ( m_left[j]   < 0 && m_right[j] >= 0 )  ) {
		// try to pick a left kid next time
		m_pickRight = false;
		// go to the right kid
		j = m_right [ j ];
		// now go left as much as we can
		while ( m_left [ j ] >= 0 ) j = m_left [ j ];
		// use node j (it's a leaf or has a right kid)
		return replaceNode_unlocked(i, j);
	}
	// . now get the previous node if i has no right kid
	// . this little routine is stolen from getPrevNode(i)
	if ( m_left[j] >= 0 ) {
		// try to pick a right kid next time
		m_pickRight = true;
		// go to the left kid
		j = m_left [ j ];
		// now go right as much as we can
		while ( m_right [ j ] >= 0 ) j = m_right [ j ];
		// use node j (it's a leaf or has a left kid)
		return replaceNode_unlocked(i, j);
	}

	// . come here if i did not have any kids (i's a leaf node)
	// . get i's parent
	int32_t iparent = m_parents[i];

	// make i's parent, if any, disown him
	if ( iparent >= 0 ) {
		if   ( m_left[iparent] == i ) m_left [iparent] = -1;
		else                          m_right[iparent] = -1;
	}
	// node i now goes to the top of the list of vacated, available homes
	m_right[i] = m_nextNode;
	// m_nextNode now points to i
	m_nextNode = i;
	// his parent is -2 (god) cuz he's dead and available
	m_parents[i] = -2;

	// . if we were the head node then, since we didn't have any kids,
	//   the tree must be empty
	// . one less node in the tree
	decreaseNodeCount_unlocked(m_collnums[i], getKey_unlocked(i));

	// . reset the depths starting at iparent and going up until unchanged
	// . will balance at pivot nodes that need it
	setDepths_unlocked(iparent);

	// tree must be empty
	if (m_numUsedNodes <= 0) {
		m_headNode = -1;
		// this will nullify our linked list of vacated, used homes
		m_nextNode = 0;
		m_minUnusedNode = 0;
		// ensure these are right
		m_numNegativeKeys = 0;
		m_numPositiveKeys = 0;
		if (m_rdbId >= 0) {
			CollectionRec *cr;
			cr = g_collectiondb.getRec(m_collnums[i]);
			if (cr) {
				cr->m_numNegKeysInTree[(unsigned char)m_rdbId] = 0;
				cr->m_numPosKeysInTree[(unsigned char)m_rdbId] = 0;
			}
		}
	}

	return true;
}

bool RdbTree::fixTree() {
	ScopedLock sl(m_mtx);
	return fixTree_unlocked();
}

// . this fixes the tree
// returns false if could not fix tree and sets g_errno, otherwise true
bool RdbTree::fixTree_unlocked() {
	m_mtx.verify_is_locked();

	// on error, fix the linked list
	log(LOG_WARN, "db: Trying to fix tree for %s.", m_dbname);
	log(LOG_WARN, "db: %" PRId32" occupied nodes and %" PRId32" empty of top %" PRId32" nodes.",
	    m_numUsedNodes, m_minUnusedNode - m_numUsedNodes, m_minUnusedNode);

	// loop through our nodes
	int32_t n = m_minUnusedNode;
	int32_t count = 0;
	// "clear" the tree as far as addNode() is concerned
	m_headNode      = -1;
	m_numUsedNodes  =  0;
	m_memAllocated  =  0;
	m_memOccupied   =  0;
	m_nextNode      =  0;
	m_minUnusedNode =  0; 
	int32_t           max  = g_collectiondb.getNumRecs();
	log("db: Valid collection numbers range from 0 to %" PRId32".",max);

	/// @todo ALC should we check repair RDB as well?
	bool isTitledb = (m_rdbId == RDB_TITLEDB || m_rdbId == RDB2_TITLEDB2);
	bool isSpiderdb = (m_rdbId == RDB_SPIDERDB_DEPRECATED || m_rdbId == RDB2_SPIDERDB2_DEPRECATED);

	// now re-add the old nods to the tree, they should not be overwritten
	// by addNode()
	for ( int32_t i = 0 ; i < n ; i++ ) {
		// speed update
		if ( (i % 100000) == 0 ) 
			log("db: Fixing node #%" PRId32" of %" PRId32".",i,n);
		// skip if empty
		if ( m_parents[i] <= -2 ) continue;
			
		if ( isTitledb && m_data[i] ) {
			char *data = m_data[i];
			int32_t ucompSize = *(int32_t *)data;
			if ( ucompSize < 0 || ucompSize > 100000000 ) {
				log("db: removing titlerec with uncompressed "
				     "size of %i from tree",(int)ucompSize);
				continue;
			}
		}

		char *key = &m_keys[i*m_ks];
		if (isSpiderdb && m_data[i] && Spiderdb::isSpiderRequest((spiderdbkey_t *)key)) {
			char *data = m_data[i];
			data -= sizeof(spiderdbkey_t);
			data -= 4;
			SpiderRequest *sreq ;
			sreq =(SpiderRequest *)data;
			if ( strncmp(sreq->m_url,"http",4) != 0 ) {
				log("db: removing spiderrequest bad url "
					"%s from tree",sreq->m_url);
				//return false;
				continue;
			}
		}
			
			
		collnum_t cn = m_collnums[i];
		// verify collnum
		if ( cn <  0   ) continue;
		if ( cn >= max ) continue;
		// collnum of non-existent coll
		if ( m_rdbId>=0 && ! g_collectiondb.getRec(cn) )
			continue;
		// now add just to set m_right/m_left/m_parent
		if ( m_fixedDataSize == 0 )
			addNode_unlocked(cn,&m_keys[i*m_ks], NULL, 0 );
		else if ( m_fixedDataSize == -1 )
			addNode_unlocked(cn,&m_keys[i*m_ks],m_data[i],m_sizes[i] );
		else 
			addNode_unlocked(cn,&m_keys[i*m_ks],m_data[i], m_fixedDataSize);
		// count em
		count++;
	}

	log("db: Fix tree removed %" PRId32" nodes for %s.",n - count,m_dbname);
	// esure it is still good
	if ( !checkTree_unlocked(false, true) ) {
		log(LOG_WARN, "db: Fix tree failed.");
		return false;
	}
	log("db: Fix tree succeeded for %s.",m_dbname);
	return true;
}

void RdbTree::verifyIntegrity() {
	ScopedLock sl(m_mtx);

	if (!checkTree_unlocked(false, true)) {
		gbshutdownCorrupted();
	}
}

// returns false if tree had problem, true otherwise
bool RdbTree::checkTree_unlocked(bool printMsgs, bool doChainTest) const {
	m_mtx.verify_is_locked();

	int32_t hkp = 0;
	bool useHalfKeys = false;

	if (m_rdbId == RDB_LINKDB || m_rdbId == RDB2_LINKDB2) {
		useHalfKeys = true;
	}

	/// @todo ALC should we check repair RDB as well?
	bool isTitledb = (m_rdbId == RDB_TITLEDB || m_rdbId == RDB2_TITLEDB2);
	bool isSpiderdb = (m_rdbId == RDB_SPIDERDB_DEPRECATED || m_rdbId == RDB2_SPIDERDB2_DEPRECATED);

	// now check parent kid correlations
	for ( int32_t i = 0 ; i < m_minUnusedNode ; i++ ) {
		// skip node if parents is -2 (unoccupied)
		if ( m_parents[i] == -2 ) continue;
		// all half key bits must be off in here
		if ( useHalfKeys && (m_keys[i*m_ks] & 0x02) ) {
			hkp++;
			// turn it off
			m_keys[i*m_ks] &= 0xfd;
		}
		// for posdb
		if ( m_ks == 18 &&(m_keys[i*m_ks] & 0x06) ) {
			g_process.shutdownAbort(true); }

		if ( isTitledb && m_data[i] ) {
			char *data = m_data[i];
			int32_t ucompSize = *(int32_t *)data;
			if ( ucompSize < 0 || ucompSize > 100000000 ) {
				log("db: found titlerec with uncompressed "
					"size of %i from tree",(int)ucompSize);
				return false;
			}
		}

		char *key = &m_keys[i*m_ks];
		if ( isSpiderdb && m_data[i] &&
		     Spiderdb::isSpiderRequest ( (spiderdbkey_t *)key ) ) {
			char *data = m_data[i];
			data -= sizeof(spiderdbkey_t);
			data -= 4;
			SpiderRequest *sreq ;
			sreq =(SpiderRequest *)data;
			if(!sreq->m_urlIsDocId) {
				if ( strncmp(sreq->m_url,"http",4) != 0 ) {
					log("db: spiderrequest bad url %s",sreq->m_url);
					return false;
				}
			} else {
				//sreq->m_url must contain only digits
				size_t count = strspn(sreq->m_url,"0123456789");
				if(count==0 || sreq->m_url[count]!='\0') {
					log("db: spiderrequest bad url %s",sreq->m_url);
					return false;
				}
			}
		}

		// bad collnum?
		collnum_t cn = m_collnums[i];
		if ( m_rdbId>=0 && (cn >= g_collectiondb.getNumRecs() || cn < 0) ) {
			log(LOG_WARN, "db: bad collnum in tree");
			return false;
		}
		if ( m_rdbId>=0 && ! g_collectiondb.getRec(cn) ) {
			log(LOG_WARN, "db: collnum is obsolete in tree");
			return false;
		}

		// if no left/right kid it MUST be -1
		if ( m_left[i] < -1 ) {
			log(LOG_WARN, "db: Tree left kid < -1.");
			return false;
		}
		if ( m_left[i] >= m_numNodes ) {
			log(LOG_WARN, "db: Tree left kid is %" PRId32" >= %" PRId32".", m_left[i], m_numNodes);
			return false;
		}
		if ( m_right[i] < -1 ) {
			log(LOG_WARN, "db: Tree right kid < -1.");
			return false;
		}
		if ( m_right[i] >= m_numNodes ) {
			log(LOG_WARN, "db: Tree left kid is %" PRId32" >= %" PRId32".", m_right[i], m_numNodes);
			return false;
		}
		// check left kid
		if ( m_left[i] >= 0 && m_parents[m_left[i]] != i ) {
			log(LOG_WARN, "db: Tree left kid and parent disagree.");
			return false;
		}
		// then right kid
		if ( m_right[i] >= 0 && m_parents[m_right[i]] != i ) {
			log(LOG_WARN, "db: Tree right kid and parent disagree.");
			return false;
		}
		// MDW: why did i comment out the order checking?
		// check order
		if ( m_left[i] >= 0 &&
		     m_collnums[i] == m_collnums[m_left[i]] ) {
			char *key = &m_keys[i*m_ks];
			char *left = &m_keys[m_left[i]*m_ks];
			if ( KEYCMP(key,left,m_ks)<0) {
				log(LOG_WARN, "db: Tree left kid > parent %i", i);
				return false;
			}
			
		}
		if ( m_right[i] >= 0 &&
		     m_collnums[i] == m_collnums[m_right[i]] ) {
			char *key = &m_keys[i*m_ks];
			char *right = &m_keys[m_right[i]*m_ks];
			if ( KEYCMP(key,right,m_ks)>0) {
				log(LOG_WARN, "db: Tree right kid < parent %i %s < %s", i, KEYSTR(right, m_ks), KEYSTR(key, m_ks));
				return false;
			}
		}
	}

	if ( hkp > 0 ) {
		log( LOG_WARN, "db: Had %" PRId32" half key bits on for %s.", hkp, m_dbname );
		return false;
	}

	// now return if we aren't doing active balancing
	if ( ! m_depth ) return true;
	// debug -- just always return now
	if ( printMsgs )logf(LOG_DEBUG,"***m_headNode=%" PRId32", m_numUsedNodes=%" PRId32,
			      m_headNode,m_numUsedNodes);
	int32_t           max  = g_collectiondb.getNumRecs();
	// verify that parent links correspond to kids
	for ( int32_t i = 0 ; i < m_minUnusedNode ; i++ ) {
		// verify collnum
		collnum_t cn = m_collnums[i];
		if ( cn < 0 ) {
			log( LOG_WARN, "db: Got bad collnum in tree, %i.", cn );
			return false;
		}
		if ( cn > max ) {
			log( LOG_WARN, "db: Got too big collnum in tree. %i.", cn );
			return false;
		}

		int32_t P = m_parents [i];
		if ( P == -2 ) continue; // deleted node

		if ( P == -1 && i != m_headNode ) {
			log( LOG_WARN, "db: Tree node %" PRId32" has no parent.", i );
			return false;
		}

		// check kids
		if ( P>=0 && m_left[P] != i && m_right[P] != i ) {
			log( LOG_WARN, "db: Tree kids of node # %" PRId32" disowned him.", i );
			return false;
		}

		// speedy tests continue
		if ( ! doChainTest ) continue;
		// ensure i goes back to head node
		int32_t j = i;
		int32_t loopCount = 0;
		while ( j >= 0 ) { 
			if ( j == m_headNode ) {
				break;
			}
			// sanity -- loop check
			if ( ++loopCount > 10000 ) {
				log( LOG_WARN, "db: tree had loop" );
				return false;
			}
			j = m_parents[j];
		}

		if ( j != m_headNode ) {
			log( LOG_WARN, "db: Node # %" PRId32" does not lead back to head node.", i );
			return false;
		}

		if ( printMsgs ) {
		        char *k = &m_keys[i*m_ks];
			logf(LOG_DEBUG,"***node=%" PRId32" left=%" PRId32" rght=%" PRId32" "
			    "prnt=%" PRId32", depth=%" PRId32" c=%" PRId32" key=%s",
			    i,m_left[i],m_right[i],m_parents[i],
			    (int32_t)m_depth[i],(int32_t)m_collnums[i],
			     KEYSTR(k,m_ks));
		}
		//ensure depth
		int32_t newDepth = computeDepth_unlocked(i);
		if ( m_depth[i] != newDepth ) {
			log( LOG_WARN, "db: Tree node # %" PRId32"'s depth should be %" PRId32".", i, newDepth );
			return false;
		}
	}
	if ( printMsgs ) logf(LOG_DEBUG,"---------------");
	// no problems found
	return true;
}

// . grow tree to "n" nodes
// . this will now actually grow from a current size to a new one
bool RdbTree::growTree_unlocked(int32_t nn) {
	m_mtx.verify_is_locked();

	// if we're that size, bail
	if ( m_numNodes == nn ) return true;

	// old number of nodes
	int32_t on = m_numNodes;
	// some quick type info
	int32_t   k = m_ks;
	int32_t   d = sizeof(char *);

	char  *kp = NULL;
	int32_t  *lp = NULL;
	int32_t  *rp = NULL;
	int32_t  *pp = NULL;
	char **dp = NULL;
	int32_t  *sp = NULL;
	char  *tp = NULL;
	collnum_t *cp = NULL;

	// do the reallocs
	int32_t cs = sizeof(collnum_t);
	cp =(collnum_t *)mrealloc (m_collnums, on*cs,nn*cs,m_allocName);
	if ( ! cp ) {
		goto error;
	}

	kp = (char  *) mrealloc ( m_keys    , on*k , nn*k , m_allocName );
	if ( ! kp ) {
		goto error;
	}

	lp = (int32_t  *) mrealloc ( m_left    , on*4 , nn*4 , m_allocName );
	if ( ! lp ) {
		goto error;
	}

	rp = (int32_t  *) mrealloc ( m_right   , on*4 , nn*4 , m_allocName );
	if ( ! rp ) {
		goto error;
	}

	pp = (int32_t  *) mrealloc ( m_parents , on*4 , nn*4 , m_allocName );
	if ( ! pp ) {
		goto error;
	}

	// deal with data, sizes and depth arrays on a basis of need
	if ( m_fixedDataSize !=  0 ) {
		dp =(char **)mrealloc (m_data  , on*d,nn*d,m_allocName);
		if ( ! dp ) {
			goto error;
		}
	}
	if ( m_fixedDataSize == -1 ) {
		sp =(int32_t  *)mrealloc (m_sizes , on*4,nn*4,m_allocName);
		if ( ! sp ) {
			goto error;
		}
	}
	tp =(char  *)mrealloc (m_depth , on  ,nn  ,m_allocName);
	if ( ! tp ) {
		goto error;
	}

	// re-assign
	m_collnums= cp;
	m_keys    = kp;
	m_left    = lp;
	m_right   = rp;
	m_parents = pp;
	m_data    = dp;
	m_sizes   = sp;
	m_depth   = tp;

	// adjust memory usage
	m_memAllocated -= m_overhead * on;
	m_memAllocated += m_overhead * nn;
	// bitch an exit if too much
	if ( m_memAllocated > m_maxMem ) {
		log( LOG_ERROR, "db: Trying to grow tree for %s to %" PRId32", but max is %" PRId32". Consider changing gb.conf.",
		     m_dbname, m_memAllocated, m_maxMem );
		return false;
	}

	// and the new # of nodes we have
	m_numNodes = nn;

	return true;

 error:

	// . realloc back down if we need to
	// . downsizing should NEVER fail!
	if ( cp ) {
		collnum_t *ss = (collnum_t *)mrealloc ( cp , nn*cs , on*cs , m_allocName);
		if ( ! ss ) { g_process.shutdownAbort(true); }
		m_collnums = ss;
	}
	if ( kp ) {
		char  *kk = (char *)mrealloc ( kp, nn*k, on*k, m_allocName );
		if ( ! kk ) { g_process.shutdownAbort(true); }
		m_keys = kk;
	}
	if ( lp ) {
		int32_t  *x = (int32_t *)mrealloc ( lp , nn*4 , on*4 , m_allocName );
		if ( ! x ) { g_process.shutdownAbort(true); }
		m_left = x;
	}
	if ( rp ) {
		int32_t  *x = (int32_t *)mrealloc ( rp , nn*4 , on*4 , m_allocName );
		if ( ! x ) { g_process.shutdownAbort(true); }
		m_right = x;
	}
	if ( pp ) {
		int32_t  *x = (int32_t *)mrealloc ( pp , nn*4 , on*4 , m_allocName );
		if ( ! x ) { g_process.shutdownAbort(true); }
		m_parents = x;
	}
	if ( dp && m_fixedDataSize != 0 ) {
		char **p = (char **)mrealloc ( dp , nn*d , on*d , m_allocName );
		if ( ! p ) { g_process.shutdownAbort(true); }
		m_data = p;
	}
	if ( sp && m_fixedDataSize == -1 ) {
		int32_t  *x = (int32_t *)mrealloc ( sp , nn*4 , on*4 , m_allocName );
		if ( ! x ) { g_process.shutdownAbort(true); }
		m_sizes = x;
	}
	/// @note ALC the following code will be used if we add something else below the mrealloc of m_depth above
	//if ( tp ) {
	//	char *s = (char *)mrealloc ( tp , nn   , on   , m_allocName );
	//	if ( ! s ) { g_process.shutdownAbort(true); }
	//	m_depth = s;
	//}

	log( LOG_ERROR, "db: Failed to grow tree for %s from %" PRId32" to %" PRId32" bytes: %s.",
	     m_dbname, on, nn, mstrerror(g_errno) );
	return false;
}


int32_t RdbTree::getMemOccupiedForList_unlocked(collnum_t collnum, const char *startKey, const char *endKey, int32_t minRecSizes) const {
	m_mtx.verify_is_locked();

	int32_t ne = 0;
	int32_t size = 0;
	int32_t i = getNextNode_unlocked(collnum, startKey) ;
	while ( i  >= 0 ) {
		// break out if we should
		if ( KEYCMP(m_keys,i,endKey,0,m_ks) > 0 ) break;
		if ( m_collnums[i] != collnum ) break;
		if ( size >= minRecSizes      ) break;
		// num elements
		ne++;
		// do we got data?
		if ( m_data ) {
			// is size fixed?
			if ( m_fixedDataSize >= 0 ) size += m_fixedDataSize;
			else                        size += m_sizes[i];
		}
		// add in key overhead
		size += m_ks;
		// add in dataSize overhead (-1 means variable data size)
		if ( m_fixedDataSize < 0 ) size += 4;
		// advance
		i = getNextNode_unlocked(i);
	}
	// that's it
	return size;
}

int32_t RdbTree::getMemOccupiedForList() const {
	ScopedLock sl(m_mtx);
	return getMemOccupiedForList_unlocked();
}

int32_t RdbTree::getMemOccupiedForList_unlocked() const {
	m_mtx.verify_is_locked();

	int32_t mem = 0;
	if ( m_fixedDataSize >= 0 ) {
		mem += m_numUsedNodes * m_ks;
		mem += m_numUsedNodes * m_fixedDataSize;
		return mem;
	}
	// get total mem used by occupied nodes
	mem  = m_memOccupied;
	// remove left/right/parent for each used node (3 int32_ts)
	mem -= m_overhead * m_numUsedNodes;
	// but do include the key in the list, even though it's in the overhead
	mem += m_ks * m_numUsedNodes;
	// but don't include the dataSize in the overhead -- that's in list too
	mem -= 4 * m_numUsedNodes;

	return mem;
}

// . returns false and sets g_errno on error
// . throw all the records in this range into this list
// . probably about 24-50 cycles per key we add
// . if this turns out to be bottleneck we can use hardcore RdbGet later
// . RdbDump should use this
bool RdbTree::getList(collnum_t collnum, const char *startKey, const char *endKey, int32_t minRecSizes,
                      RdbList *list, int32_t *numPosRecs, int32_t *numNegRecs, bool useHalfKeys) const {
	ScopedLock sl(m_mtx);

	// reset the counts of positive and negative recs
	int32_t numNeg = 0;
	int32_t numPos = 0;
	if ( numNegRecs ) *numNegRecs = 0;
	if ( numPosRecs ) *numPosRecs = 0;

	// . set the start and end keys of this list
	// . set lists's m_ownData member to true
	list->reset();
	// got set m_ks first so the set ( startKey, endKey ) works!
	list->setKeySize(m_ks);
	list->set              ( startKey , endKey );
	list->setFixedDataSize ( m_fixedDataSize   );
	list->setUseHalfKeys   ( useHalfKeys       );
	// bitch if list does not own his own data
	if ( ! list->getOwnData() ) {
		g_errno = EBADENGINEER;
		log(LOG_LOGIC,"db: rdbtree: getList: List does not own data");
		return false;
	}
	// bail if minRecSizes is 0
	if ( minRecSizes == 0 ) return true;
	// return true if no non-empty nodes in the tree
	if ( m_numUsedNodes == 0 ) return true;

	// get first node >= startKey
	int32_t node = getNextNode_unlocked(collnum, startKey);
	if ( node < 0 ) return true;
	// if it's already beyond endKey, give up
	if ( KEYCMP ( m_keys,node,endKey,0,m_ks) > 0 ) return true;
	// or if we hit a different collection number
	if ( m_collnums [ node ] > collnum ) return true;
	// save lastNode for setting *lastKey
	int32_t lastNode = -1;
	// . how much space would whole tree take if we stored it in a list?
	// . this includes records that are deletes
	// . caller will often say give me 500MB for a fixeddatasize list
	//   that is heavily constrained by keys...
	int32_t growth = getMemOccupiedForList_unlocked();

	// do not allocate whole tree's worth of space if we have a fixed
	// data size and a finite minRecSizes
	if ( m_fixedDataSize >= 0 && minRecSizes >= 0 ) {
		// only assign if we require less than minRecSizes of growth
		// because some callers set minRecSizes to 500MB!!
		int32_t ng = minRecSizes + m_fixedDataSize + m_ks ;
		if ( ng < growth && ng > minRecSizes ) growth = ng;
	}

	// raise to virtual inifinite if not constraining us
	if ( minRecSizes < 0 ) minRecSizes = 0x7fffffff;

	// . nail it down if titledb because metalincs was getting
	//   out of memory errors when getting a bunch of titleRecs
	// . only do this for titledb/spiderdb lookups since it can be slow
	//   to go through a million indexdb nodes.
	// . this was because minRecSizes was way too big... 16MB i think
	// . if they pass us a size-unbounded request for a fixed data size
	//   list then we should call this as well... as in Msg22.cpp's
	//   call to msg5::getList for tfndb.
	//
	// ^^^^^ Partially obsolete rambling above ^^^^^
	// Until we have verified that no call uses some silly huge minRecSizes we keep the
	// counting threshold but have it a 2MB which is more suitable for modern hardware.
	// The problem is that the getMemOccupiedForList_unlocked() method has to traverse all the
	// relevant nodes to calculate the total size and doing so is not cheap.
	if ( m_fixedDataSize < 0 || minRecSizes >= 2*1024*1024 )
		growth = getMemOccupiedForList_unlocked(collnum, startKey, endKey, minRecSizes);

	// grow the list now
	if ( ! list->growList ( growth ) ) {
		log(LOG_WARN, "db: Failed to grow list to %" PRId32" bytes for storing records from tree: %s.",
		    growth, mstrerror(g_errno));
		return false;
	}

	// stop when we've hit or just exceed minRecSizes
	// or we're out of nodes
	for ( ; node >= 0 && list->getListSize() < minRecSizes ; node = getNextNode_unlocked(node) ) {
		// stop before exceeding endKey
		if ( KEYCMP (m_keys,node,endKey,0,m_ks) > 0 ) break;
		// or if we hit a different collection number
		if ( m_collnums [ node ] != collnum ) break;
		// if more recs were added to tree since we initialized the
		// list then grow the list to compensate so we do not end up
		// reallocating one key at a time.

		// add record to our list
		if ( m_fixedDataSize == 0 ) {
			if (!list->addRecord(&m_keys[node * m_ks], 0, NULL)) {
				log(LOG_WARN, "db: Failed to add record to tree list for %s: %s. Fix the growList algo.",
				    m_dbname,mstrerror(g_errno));
				return false;
			}
		}
		else {
			int32_t dataSize = (m_fixedDataSize == -1) ? m_sizes[node] : m_fixedDataSize;

			// point to key
			char *key = &m_keys[node*m_ks];

			// do not allow negative keys to have data, or
			// at least ignore it! let's RdbList::addRecord()
			// core dump on us!
			if (KEYNEG(key)) {
				dataSize = 0;
			}

			// add the key and data
			if (!list->addRecord(key, dataSize, m_data[node])) {
				log(LOG_WARN, "db: Failed to add record to tree list for %s: %s. Fix the growList algo.",
				    m_dbname,mstrerror(g_errno));
				return false;
			}
		}

		// we are little endian
		if ( KEYNEG(m_keys,node,m_ks) ) numNeg++;
		else                            numPos++;
		// save lastNode for setting *lastKey
		lastNode = node;
	}

	// set counts to pass back
	if ( numNegRecs ) *numNegRecs = numNeg;
	if ( numPosRecs ) *numPosRecs = numPos;
	// . we broke out of the loop because either:
	// . 1. we surpassed endKey OR
	// . 2. we hit or surpassed minRecSizes
	// . constrain the endKey of the list to the key of "node" minus 1
	// . "node" should be the next node we would have added to this list
	// . if "node" is < 0 then we can keep endKey set high the way it is

	// record the last key inserted into the list
	if ( lastNode >= 0 ) 
		list->setLastKey ( &m_keys[lastNode*m_ks] );
	// reset the list's endKey if we hit the minRecSizes barrier cuz
	// there may be more records before endKey than we put in "list"
	if ( list->getListSize() >= minRecSizes && lastNode >= 0 ) {
		// use the last key we read as the new endKey
		char newEndKey[MAX_KEY_BYTES];
		KEYSET(newEndKey,&m_keys[lastNode*m_ks],m_ks);
		// . if he's negative, boost new endKey by 1 because endKey's
		//   aren't allowed to be negative
		// . we're assured there's no positive counterpart to him 
		//   since Rdb::addRecord() doesn't allow both to exist in
		//   the tree at the same time
		// . if by some chance his positive counterpart is in the
		//   tree, then it's ok because we'd annihilate him anyway,
		//   so we might as well ignore him
		// we are little endian
		if ( KEYNEG(newEndKey,0,m_ks) ) KEYINC(newEndKey,m_ks);
		// if we're using half keys set his half key bit
		if ( useHalfKeys ) KEYOR(newEndKey,0x02);
		// tell list his new endKey now
		list->set ( startKey , newEndKey );
	}
	// reset list ptr to point to first record
	list->resetListPtr();

	// success
	return true;
}

// . this just estimates the size of the list 
// . the more balanced the tree the better the accuracy
// . this now returns total recSizes not # of NODES like it used to
//   in [startKey, endKey] in this tree
// . if the count is < 200 it returns an EXACT count
// . right now it only works for dataless nodes (keys only)
int32_t RdbTree::estimateListSize(collnum_t collnum, const char *startKey, const char *endKey, char *minKey, char *maxKey) const {
	ScopedLock sl(m_mtx);

	// make these as benign as possible
	if ( minKey ) KEYSET ( minKey , endKey   , m_ks );
	if ( maxKey ) KEYSET ( maxKey , startKey , m_ks );
	// get order of a key as close to startKey as possible
	int32_t order1 = getOrderOfKey_unlocked(collnum, startKey, minKey);
	// get order of a key as close to endKey as possible
	int32_t order2 = getOrderOfKey_unlocked(collnum, endKey, maxKey);
	// how many recs?
	int32_t size = order2 - order1;
	// . if enough, return
	// . NOTE minKey/maxKey may be < or > startKey/endKey
	// . return an estimated list size
	if ( size > 200 ) return size * m_ks;
	// . otherwise, count exactly
	// . reset size and get the initial node
	size = 0;
	int32_t n = getPrevNode_unlocked(collnum, startKey);
	// return 0 if no nodes in that key range

	if( n < 0 ) {
		return 0;
	}

	// skip to next node if this one is < startKey
	if( KEYCMP(m_keys, n, startKey, 0, m_ks) < 0 ) {
		n = getNextNode_unlocked(n);
	}

	if( n < 0 ) {
		return 0;
	}
	
	// or collnum
	if ( m_collnums[n] < collnum ) {
		n = getNextNode_unlocked(n);
	}

	// loop until we run out of nodes or one breeches endKey
	while( n > 0 && KEYCMP(m_keys,n,endKey,0,m_ks) <= 0 && m_collnums[n]==collnum ) {
		size++;
		n = getNextNode_unlocked(n);
	}
	// this should be an exact list size (actually # of nodes)
	return size * m_ks;
}


bool RdbTree::collExists(collnum_t coll) const {
	ScopedLock sl(m_mtx);
	int32_t nn = getNextNode_unlocked(coll, KEYMIN());
	if(nn < 0)
		return false;
	if(getCollnum_unlocked(nn) != coll)
		return false;
	return true;
}

// we don't lock because variable is already atomic
bool RdbTree::isSaving() const {
	return m_isSaving;
}

bool RdbTree::needsSave() const {
	ScopedLock sl(m_mtx);
	return m_needsSave;
}

// . returns a number from 0 to m_numUsedNodes-1
// . represents the ordering of this key in that range
// . *retKey is the key that has the returned order
// . *retKey gets as close to "key" as it can
// . returns # of NODES
int32_t RdbTree::getOrderOfKey_unlocked(collnum_t collnum, const char *key, char *retKey) const {
	m_mtx.verify_is_locked();

	if ( m_numUsedNodes <= 0 ) return 0;
	int32_t i     = m_headNode;
	// estimate the depth of tree if not balanced
	int32_t d     = getTreeDepth_unlocked();
	// TODO: WARNING: ensure d-1 not >= 32 !!!!!!!!!!!!!!!!!
	int32_t step  = 1 << (d-1);
	int32_t order = step;
	while ( i != -1 ) {
		//if ( retKey ) *retKey = m_keys[i];
		if ( retKey ) KEYSET ( retKey , &m_keys[i*m_ks] , m_ks );
		step /= 2;
		if ( collnum < m_collnums[i] ||
		     (collnum==m_collnums[i] &&KEYCMP(key,0,m_keys,i,m_ks)<0)){
			i = m_left [i]; 
			if ( i >= 0 ) order -= step;
			continue;
		}
		if ( collnum > m_collnums[i] ||
		     (collnum==m_collnums[i] &&KEYCMP(key,0,m_keys,i,m_ks)>0)){
			i = m_right[i]; 
			if ( i >= 0 ) order += step;
			continue;
		}
		break;
        }
	// normalize order since tree probably has less then 2^d nodes
	int64_t normOrder = (int64_t)order * (int64_t)m_numUsedNodes / ( ((int64_t)1 << d)-1);
	return (int32_t) normOrder;
}

int32_t RdbTree::getTreeDepth_unlocked() const {
	m_mtx.verify_is_locked();
	return m_depth[m_headNode];
}


// . recompute depths of nodes starting at i and ascending the tree
// . call rotateRight/Left() when depth of children differs by 2 or more
void RdbTree::setDepths_unlocked(int32_t i) {
	m_mtx.verify_is_locked();

	// inc the depth of all parents if it changes for them
	while ( i >= 0 ) {
		// . compute the new depth for node i
		// . get depth of left kid
		// . left/rightDepth is depth of subtree on left/right
		int32_t leftDepth  = 0;
		int32_t rightDepth = 0;
		if ( m_left [i] >= 0 ) leftDepth  = m_depth [ m_left [i] ] ;
		if ( m_right[i] >= 0 ) rightDepth = m_depth [ m_right[i] ] ;
		// . get the new depth for node i
		// . add 1 cuz we include ourself in our m_depth
		int32_t newDepth ;
		if ( leftDepth > rightDepth ) newDepth = leftDepth  + 1;
		else                          newDepth = rightDepth + 1;
		// if the depth did not change for i then we're done
		int32_t oldDepth = m_depth[i] ;
		// set our new depth
		m_depth[i] = newDepth;
		// diff can be -2, -1, 0, +1 or +2
		int32_t diff = leftDepth - rightDepth;
		// . if it's -1, 0 or 1 then we don't need to balance
		// . if rightside is deeper rotate left, i is the pivot
		// . otherwise, rotate left
		// . these should set the m_depth[*] for all nodes needing it
		if      ( diff == -2 ) i = rotateLeft_unlocked(i);
		else if ( diff ==  2 ) i = rotateRight_unlocked(i);
		// . return if our depth was ultimately unchanged
		// . i may have change if we rotated, but same logic applies
		if ( m_depth[i] == oldDepth ) break;
		// debug msg
		//fprintf (stderr,"changed node %" PRId32"'s depth from %" PRId32" to %" PRId32"\n",
		//i,oldDepth,newDepth);
		// get his parent to continue the ascension
		i = m_parents [ i ];
	}
	// debug msg
	//printTree();
}

/*
// W , X and B are SUBTREES.
// B's subtree was 1 less in depth than W or X, then a new node was added to
// W or X triggering the imbalance.
// However, if B gets deleted W and X can be the same size.
//
// Right rotation if W subtree depth is >= X subtree depth:
//
//          A                N
//         / \              / \
//        /   \            /   \
//       N     B   --->   W     A
//      / \                    / \
//     W   X                  X   B
//
// Right rotation if W subtree depth is <  X subtree depth:
//          A                X
//         / \              / \
//        /   \            /   \
//       N     B   --->   N     A
//      / \              / \   / \
//     W   X            W   Q T   B
//        / \                 
//       Q   T               
*/
// . we come here when A's left subtree is deeper than it's right subtree by 2
// . this rotation operation causes left to lose 1 depth and right to gain one
// . the type of rotation depends on which subtree is deeper, W or X
// . W or X must deeper by the other by exactly one
// . if they were equal depth then how did adding a node inc the depth?
// . if their depths differ by 2 then N would have been rotated first!
// . the parameter "i" is the node # for A in the illustration above
// . return the node # that replaced A so the balance() routine can continue
// . TODO: check our depth modifications below
int32_t RdbTree::rotateRight_unlocked(int32_t i) {
	m_mtx.verify_is_locked();
	return rotate_unlocked(i, m_left, m_right);
}

// . i just swapped left with m_right
int32_t RdbTree::rotateLeft_unlocked(int32_t i) {
	m_mtx.verify_is_locked();
	return rotate_unlocked(i, m_right, m_left);
}

int32_t RdbTree::rotate_unlocked(int32_t i, int32_t *left, int32_t *right) {
	m_mtx.verify_is_locked();

	// i's left kid's right kid takes his place
	int32_t A = i;
	int32_t N = left  [ A ];
	int32_t W = left  [ N ];
	int32_t X = right [ N ];
	int32_t Q = -1;
	int32_t T = -1;
	if ( X >= 0 ) {
		Q = left  [ X ];
		T = right [ X ];
	}
	// let AP be A's parent
	int32_t AP = m_parents [ A ];
	// whose the bigger subtree, W or X? (depth includes W or X itself)
	int32_t Wdepth = 0;
	int32_t Xdepth = 0;
	if ( W >= 0 ) Wdepth = m_depth[W];
	if ( X >= 0 ) Xdepth = m_depth[X];

	// goto Xdeeper if X is deeper
	if ( Wdepth < Xdepth ) goto Xdeeper;
	// N's parent becomes A's parent
	m_parents [ N ] = AP;
	// A's parent becomes N
	m_parents [ A ] = N;
	// X's parent becomes A
	if ( X >= 0 ) m_parents [ X ] = A;
	// A's parents kid becomes N
	if ( AP >= 0 ) {
		if ( left [ AP ] == A ) left  [ AP ] = N;
		else                    right [ AP ] = N;
	}
	// if A had no parent, it was the headNode
	else {
		//fprintf(stderr,"changing head node from %" PRId32" to %" PRId32"\n",
		//m_headNode,N);
		m_headNode = N;
	}
	// N's right kid becomes A
	right [ N ] = A;
	// A's left  kid becomes X		
	left  [ A ] = X;
	// . compute A's depth from it's X and B kids
	// . it should be one less if Xdepth smaller than Wdepth
	// . might set m_depth[A] to computeDepth_unlocked(A) if we have problems
	if ( Xdepth < Wdepth ) m_depth [ A ] -= 2;
	else                   m_depth [ A ] -= 1;
	// N gains a depth iff W and X were of equal depth
	if ( Wdepth == Xdepth ) m_depth [ N ] += 1;
	// now we're done, return the new pivot that replaced A
	return N;
	// come here if X is deeper
 Xdeeper:
	// X's parent becomes A's parent
	m_parents [ X ] = AP;
	// A's parent becomes X
	m_parents [ A ] = X;
	// N's parent becomes X
	m_parents [ N ] = X;
	// Q's parent becomes N
	if ( Q >= 0 ) m_parents [ Q ] = N;
	// T's parent becomes A
	if ( T >= 0 ) m_parents [ T ] = A;
	// A's parent's kid becomes X
	if ( AP >= 0 ) {
		if ( left [ AP ] == A ) left  [ AP ] = X;
		else	                right [ AP ] = X;
	}
	// if A had no parent, it was the headNode
	else {
		//fprintf(stderr,"changing head node2 from %" PRId32" to %" PRId32"\n",
		//m_headNode,X);
		m_headNode = X;
	}
	// A's left     kid becomes T
	left  [ A ] = T;
	// N's right    kid becomes Q
	right [ N ] = Q;
	// X's left     kid becomes N
	left  [ X ] = N;
	// X's right    kid becomes A
	right [ X ] = A;
	// X's depth increases by 1 since it gained 1 level of 2 new kids
	m_depth [ X ] += 1;
	// N's depth decreases by 1
	m_depth [ N ] -= 1;
	// A's depth decreases by 2
	m_depth [ A ] -= 2; 
	// now we're done, return the new pivot that replaced A
	return X;
}

// . depth of subtree with i as the head node
// . includes i, so minimal depth is 1
int32_t RdbTree::computeDepth_unlocked(int32_t i) const {
	m_mtx.verify_is_locked();

	int32_t leftDepth  = 0;
	int32_t rightDepth = 0;
	if ( m_left [i] >= 0 ) leftDepth  = m_depth [ m_left [i] ] ;
	if ( m_right[i] >= 0 ) rightDepth = m_depth [ m_right[i] ] ;
	// . get the new depth for node i
	// . add 1 cuz we include ourself in our m_depth
	if ( leftDepth > rightDepth ) return leftDepth  + 1;
	else                          return rightDepth + 1;  
}

int32_t RdbTree::getMemAllocated() const {
	ScopedLock sl(m_mtx);
	return m_memAllocated;
}

int32_t RdbTree::getMemOccupied() const {
	ScopedLock sl(m_mtx);
	return m_memOccupied;
}

bool RdbTree::is90PercentFull() const {
	ScopedLock sl(m_mtx);

	// . m_memOccupied is amount of alloc'd mem that data occupies
	// . now we /90 and /100 since multiplying overflowed
	return ( m_numUsedNodes/90 >= m_numNodes/100 );
}

#define BLOCK_SIZE 10000

// . caller should call f->set() himself
// . we'll open it here
// . returns false if blocked, true otherwise
// . sets g_errno on error
bool RdbTree::fastSave(const char *dir, bool useThread, void *state, void (*callback)(void *)) {
	ScopedLock sl(m_mtx);

	logTrace(g_conf.m_logTraceRdbTree, "BEGIN. dir=%s", dir);

	if (g_conf.m_readOnlyMode) {
		logTrace(g_conf.m_logTraceRdbTree, "END. Read only mode. Returning true.");
		return true;
	}

	// we do not need a save
	if (!m_needsSave) {
		logTrace(g_conf.m_logTraceRdbTree, "END. Don't need to save. Returning true.");
		return true;
	}

	// return true if already in the middle of saving
	bool isSaving = m_isSaving.exchange(true);
	if (isSaving) {
		logTrace(g_conf.m_logTraceRdbTree, "END. Is already saving. Returning false.");
		return false;
	}

	// note it
	logf(LOG_INFO, "db: Saving %s%s-saved.dat", dir, m_dbname);

	// save parms
	strncpy(m_dir, dir, sizeof(m_dir)-1);
	m_dir[sizeof(m_dir) - 1] = '\0';

	m_state    = state;
	m_callback = callback;

	// assume no error
	m_errno = 0;

	if (useThread) {
		// make this a thread now
		if (g_jobScheduler.submit(saveWrapper, saveDoneWrapper, this, thread_type_unspecified_io, 1/*niceness*/)) {
			return false;
		}

		// if it failed
		if (g_jobScheduler.are_new_jobs_allowed()) {
			log(LOG_WARN, "db: Thread creation failed. Blocking while saving tree. Hurts performance.");
		}
	}

	sl.unlock();

	// no threads
	saveWrapper(this);
	saveDoneWrapper(this, job_exit_normal);

	logTrace(g_conf.m_logTraceRdbTree, "END. Returning true.");

	// we did not block
	return true;
}

void RdbTree::saveWrapper ( void *state ) {
	logTrace(g_conf.m_logTraceRdbTree, "BEGIN");

	// get this class
	RdbTree *that = (RdbTree *)state;

	ScopedLock sl(that->getLock());

	// assume no error since we're at the start of thread call
	that->m_errno = 0;

	// this returns false and sets g_errno on error
	that->fastSave_unlocked();

	if (g_errno && !that->m_errno) {
		that->m_errno = g_errno;
	}

	if (that->m_errno) {
		log(LOG_ERROR, "db: Had error saving tree to disk for %s: %s.", that->m_dbname, mstrerror(that->m_errno));
	} else {
		log(LOG_INFO, "db: Done saving %s with %" PRId32" keys (%" PRId64" bytes)",
		    that->m_dbname, that->m_numUsedNodes, that->m_bytesWritten);
	}

	// . resume adding to the tree
	// . this will also allow other threads to be queued
	// . if we did this at the end of the thread we could end up with
	//   an overflow of queued SAVETHREADs
	that->m_isSaving = false;

	// we do not need to be saved now?
	that->m_needsSave = false;

	logTrace(g_conf.m_logTraceRdbTree, "END");
}

/// @todo ALC cater for when exit_type != job_exit_normal
// we come here after thread exits
void RdbTree::saveDoneWrapper(void *state, job_exit_t exit_type) {
	logTrace(g_conf.m_logTraceRdbTree, "BEGIN");

	// get this class
	RdbTree *that = (RdbTree *)state;

	// store save error into g_errno
	g_errno = that->m_errno;

	// . call callback
	if ( that->m_callback ) {
		that->m_callback ( that->m_state );
	}

	logTrace(g_conf.m_logTraceRdbTree, "END");
}

// . returns false and sets g_errno on error
// . NO USING g_errno IN A DAMN THREAD!!!!!!!!!!!!!!!!!!!!!!!!!
bool RdbTree::fastSave_unlocked() {
	m_mtx.verify_is_locked();

	if ( g_conf.m_readOnlyMode ) return true;

	/// @todo ALC we should probably use BigFile now. don't think g_errno being messed up is a good reason
	// cannot use the BigFile class, since we may be in a thread and it messes with g_errno
	char s[1024];
	sprintf ( s , "%s/%s-saving.dat", m_dir , m_dbname );
	int fd = ::open ( s , O_RDWR | O_CREAT | O_TRUNC , getFileCreationFlags() );
	if ( fd < 0 ) {
		m_errno = errno;
		log( LOG_ERROR, "db: Could not open %s for writing: %s.", s, mstrerror( errno ) );
		return false;
	}

	// verify the tree
	if (g_conf.m_verifyWrites) {
		for (;;) {
			log("db: verify writes is enabled, checking tree before saving.");
			if (!checkTree_unlocked(false, true)) {
				log(LOG_WARN, "db: fixing tree and re-checking");
				fixTree_unlocked();
				continue;
			}
			break;
		}
	}


	// clear our own errno
	errno = 0;
	// . save the header
	// . force file head to the 0 byte in case offset was elsewhere
	int64_t offset = 0;
	int64_t br = 0;
	static const bool doBalancing = true;
	br += pwrite ( fd , &m_numNodes       , 4 , offset ); offset += 4;
	br += pwrite ( fd , &m_fixedDataSize  , 4 , offset ); offset += 4;
	br += pwrite ( fd , &m_numUsedNodes   , 4 , offset ); offset += 4;
	br += pwrite ( fd , &m_headNode       , 4 , offset ); offset += 4;
	br += pwrite ( fd , &m_nextNode       , 4 , offset ); offset += 4;
	br += pwrite ( fd , &m_minUnusedNode  , 4 , offset ); offset += 4;
	br += pwrite ( fd , &doBalancing   , sizeof(doBalancing) , offset);
	offset += sizeof(doBalancing);
	br += pwrite ( fd , &m_ownData       , sizeof(m_ownData)     , offset);
	offset += sizeof(m_ownData);
	// bitch on error
	if ( br != offset ) {
		m_errno = errno;
		close ( fd );
		log(LOG_WARN, "db: Failed to save tree1 for %s: %s.", m_dbname,mstrerror(errno));
		return false;
	}
	// position to store into m_keys, ...
	int32_t start = 0;
	// save tree in block units
	while ( start < m_minUnusedNode ) {
		// . returns number of nodes, starting at node #i, saved
		// . returns -1 and sets errno on error
		int32_t bytesWritten = fastSaveBlock_unlocked(fd, start, offset) ;
		// returns -1 on error
		if ( bytesWritten < 0 ) {
			close ( fd );
			m_errno = errno;
			log(LOG_WARN, "db: Failed to save tree2 for %s: %s.", m_dbname,mstrerror(errno));
			return false;
		}
		// point to next block to save to
		start += BLOCK_SIZE;
		// and advance the file offset
		offset += bytesWritten;
	}
	// remember total bytes written
	m_bytesWritten = offset;
	// close it up
	close ( fd );

	// now rename it
	char s2[1024];
	sprintf ( s2 , "%s/%s-saved.dat", m_dir , m_dbname );
	if( ::rename(s, s2) == -1 ) {
		logError("Error renaming file [%s] to [%s] (%d: %s)", s, s2, errno, mstrerror(errno));
	}

	return true;
}

// return bytes written
int32_t RdbTree::fastSaveBlock_unlocked(int fd, int32_t start, int64_t offset) {
	m_mtx.verify_is_locked();

	// save offset
	int64_t oldOffset = offset;
	// . just save each one right out, even if empty
	//   because the empty's have a linked list in m_right[]
	// . set # n
	int32_t n = BLOCK_SIZE;
	// don't over do it
	if ( start + n > m_minUnusedNode ) n = m_minUnusedNode - start;

	errno = 0;
	int64_t br = 0;
	// write the block
	br += pwrite ( fd,&m_collnums[start], n * sizeof(collnum_t) , offset );
	offset += n * sizeof(collnum_t);
	br += pwrite ( fd , &m_keys   [start*m_ks] , n * m_ks , offset );
	offset += n * m_ks;
	br += pwrite(fd, &m_left   [start] , n * 4 , offset ); offset += n * 4;
	br += pwrite(fd, &m_right  [start] , n * 4 , offset ); offset += n * 4;
	br += pwrite(fd, &m_parents[start] , n * 4 , offset ); offset += n * 4;
	br += pwrite ( fd , &m_depth[start] , n  , offset ); offset += n  ;
	if ( m_fixedDataSize == -1 ) {
	  br += pwrite ( fd , &m_sizes[start] , n*4, offset ); offset += n*4; }
	// bitch on error
	if ( br != offset - oldOffset ) {
		log(LOG_WARN, "db: Failed to save tree3 for %s (%" PRId64"!=%" PRId64"): %s.",
		    m_dbname, br, offset, mstrerror(errno));
		return -1;
	}

	// if no data to write then return bytes written this call
	if ( m_fixedDataSize == 0 ) return offset - oldOffset ;

	// debug count
	//int32_t count = 0;
	// define ending node for all loops
	int32_t end = start + n ;
	// now we have to dump out all the records
	for ( int32_t i = start ; i < end ; i++ ) {
		// skip if empty
		if ( m_parents[i] == -2 ) continue;
		// write variable sized nodes
		if ( m_fixedDataSize == -1 ) {
			if ( m_sizes[i] <= 0 ) continue;
			pwrite ( fd , m_data[i] , m_sizes[i] , offset );
			offset += m_sizes[i];
			continue;
		}
		// write fixed sized nodes
		pwrite (  fd , m_data[i] , m_fixedDataSize , offset );
		offset += m_fixedDataSize;
	}

	// return bytes written
	return offset - oldOffset;
}

// . caller should call f->set() himself
// . we'll open it here
// . returns false and sets g_errno on error (sometimes g_errno not set)
bool RdbTree::fastLoad(BigFile *f, RdbMem *stack) {
	ScopedLock sl(m_mtx);

	log( LOG_INIT, "db: Loading %s.", f->getFilename() );

	// open it up
	if ( ! f->open ( O_RDONLY ) ) {
		log( LOG_ERROR, "db: open failed" );
		return false;
	}

	int32_t fsize = f->getFileSize();
	// init offset
	int64_t offset = 0;
	// 16 byte header
	int32_t header = 4*6 + 1 + sizeof(m_ownData);
	// file size must be a min of "header"
	if ( fsize < header ) {
		f->close();
		g_errno=EBADFILE;
		log( LOG_ERROR, "db: file size smaller than header" );
		return false;
	}

	// get # of nodes in the tree
	int32_t n , fixedDataSize , numUsedNodes ;
	bool doBalancing , ownData ;
	int32_t headNode , nextNode , minUnusedNode;
	// force file head to the 0 byte in case offset was elsewhere
	f->read  ( &n              , 4 , offset ); offset += 4;
	f->read  ( &fixedDataSize  , 4 , offset ); offset += 4;
	f->read  ( &numUsedNodes   , 4 , offset ); offset += 4;
	f->read  ( &headNode       , 4 , offset ); offset += 4;
	f->read  ( &nextNode       , 4 , offset ); offset += 4;
	f->read  ( &minUnusedNode  , 4 , offset ); offset += 4;
	f->read  ( &doBalancing    , sizeof(doBalancing) , offset );
	offset += sizeof(doBalancing);
	f->read  ( &ownData        , sizeof(m_ownData    ) , offset ) ; 
	offset += sizeof(m_ownData);
	// return false on read error
	if ( g_errno ) {
		f->close();
		log( LOG_ERROR, "db: read error: %s", mstrerror( g_errno ) );
		return false;
	}
	// parms check
	if ( m_fixedDataSize != fixedDataSize || 
	     !doBalancing   ||
	     m_ownData       != ownData        ) {
		f->close();
		log(LOG_LOGIC,"db: rdbtree: fastload: Bad parms. File "
			   "may be corrupt or a key attribute was changed in "
			   "the code and is not reflected in this file.");
		return false;
	}
	// make sure size it right again
	int32_t nodeSize    = (sizeof(collnum_t)+m_ks+4+4+4);
	int32_t minFileSize = header + minUnusedNode * nodeSize;
	minFileSize += minUnusedNode     ;
	if ( fixedDataSize == -1 ) minFileSize += minUnusedNode * 4 ;
	//if ( fixedDataSize > 0 ) minFileSize += minUnusedNode *fixedDataSize;
	// if no data, sizes much match exactly
	if ( fixedDataSize == 0 && fsize != minFileSize ) {
		g_errno = EBADFILE;
		log( LOG_ERROR, "db: File size of %s is %" PRId32", should be %" PRId32". File may be corrupted.",
		     f->getFilename(),fsize,minFileSize);
		f->close();
		return false;
	}
	// does it fit?
	if ( fsize < minFileSize ) {
		g_errno = EBADFILE;
		log( LOG_ERROR, "db: File size of %s is %" PRId32", should >= %" PRId32". File may be corrupted.",
		     f->getFilename(),fsize,minFileSize);
		f->close();
		return false;
	}

	// make room if we don't have any
	if ( m_numNodes < minUnusedNode ) {
		log( LOG_INIT, "db: Growing tree to make room for %s", f->getFilename() );
		if (!growTree_unlocked(minUnusedNode)) {
			f->close();
			log( LOG_ERROR, "db: Failed to grow tree" );
			return false;
		}
	}
	// we'll read this many
	int32_t start = 0;

	// reset corruption count
	m_corrupt = 0;

	// read block by block
	while ( start < minUnusedNode ) {
		// . returns next place to start scan
		// . incs m_numPositive/NegativeKeys and m_numUsedNodes 
		// . incs m_memAllocated and m_memOccupied
		int32_t bytesRead = fastLoadBlock_unlocked(f, start, minUnusedNode, stack, offset) ;
		if ( bytesRead < 0 ) {
			f->close();
			g_errno = errno;
			return false;
		}
		// inc the start
		start += BLOCK_SIZE;
		// and the offset
		offset += bytesRead;
	}

	// print corruption
	if ( m_corrupt ) {
		log( LOG_WARN, "admin: Loaded %" PRId32" corrupted recs in tree for %s.", m_corrupt, m_dbname );
	}

	// remember total bytes read
	m_bytesRead = offset;
	// set these
	m_headNode      = headNode;
	m_nextNode      = nextNode;
	m_minUnusedNode = minUnusedNode;

	// check it
	if ( !checkTree_unlocked(false, true) ) {
		return fixTree_unlocked();
	}

	// no longer needs save
	m_needsSave = false;

	return true;
}

// . return bytes loaded
// . returns -1 and sets g_errno on error
int32_t RdbTree::fastLoadBlock_unlocked(BigFile *f, int32_t start, int32_t totalNodes, RdbMem *stack, int64_t offset) {
	m_mtx.verify_is_locked();

	// set # ndoes to read
	int32_t n = totalNodes - start;
	if ( n > BLOCK_SIZE ) n = BLOCK_SIZE;
	// debug msg
	//log("reading block at %" PRId64", %" PRId32" nodes",
	//     f->m_currentOffset, n );
	int64_t oldOffset = offset;
	// . copy them in
	// . start reading at beginning of file
	f->read ( &m_collnums[start], n * sizeof(collnum_t) , offset ); 
	offset += n * sizeof(collnum_t);
	f->read ( &m_keys   [start*m_ks] , n * m_ks , offset ); 
	offset += n * m_ks;
	f->read ( &m_left   [start] , n * 4 , offset ); offset += n * 4;
	f->read ( &m_right  [start] , n * 4 , offset ); offset += n * 4;
	f->read ( &m_parents[start] , n * 4 , offset ); offset += n * 4;
	f->read ( &m_depth[start] , n , offset    ); offset += n;
	if ( m_fixedDataSize == -1 ) {
		f->read ( &m_sizes[start] , n * 4 , offset); offset += n * 4; }
	// return false on read error
	if ( g_errno ) {
		log( LOG_ERROR, "db: Failed to read %s: %s.", f->getFilename(), mstrerror(g_errno) );
		return -1;
	}
	// get valid collnum ranges
	int32_t max  = g_collectiondb.getNumRecs();
	// sanity check
	//if ( max >= MAX_COLLS ) { g_process.shutdownAbort(true); }
	// define ending node for all loops
	int32_t end = start + n ;
	// store into tree in the appropriate nodes
	for ( int32_t i = start ; i < end ; i++ ) {
		// skip if empty
		if ( m_parents[i] == -2 ) continue;
		// watch out for bad collnums... corruption...
		collnum_t c = m_collnums[i];
		if ( c < 0 || c >= max ) {
			m_corrupt++;
			continue;
		}
		// must have rec as well. unless it its statsdb tree
		// or m_waitingTree which are collection-less and always use
		// 0 for their collnum. if collection-less m_rdbId==-1.
		if ( ! g_collectiondb.getRec(c) && m_rdbId >= 0 ) {
			m_corrupt++;
			continue;
		}
		// keep a tally on all this
		m_memOccupied += m_overhead;
		increaseNodeCount_unlocked(c, getKey_unlocked(i));
	}
	// bail now if we can 
	if ( m_fixedDataSize == 0 ) return offset - oldOffset ;
	// how much should we read?
	int32_t bufSize = 0;
	if ( m_fixedDataSize == -1 ) {
		for ( int32_t i = start ; i < end ; i++ ) 
			if ( m_parents[i] != -2 ) bufSize += m_sizes[i];
	}
	else if ( m_fixedDataSize > 0 ) {
		for ( int32_t i = start ; i < end ; i++ ) 
			if ( m_parents[i] != -2 ) bufSize += m_fixedDataSize;
	}
	// get space
	char *buf = (char *) stack->allocData(bufSize);
	if ( ! buf ) {
	        log( LOG_ERROR, "db: Failed to allocate %" PRId32" bytes to read %s. Increase tree size for it in gb.conf.",
	             bufSize,f->getFilename());
		return -1;
	}

	// debug
	//log("reading %" PRId32" bytes of raw rec data", bufSize );
	// establish end point
	char *bufEnd = buf + bufSize;
	// . read all into that buf
	// . this should block since callback is NULL
	f->read ( buf , bufSize , offset ) ;
	// return false on read error
	if ( g_errno ) return -1;
	// advance file offset
	offset += bufSize;
	// part it out now
	int32_t  size = m_fixedDataSize;
	for ( int32_t i = start ; i < end ; i++ ) {
		// skip unused
		if ( m_parents[i] == -2 ) continue;
		// get size of his data if it's variable
		if ( m_fixedDataSize == -1 ) size = m_sizes[i];
		// ensure we have the room
		if ( buf + size > bufEnd ) {
			g_errno = EBADFILE;
			log( LOG_ERROR, "db: Encountered record with corrupted size parameter of %" PRId32" in %s.",
			     size, f->getFilename() );
			return -1;
		}
		m_data[i]  = buf;
		buf       += size;
		// update these
		m_memAllocated  += size;
		m_memOccupied += size;
	}
	return offset - oldOffset ;
}
// . caller should call f->set() himself
// . we'll open it here
// . returns false and sets g_errno on error (sometimes g_errno not set)


void RdbTree::cleanTree() {
	ScopedLock sl(m_mtx);

	// some trees always use 0 for all node collnum_t's like
	// statsdb, waiting tree etc.
	if ( m_rdbId < 0 ) return;

	// the liberation count
	int32_t count = 0;
	collnum_t collnum;
	int32_t max = g_collectiondb.getNumRecs();

	for ( int32_t i = 0 ; i < m_minUnusedNode ; i++ ) {
		// skip node if parents is -2 (unoccupied)
		if ( m_parents[i] == -2 ) continue;
		// is collnum valid?
		if ( m_collnums[i] >= 0   &&
		     m_collnums[i] <  max &&
		     g_collectiondb.getRec(m_collnums[i]) ) continue;
		// if it is negtiave, remove it, that is wierd corruption
		if ( m_collnums[i] < 0 )
			deleteNode_unlocked(i, true);
		// remove it otherwise
		// don't actually remove it!!!! in case collection gets
		// moved accidentally.
		// no... otherwise it can clog up the tree forever!!!!
		deleteNode_unlocked(i, true);
		count++;
		// save it
		collnum = m_collnums[i];
	}

	// print it
	if ( count == 0 ) return;
	log(LOG_LOGIC,"db: Removed %" PRId32" records from %s tree for invalid "
	    "collection number %i.",count,m_dbname,collnum);
	//log(LOG_LOGIC,"db: Records not actually removed for safety. Except "
	//    "for those with negative colnums.");
	static bool s_print = true;
	if ( ! s_print ) return;
	s_print = false;
	log (LOG_LOGIC,"db: This is bad. Did you remove a collection "
	     "subdirectory? Don't do that, you should use the \"delete "
	     "collections\" interface because it also removes records from "
	     "memory, too.");
}

bool RdbTree::isEmpty() const {
	ScopedLock sl(m_mtx);
	return isEmpty_unlocked();
}

bool RdbTree::isEmpty_unlocked() const {
	m_mtx.verify_is_locked();
	return (m_numUsedNodes == 0);
}

int32_t RdbTree::getNumNodes() const {
	ScopedLock sl(m_mtx);
	return getNumNodes_unlocked();
}

int32_t RdbTree::getNumNodes_unlocked() const {
	m_mtx.verify_is_locked();
	return m_minUnusedNode;
}

int32_t RdbTree::getNumUsedNodes() const {
	ScopedLock sl(m_mtx);
	return getNumUsedNodes_unlocked();
}

int32_t RdbTree::getNumUsedNodes_unlocked() const {
	m_mtx.verify_is_locked();
	return m_numUsedNodes;
}

int32_t  RdbTree::getNumAvailNodes() const {
	ScopedLock sl(m_mtx);
	return m_numNodes - m_numUsedNodes;
}

int32_t RdbTree::getNumNegativeKeys() const {
	ScopedLock sl(m_mtx);
	return m_numNegativeKeys;
}

int32_t RdbTree::getNumPositiveKeys() const {
	ScopedLock sl(m_mtx);
	return m_numPositiveKeys;
}

int32_t RdbTree::getNumNegativeKeys(collnum_t collnum) const {
	ScopedLock sl(m_mtx);
	// fix for collectionless rdbs
	if ( m_rdbId < 0 ) return m_numNegativeKeys;

	/// @todo ALC thread safety?
	CollectionRec *cr = g_collectiondb.getRec(collnum);
	if ( ! cr ) return 0;
	return cr->m_numNegKeysInTree[(unsigned char)m_rdbId]; 
}

int32_t RdbTree::getNumPositiveKeys(collnum_t collnum) const {
	ScopedLock sl(m_mtx);
	// fix for collectionless rdbs
	if ( m_rdbId < 0 ) return m_numPositiveKeys;

	/// @todo ALC thread safety?
	CollectionRec *cr = g_collectiondb.getRec(collnum);
	if ( ! cr ) return 0;
	return cr->m_numPosKeysInTree[(unsigned char)m_rdbId]; 
}

void RdbTree::printTree(std::function<void(rdbid_t, const char *)> print_fn) const {
	for ( int32_t i = 0 ; i < m_minUnusedNode ; i++ ) {
		// skip node if parents is -2 (unoccupied)
		if ( m_parents[i] == -2 ) continue;

		if (print_fn) {
			print_fn((rdbid_t )m_rdbId, &m_keys[i*m_ks]);
		} else {
			logf(LOG_TRACE, "db: i=%04" PRId32 " k=%s keySize=%" PRId32 "",
			     i, KEYSTR(&m_keys[i*m_ks], m_ks), m_ks);
		}
	}
}
