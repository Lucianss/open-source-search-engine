#include "RdbScan.h"
#include "Rdb.h"
#include "Process.h"
#include "Mem.h"
#include "Errno.h"


// . readset up for a scan of slots in the RdbScans
// . returns false if blocked, true otherwise
// . sets errno on error
bool RdbScan::setRead ( BigFile  *file         ,
			int32_t      fixedDataSize,
			int64_t offset       ,
			int32_t      bytesToRead  ,
			char     *startKey     , 
			char     *endKey       ,
			char      keySize      ,
			RdbList  *list         , // we fill this up
			void     *state        ,
			void   (* callback) ( void *state ) ,
			bool      useHalfKeys  ,
			rdbid_t   rdbId ,
			int32_t      niceness     ,
			bool      hitDisk        ) {
	// remember list
	m_rdblist = list;
	// reset the list
	m_rdblist->reset();
	// save keySize
	m_ks = keySize;
	m_rdbId = rdbId;
	m_hitDisk        = hitDisk;
	// set list now
	m_rdblist->set(NULL, 0, NULL, 0, startKey, endKey, fixedDataSize, true, useHalfKeys, keySize);

	// . don't do anything if startKey exceeds endKey
	// . often Msg3 will call us with this true because it's page range
	//   is empty because the map knows without having to hit disk. 
	//   therefore, just return silently now.
	// . Msg3 will not merge empty lists so don't worry about setting the
	//   lists startKey/endKey
	if ( KEYCMP(startKey,endKey,m_ks)>0 ) return true;
	// don't bother doing anything if nothing needs to be read
	if ( bytesToRead == 0 ) return true;

	// . start reading at m_offset in the file
	// . also, remember this offset for finding the offset of the last key
	//   to set a tighter m_bufEnd in doneReading() so we don't have to
	//   keep checking if the returned record's key falls exactly in
	//   [m_startKey,m_endKey]
	// . set m_bufSize to how many bytes we need to read
	// . m_keyMin is the first key we read, may be < startKey
	// . we won't read any keys strictly greater than "m_keyMax"
	// . m_hint is set to the offset of the BIGGEST key found in the map
	//   that is still <= endKey
	// . we use m_hint so that RdbList::merge() can find the last key
	//   in the startKey/endKey range w/o having to step through
	//   all the records in the read
	// . m_hint will limit the stepping to a PAGE_SIZE worth of records
	// . m_hint is an offset, like m_offset
	// . TODO: what if it returns false?

	// . alloc some read buffer space, m_buf
	// . add 4 extra in case first key is half key and needs to be full
	int32_t bufSize = bytesToRead ;
	// add 6 more if we use half keys
	if ( useHalfKeys ) m_off = 6;
	else               m_off = 0;
	// posdb keys are 18 bytes but can be 12 ot 6 bytes compressed
	if ( m_rdbId == RDB_POSDB || m_rdbId == RDB2_POSDB2  ) m_off = 12;
	// alloc more for expanding the first 6-byte key into 12 bytes,
	// or in the case of posdb, expanding a 6 byte key into 18 bytes
	bufSize += m_off;
	// . and a little extra in case read() reads TOO much
	// . i think a read overflow might be causing a segv in malloc
	// . but try badding under us, maybe read() writes before the buf
	int32_t pad = 16;
	bufSize += pad;
	// . set up the list
	// . set min/max keys on list if we're done reading
	// . the min/maxKey defines the range of keys we read
	// . m_hint is the offset of the BIGGEST key in the map that is
	//   still <= the m_endKey specified in setRead()
	// . it's used to make it easy to find the actual biggest key that is
	//   <= m_endKey
	// save caller's callback
	m_callback = callback;
	m_state    = state;
	// save the first key in the list
	KEYSET(m_startKey,startKey,m_ks);//m_rdblist->m_ks);
	KEYSET(m_endKey,endKey,m_ks);
	m_fixedDataSize = fixedDataSize;
	m_useHalfKeys   = useHalfKeys;
	m_bytesToRead   = bytesToRead;
	// save file and offset for sanity check
	m_file   = file;
	m_offset = offset;
	// ensure we don't mess around
	m_fstate.m_allocBuf = NULL;
	m_fstate.m_buf      = NULL;

	// . do a threaded, non-blocking read 
	// . we now pass in a NULL buffer so Threads.cpp will do the
	//   allocation right before launching the thread so we don't waste
	//   memory. i've seen like 19000 unlaunched threads each allocating
	//   32KB for a tfndb read, hogging up all the memory.
	if ( ! file->read ( NULL, bytesToRead, offset, &m_fstate,
	                    callback ? this : NULL,
			    callback ? gotListWrapper0 : NULL,
			    niceness,
	                    pad + m_off )) // allocOff, buf offset to read into
		return false;

	if ( m_fstate.m_errno && ! g_errno ) {
		g_process.shutdownAbort(true);
	}

	// fix the list if we need to
	gotList();

	// we did not block
	return true;
}


void RdbScan::gotListWrapper0(void *state) {
	RdbScan *that = static_cast<RdbScan*>(state);
	that->gotListWrapper();
}

void RdbScan::gotListWrapper() {
	gotList();
	// let caller know we're done
	m_callback(m_state);
}


void RdbScan::gotList ( ) {
	char *allocBuf  = m_fstate.m_allocBuf;
	int32_t  allocOff  = m_fstate.m_allocOff; //buf=allocBuf+allocOff
	int32_t  allocSize = m_fstate.m_allocSize;

	// just return on error, do nothing
	if ( g_errno || m_fstate.m_errno ) {
		// free buffer though!! don't forget!
		if ( allocBuf ) {
			mfree( allocBuf, allocSize, "RdbScan" );
		}
		m_fstate.m_allocBuf = NULL;
		m_fstate.m_allocSize = 0;
		return;
	}

	// . set our list here now since the buffer was allocated in
	//   DiskPageCache.cpp or Threads.cpp to save memory.
	// . only set the list if there was a buffer. if not, it s probably 
	//   due to a failed alloc and we'll just end up using the empty
	//   m_rdblist we set way above.
	if ( m_fstate.m_allocBuf ) {
		int32_t  bytesDone = m_fstate.m_bytesDone;
		// sanity checks
		if ( bytesDone > allocSize                 ) { 
			g_process.shutdownAbort(true); }
		if ( allocOff + m_bytesToRead != allocSize ) { 
			g_process.shutdownAbort(true); }
		if ( allocOff != m_off + 16                ) { 
			g_process.shutdownAbort(true); }
		// now set this list. this always succeeds.
		m_rdblist->set ( allocBuf + allocOff , // buf + pad + m_off , 
			      m_bytesToRead   , // bytesToRead   , 
			      allocBuf        ,
			      allocSize       ,
			      m_startKey      ,
			      m_endKey        ,
			      m_fixedDataSize ,
			      true            , // ownData?
			      m_useHalfKeys   , 
			      m_ks            );
	}

	// assume we did not shift it
	m_shifted = 0;
	// if we were doing a cache only read, and got nothing, bail now
	if ( ! m_hitDisk && m_rdblist->isEmpty() ) return;
	// if first key in list is half, make it full
	char *p = m_rdblist->getList();
	
	//@@@ BR: Cores seen here. Added NULL-check. For now dump core until 
	// we figure out why this is happening.
	if( !p )
	{
		log(LOG_ERROR,"%s:%d: Returned list is NULL", __FILE__, __LINE__);
		g_process.shutdownAbort(true);
	}

	// . bitch if we read too much!
	// . i think a read overflow might be causing a segv in malloc
	// . NOTE: BigFile's call to DiskPageCache alters these values
	if ( m_fstate.m_bytesDone != m_fstate.m_bytesToGo && m_hitDisk )
		log(LOG_INFO,"disk: Read %" PRId64" bytes but needed %" PRId64".", m_fstate.m_bytesDone , m_fstate.m_bytesToGo );
		
	// bail if we don't do the 6 byte thing
	if ( m_off == 0 ) return;
	// posdb double compression?
	if ( (m_rdbId == RDB_POSDB || m_rdbId == RDB2_POSDB2)
	     && (p[0] & 0x04) ) {
		// make it full
		m_rdblist->setList(m_rdblist->getList() - 12);
		m_rdblist->setListSize(m_rdblist->getListSize() + 12);
		p                  -= 12;
		KEYSET(p,m_startKey,m_rdblist->getKeySize());
		// clear the compression bits
		*p &= 0xf9;
		// let em know we shifted it so they can shift the hint offset
		// up by 6
		m_shifted = 12;
	}
	// if first key is already full (12 bytes) no need to do anything
	else if ( RdbList::isHalfBitOn ( p ) ) {
		// otherwise, make it full
		m_rdblist->setList(m_rdblist->getList() - 6);
		m_rdblist->setListSize(m_rdblist->getListSize() + 6);
		p                  -= 6;
		KEYSET(p,m_startKey,m_rdblist->getKeySize());
		// clear the half bit in case it is set
		*p &= 0xfd;
		// let em know we shifted it so they can shift the hint offset
		// up by 6
		m_shifted = 6; // true;
	}
}
