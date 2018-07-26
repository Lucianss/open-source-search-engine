#include "RdbMap.h"
#include "BigFile.h"
#include "Process.h"
#include "BitOperations.h"
#include "File.h"
#include "Conf.h"
#include "Mem.h"
#include "Errno.h"
#include "hash.h"
#include <fcntl.h>
#include <unistd.h>


RdbMap::RdbMap() {
	m_numSegments = 0;
	m_numSegmentPtrs = 0;
	m_numSegmentOffs = 0;
	m_newPagesPerSegment = 0;
	m_keys = NULL;
	m_offsets = NULL;

	// Coverity	
	m_fixedDataSize = 0;
	m_useHalfKeys = false;
	m_ks = 0;
	m_pageSize = 0;
	m_pageSizeBits = 0;

	reset();
}

// dont save map on deletion!
RdbMap::~RdbMap() {
	reset();
}

void RdbMap::set ( const char *dir, const char *mapFilename,
		   int32_t fixedDataSize , bool useHalfKeys , char keySize ,
		   int32_t pageSize ) {
	logTrace( g_conf.m_logTraceRdbMap, "BEGIN. dir [%s], mapFilename [%s]", dir, mapFilename );

	reset();
	m_fixedDataSize = fixedDataSize;
	m_file.set ( dir , mapFilename );
	m_useHalfKeys = useHalfKeys;
	m_ks = keySize;
	m_pageSize = pageSize;
	m_pageSizeBits = getNumBitsOn32(pageSize-1);

	// m_pageSize -1 must be able to be stored in m_offsets[][] (a int16_t)
	if ( m_pageSize > 32768 ) {
	      log(LOG_LOGIC,"db: rdbmap: m_pageSize too big for m_offsets.");
	      g_process.shutdownAbort(true);
	}

	// . we remove the head part files of a BigFile when merging it
	// . this keeps the required merge space down to a small amount
	// . when we chop off a part file from a BigFile we must also
	//   chop off the corresponding segments in the map
	// . the match must be EXACT
	// . therefore, PAGES_PER_SEGMENT * m_pageSize must evenly divide
	//   MAX_PART_SIZE #define'd in BigFile.h
	if ( (MAX_PART_SIZE % (PAGES_PER_SEGMENT*m_pageSize)) == 0 ) {
		logTrace( g_conf.m_logTraceRdbMap, "END" );
		return;
	}

	log(LOG_LOGIC,"db: rdbmap: PAGES_PER_SEGMENT*"
	    "m_pageSize does not divide MAX_PART_SIZE.  cannot do "
	    "space-saving merges due to this.");
	g_process.shutdownAbort(true);
}


bool RdbMap::close ( bool urgent ) {
	bool status = true;
	if ( m_needToWrite ) {
		status = writeMap(false);
	}

	// clears and frees everything
	if ( ! urgent ) {
		reset ();
	}

	return status;
}


void RdbMap::reset ( ) {
	m_reducedMem = false;
	int32_t pps = PAGES_PER_SEGMENT;
	if ( m_newPagesPerSegment > 0 ) {
		pps = m_newPagesPerSegment;
	}

	for ( int32_t i = 0 ; i < m_numSegments; i++ ) {
		mfree(m_keys[i],m_ks *pps,"RdbMap");
		mfree(m_offsets[i], 2*pps,"RdbMap");
		// set to NULL so we know if accessed illegally
		m_keys   [i] = NULL;
		m_offsets[i] = NULL;
	}

	// the ptrs themselves are now a dynamic array to save mem
	// when we have thousands of collections
	mfree(m_keys,m_numSegmentPtrs*sizeof(char *),"MapPtrs1");
	mfree(m_offsets,m_numSegmentOffs*sizeof(int16_t *),"MapPtrs2");
	m_numSegmentPtrs = 0;
	m_numSegmentOffs = 0;

	m_newPagesPerSegment = 0;

	m_needToWrite     = false;
	m_fileStartOffset = 0LL;
	m_numSegments     = 0;
	m_numPages        = 0;
	m_maxNumPages     = 0;
	m_offset          = 0LL;
	m_numPositiveRecs = 0LL;
	m_numNegativeRecs = 0LL;
	//m_lastKey.n1      = 0;
	//m_lastKey.n0      = 0LL;
	KEYMIN(m_lastKey,MAX_KEY_BYTES); // m_ks);
	// close up shop
	// m_file.close ( ); this casues an error in Rdb.cpp:317 (new RdbMap)
	m_badKeys     = 0;
	m_needVerify  = false;

	m_file.reset();
}


bool RdbMap::writeMap ( bool allDone ) {
	logTrace( g_conf.m_logTraceRdbMap, "BEGIN. filename [%s]", m_file.getFilename());

	if ( g_conf.m_readOnlyMode ) {
		logTrace( g_conf.m_logTraceRdbMap, "END. Read-only mode, not writing map. filename [%s]. Returning true.",
		          m_file.getFilename());
		return true;
	}

	if ( ! m_needToWrite ) {
		logTrace( g_conf.m_logTraceRdbMap, "END. no need, not writing map. filename [%s]. Returning true.",
		          m_file.getFilename());
		return true;
	}

	log(LOG_INFO, "db: Saving %s", m_file.getFilename());

	// open a new file
	if ( ! m_file.open ( O_RDWR | O_CREAT | O_TRUNC ) ) {
		log(LOG_ERROR, "%s:%s: END. Could not open %s for writing: %s. Returning false.",
		    __FILE__, __func__, m_file.getFilename(), mstrerror(g_errno));
		return false;
	}
	   
			   
	// write map data
	bool status = writeMap2 ( );

	// on success, we don't need to write it anymore
	if ( status ) {
		m_needToWrite = false;
	}

	// map is done so save some memory
	if ( allDone ) {
		reduceMemFootPrint ();
	}

	logTrace( g_conf.m_logTraceRdbMap, "END. filename [%s], returning %s", m_file.getFilename(), status ? "true" : "false");

	return status;
}


bool RdbMap::writeMap2 ( ) {
	logTrace( g_conf.m_logTraceRdbMap, "BEGIN. filename [%s]", m_file.getFilename());
	
	// the current disk offset
	int64_t offset = 0LL;
	g_errno = 0;

	if( g_conf.m_logTraceRdbMap ) {
		log(LOG_DEBUG, " m_offset.........: %" PRId64, m_offset);
		log(LOG_DEBUG, " m_fileStartOffset: %" PRId64, m_fileStartOffset);
		log(LOG_DEBUG, " m_numPositiveRecs: %" PRId64, m_numPositiveRecs.load());
		log(LOG_DEBUG, " m_numNegativeRecs: %" PRId64, m_numNegativeRecs.load());
		loghex(LOG_DEBUG, m_lastKey, m_ks, " m_lastKey........: (hexdump)");
	}
	
	// first 8 bytes are the size of the DATA file we're mapping
	m_file.write ( &m_offset , 8 , offset );
	if ( g_errno )  {
		log(LOG_ERROR, "%s:%s: Failed to write to %s (m_offset): %s",
		    __FILE__, __func__, m_file.getFilename(), mstrerror(g_errno));
		return false;
	}
	
	offset += 8;

	// when a BigFile gets chopped, keep up a start offset for it
	m_file.write ( &m_fileStartOffset , 8 , offset );

	if ( g_errno ) {
		log(LOG_ERROR, "%s:%s: Failed to write to %s (m_fileStartOffset): %s",
		    __FILE__, __func__, m_file.getFilename(), mstrerror(g_errno));
		return false;
	}
	
	offset += 8;

	// store total number of non-deleted records
	m_file.write ( &m_numPositiveRecs , 8 , offset );
	if ( g_errno ) {
		log(LOG_ERROR,"%s:%s: Failed to write to %s (m_numPositiveRecs): %s",
		    __FILE__, __func__, m_file.getFilename(), mstrerror(g_errno));
		return false;
	}

	offset += 8;

	// store total number of deleted records
	m_file.write ( &m_numNegativeRecs , 8 , offset );
	if ( g_errno ) {
		log(LOG_ERROR,"%s:%s: Failed to write to %s (m_numNegativeRecs): %s",
		    __FILE__, __func__, m_file.getFilename(), mstrerror(g_errno));
		return false;
	}

	offset += 8;

	// store last key in map
	m_file.write ( m_lastKey , m_ks , offset );
	if ( g_errno ) {
		log(LOG_ERROR,"%s:%s: Failed to write to %s (m_lastKey): %s",
		    __FILE__, __func__, m_file.getFilename(), mstrerror(g_errno));
		return false;
	}

	offset += m_ks;

	logTrace( g_conf.m_logTraceRdbMap, "Writing %" PRId32" segments", m_numSegments);

	// . now store the map itself
	// . write the segments (keys/offsets) from the map file
	for ( int32_t i = 0 ; i < m_numSegments ; ++i ) {
		offset = writeSegment ( i , offset );
		if ( offset<=0 ) {
			log(LOG_ERROR, "%s:%s: Failed to write to %s (m_numSegments, segment %" PRId32"): %s",
			    __FILE__, __func__, m_file.getFilename(), i, mstrerror(g_errno));
			return false;
		}
	}

	logTrace( g_conf.m_logTraceRdbMap, "END - OK, returning true." );

	return true;
}


int64_t RdbMap::writeSegment( int32_t seg , int64_t offset ) {
	// how many pages have we written?
	int32_t pagesWritten = seg * PAGES_PER_SEGMENT;
	// how many pages are left to write?
	int32_t pagesLeft    = m_numPages - pagesWritten;
	// if none left to write return offset now
	if ( pagesLeft <= 0 ) return offset;
	// truncate to segment's worth of pages for writing purposes
	if ( pagesLeft > PAGES_PER_SEGMENT ) pagesLeft = PAGES_PER_SEGMENT;
	// determine writeSize for keys
	int32_t writeSize = pagesLeft * m_ks;
	// write the keys segment
	g_errno = 0;
	m_file.write ( (char *)m_keys[seg] , writeSize , offset );
	if ( g_errno ) return -1;//log("RdbMapFile::writeSegment: failed");
	offset += writeSize ;
	// determine writeSize for relative 2-byte offsets
	writeSize = pagesLeft * 2;
	// write the offsets of segment
	m_file.write ( (char *)m_offsets[seg] , writeSize , offset );
	if ( g_errno ) return -1;//log("RdbMapFile::writeSegment: failed");
	offset += writeSize ;
	// return the new offset
	return offset ;
}


// . called by openOld()
// . returns true on success
// . returns false on i/o error.
// . The format of the map on disk is described in Map.h
// . sets "m_numPages", "m_keys", and "m_offsets"
// . reads the keys and offsets into buffers allocated during open().
// . now we pass in ptr to the data file we map so verifyMap() can use it
bool RdbMap::readMap ( BigFile *dataFile ) 
{
	logTrace( g_conf.m_logTraceRdbMap, "BEGIN. filename [%s]", m_file.getFilename());
	
	// bail if does not exist
	if ( ! m_file.doesExist() ) {
		logError("Map file [%s] does not exist.", m_file.getFilename());
		
		logTrace( g_conf.m_logTraceRdbMap, "END. Returning false" );
		return false;
	}

			   
	// . open the file
	// . do not open O_RDONLY because if we are resuming a killed merge
	//   we will add to this map and write it back out.
	if ( ! m_file.open ( O_RDWR ) ) {
		logError("Could not open map file %s for reading: %s.", m_file.getFilename(),mstrerror(g_errno));
			   
		logTrace( g_conf.m_logTraceRdbMap, "END. Returning false" );
		return false;
	}

	bool status = readMap2();

	// . close map
	// . no longer since we use BigFile
	// . no, we have to close since we will hog all the fds
	// . we cannot call BigFile::close() because then RdbMap::unlink() will
	//   not work because BigFile::m_maxParts gets set to 0, and that is
	//   used in the loop in BigFile::unlinkRename().
	m_file.closeFds();

	// verify and fix map, data on disk could be corrupted
	if (status && !verifyMap(dataFile)) {
		logError("END. Could not verify map. filename [%s]. Returning false", m_file.getFilename());
		return false;
	}
	
	logTrace( g_conf.m_logTraceRdbMap, "END. Returning %s", status?"true":"false");

	// return status
	return status;
}

bool RdbMap::verifyMap ( BigFile *dataFile ) {
	logTrace( g_conf.m_logTraceRdbMap, "BEGIN. filename [%s]", m_file.getFilename());
	
	int64_t diff = m_offset - m_fileStartOffset;
	diff -= dataFile->getFileSize();
	// make it positive
	if ( diff < 0 ) {
		diff = diff * -1LL;
	}

	logTrace( g_conf.m_logTraceRdbMap, "diff: %" PRId64, diff );

	// . return false if file size does not match
	// . i've seen this happen before
	if ( diff ) {
		log(LOG_WARN, "%s:%s:%d: Map file [%s] says that file [%s] should be %" PRId64" bytes long, but it is %" PRId64" bytes.",
			__FILE__, __func__, __LINE__, m_file.getFilename(), dataFile->getFilename(), m_offset - m_fileStartOffset, dataFile->getFileSize() );
		    
		// we let headless files squeak by on this because we cannot
		// generate a map for them yet. if power went out a key can be
		// caught in the middle of a write... thus limit to 12 bytes
		if ( dataFile->doesPartExist(0) || diff >= 12 ) {
			return false;
		}

		// explain it
		log(LOG_WARN, "db: Datafile is headless (so the map can not be regenerated right now) and the difference is < 12, so we will let this one squeak by.");
		
		//log("RdbMap::verifyMap: Regenerating map.");
		//log("db: Please delete map file %s and restart. "
		//    "This will regenerate the map file.",
		//    //Continuing despite discrepancy.",
		//    m_file.getFilename());
		//exit(-1);
		//return false;
		//return true;
	}
	
	
	// are we a 16k page size map?
	int64_t maxSize =(int64_t)(m_numPages + 1)*(int64_t)m_pageSize;
	int64_t minSize =(int64_t)(m_numPages - 1)*(int64_t)m_pageSize;
	int64_t dfs = dataFile->getFileSize();
	if ( dfs < minSize || dfs > maxSize ) {
		//log("db: File is not mapped with PAGE_SIZE of %" PRId32". Please "
		//    "delete map file %s and restart in order to regenerate "
		//    "it. Chances are you are running a new version of gb on "
		//    "old data.", (int32_t)PAGE_SIZE, m_file.getFilename());
		log(LOG_ERROR, "File %s is not mapped with PAGE_SIZE of %" PRId32". "
		    "You may be running a new version of gb on "
		    "old data.", m_file.getFilename(),(int32_t)m_pageSize);
		//exit (-1);
		return false;
	}

	// . first, if our data file is headless we may have to chop our heads
	//   because a merge was probably killed
	// . how many head PARTs are missing?
	//int32_t numMissingParts = 0;
	//while ( ! dataFile->doesPartExist ( numMissingParts ) )
	//	numMissingParts++;

	// we should count backwards so we stop at the first gap from the top.
	// power outages sometimes leave one file linked when it should have
	// been unlinked... although a file after it was successfully recorded
	// as being unlinked on the hard drive, it itself was never committed.
	// thereby producing a gap in the contiguous sequence of part files.
	// let's ignore such islands. these islands can be more than one file
	// too. let's verify they are unlinked after the merge completes.
	int32_t numMissingParts = dataFile->getMaxParts();
	
	while ( numMissingParts > 0 && dataFile->doesPartExist ( numMissingParts-1 ) )
		numMissingParts--;

	
	if ( numMissingParts > 0 ) {
		logError("%" PRId32" missing parts. filename [%s]", numMissingParts, m_file.getFilename());

		File *f = dataFile->getFile2 ( numMissingParts );
		
		if ( f ) {
			logError("Missing part file before %s.", f->getFilename());
		}
	}

	// how many PARTs have been removed from map?
	int32_t removed = m_fileStartOffset / MAX_PART_SIZE;
	// . balance it out
	// . don't map to PARTs of data file that have been chopped
	while ( removed < numMissingParts ) {
		log(LOG_WARN,"db: Removing part #%" PRId32" from map.",removed);
		chopHead ( MAX_PART_SIZE );
		removed++;
	}
	// now fix the map if it had out of order keys in it
	bool status = verifyMap2 ( );
	logTrace( g_conf.m_logTraceRdbMap, "END. Returning %s", status?"true":"false" );
		
	return status;
}


// this just fixes a bad map
bool RdbMap::verifyMap2 ( ) {
// top:
	char lastKey[MAX_KEY_BYTES];
	KEYMIN(lastKey,m_ks);
	for ( int32_t i = 0 ; i < m_numPages ; i++ ) {
		char *k = getKeyPtr(i);
		if ( KEYCMP(k,lastKey,m_ks)>=0 ) {
			KEYSET(lastKey,k,m_ks); continue; }
		// just bitch for now
		log( LOG_WARN,
		    "db: Key out of order in map file %s/%s. "
		    "page = %" PRId32". key offset = %" PRId64". "
		    "Map or data file is "
		    "corrupt, but it is probably the data file. Please "
		    "delete the map file and restart.",
		    m_file.getDir(),m_file.getFilename() ,
		    i,(int64_t)m_pageSize*(int64_t)i+getOffset(i));

		//log("db: oldk.n1=%08" PRIx32" n0=%016" PRIx64,
		//    lastKey.n1,lastKey.n0);
		//log("db: k.n1=%08" PRIx32" n0=%016" PRIx64,k.n1 ,k.n0);
		log( LOG_WARN, "db: oldk.n1=%016" PRIx64" n0=%016" PRIx64,
		    KEY1(lastKey,m_ks),KEY0(lastKey));
		log( LOG_WARN, "db:    k.n1=%016" PRIx64" n0=%016" PRIx64,KEY1(k,m_ks),KEY0(k));
		log( LOG_WARN, "db: m_numPages = %" PRId32,m_numPages);

		logError("Previous versions would have move %s/%s to trash!!", m_file.getDir(), m_file.getFilename());

		// make the bash shell restart us by returning a 1 error code
		g_process.shutdownAbort(false);

#if 0
		//g_process.shutdownAbort(true);
		// was k too small?
		//if ( i + 1 < m_numPages && lastKey <= getKey(i+1) ) {
		if (i+1<m_numPages && KEYCMP(lastKey,getKeyPtr(i+1),m_ks)<=0){
			char f[MAX_KEY_BYTES];
			KEYSET(f,lastKey,m_ks);
			//if ( lastKey != getKey(i+1) ) f += (uint32_t)1;
			if (KEYCMP(lastKey,getKeyPtr(i+1),m_ks)!=0)
				KEYINC(f,m_ks);
			setKey(i,f);
			log("db: Key in map was too small. Fixed.");
			goto top;
		}
		// was lastKey too big?
		//if ( i - 2 >= m_numPages && getKey(i-2) <= k ) {
		if ( i - 2 >= m_numPages && KEYCMP(getKeyPtr(i-2),k,m_ks)<=0) {
			char *f = getKeyPtr(i-2);
			//if ( f != k ) f += (uint32_t)1;
			if ( KEYCMP(f,k,m_ks)!=0) KEYINC(f,m_ks);
			setKey(i-1,f);
			log("db: LastKey in map was too big. Fixed.");
			goto top;
		}
		// otherwise it is a sequence of out-of-order keys
		int32_t left  = i - 1;
		int32_t right = i;
		// try removing left side
		//while ( left > 0 && getKey(left-1) > k )
		while ( left > 0 && KEYCMP(getKeyPtr(left-1),k,m_ks)>0 )
			left--;
		int32_t leftCount = i - left;
		// try removing the right side
		//while ( right + 1 < m_numPages && getKey(right+1) < lastKey)
		while ( right + 1 < m_numPages &&
			KEYCMP(getKeyPtr(right+1),lastKey,m_ks)<0)
			right++;
		int32_t rightCount = right - i + 1;
		// make [a,b] represent the smallest bad chunk that when
		// removed will fix the map
		int32_t  a , b ;
		if ( leftCount <= rightCount ) { a = left ; b = i - 1 ; }
		else                           { a = i    ; b = right ; }
		char *keya = KEYMIN();
		if ( a > 0 ) keya = getKeyPtr(a-1);
		// remove the smallest chunk
		for ( int32_t j = a ; j <= b ; j++ )
			setKey ( j , keya );
		// count it for reference
		log("db: Removed bad block in map of %" PRId32" pages. Data "
		    "may have been permanently lost. Consider "
		    "syncing from a twin.",b-a+1);
		// try from the top
		goto top;
#endif
	}
	return true;
}

bool RdbMap::readMap2 ( ) {
	// keep track of read offset
	int64_t offset = 0;
	g_errno = 0;

	// first 8 bytes are the size of the DATA file we're mapping
	m_file.read ( &m_offset , 8 , offset );
	if ( g_errno ) {
		log( LOG_WARN, "db: Had error reading %s: %s.", m_file.getFilename(),mstrerror(g_errno));
		return false;
	}
	offset += 8;

	// when a BigFile gets chopped, keep up a start offset for it
	m_file.read ( &m_fileStartOffset , 8 , offset );
	if ( g_errno ) {
		log( LOG_WARN, "db: Had error reading %s: %s.", m_file.getFilename(),mstrerror(g_errno));
		return false;
	}
	offset += 8;

	// read total number of non-deleted records
	m_file.read ( &m_numPositiveRecs , 8 , offset );
	if ( g_errno ) {
		log( LOG_WARN, "db: Had error reading %s: %s.", m_file.getFilename(),mstrerror(g_errno));
		return false;
	}
	offset += 8;

	// read total number of deleted records
	m_file.read ( &m_numNegativeRecs , 8 , offset );
	if ( g_errno ) {
		log( LOG_WARN, "db: Had error reading %s: %s.", m_file.getFilename(),mstrerror(g_errno));
		return false;
	}
	offset += 8;

	// read total number of deleted records
	m_file.read ( m_lastKey , m_ks , offset );
	if ( g_errno ) {
		log( LOG_WARN, "db: Had error reading %s: %s.", m_file.getFilename(),mstrerror(g_errno));
		return false;
	}
	offset += m_ks;

	// get the total size of this map file from our derived file class
	int32_t fileSize = m_file.getFileSize () ;
	if ( fileSize < 0 ) {
		log( LOG_WARN, "db: getFileSize failed on %s: %s.", m_file.getFilename(),mstrerror(g_errno));
		return false;
	}

	// read in the segments
	for ( int32_t i = 0 ; offset < fileSize ; i++ ) {
		// . this advance offset passed the read segment
		// . it uses fileSize for reading the last partial segment
		offset = readSegment ( i , offset , fileSize ) ;
		if ( offset<=0 ) {
			log( LOG_WARN, "db: Had error reading %s: %s.", m_file.getFilename(), mstrerror(g_errno));
			return false;
		}
	}
	return true;
}

int64_t RdbMap::readSegment ( int32_t seg , int64_t offset , int32_t fileSize ) {
	// . add a new segment for this
	// . increments m_numSegments and increases m_maxNumPages
	if ( ! addSegment () ) return -1;
	// get the slot size, 1 12 byte key and 1 int16_t offset per page
	int32_t slotSize = m_ks + 2;
	// how much will we read now?
	int32_t totalReadSize = PAGES_PER_SEGMENT * slotSize;
	// how much left in the map file?
	int64_t avail = fileSize - offset;
	// . what's available MUST always be a multiple of 16
	// . sanity check
	if ( ( avail % slotSize ) != 0 ) {
		log("db: Had error reading part of map: Bad map "
		    "size."); return -1; }
	// truncate if not a full segment
	if ( totalReadSize > avail ) totalReadSize = avail;
	// get # of keys/offsets to read
	int32_t numKeys = totalReadSize / slotSize;
	// calculate how many bytes to read of keys
	int32_t readSize = numKeys * m_ks;
	// do the read
	g_errno = 0;
	m_file.read ( (char *)m_keys[seg] , readSize , offset );
	if ( g_errno ) return -1; // log("RdbMapFile::readSegment: failed");
	offset += readSize;
	// read the offsets of segment
	readSize = numKeys * 2;
	m_file.read ( (char *)m_offsets[seg] , readSize , offset );
	if ( g_errno ) return -1; // log("RdbMapFile::readSegment: failed");
	offset += readSize ;
	// increase m_numPages based on the keys/pages read
	m_numPages += numKeys;
	// return the new offset
	return offset ;
}

// . add a record to the map
// . returns false and sets g_errno on error
// . offset is the current offset of the rdb file where the key/data was added
// . TODO: speed this up
// . we pass in "data" so we can compute the crc of each page
bool RdbMap::addRecord ( char *key, char *rec , int32_t recSize ) {
	if (m_reducedMem) {
		gbshutdownAbort(true);
	}

	// what page is first byte of key on?
	int32_t pageNum = m_offset >> m_pageSizeBits;

	// what is the last page we touch?
	int32_t lastPageNum     = (m_offset + recSize - 1) >> m_pageSizeBits;

	// . see if we need to reallocate/allocate more pages in the map.
	// . g_errno should be set to ENOMEM
	// . only do this if we're NOT adding to disk
	// . should only change m_maxNumPages, not m_numPages
	// . if the rec is HUGE it may span SEVERAL, so do a while()
	while (lastPageNum + 2 >= m_maxNumPages) {
		if (!addSegment()) {
			log(LOG_ERROR, "db: Failed to add segment3 to map file %s.", m_file.getFilename());
			// core dump until we revert to old values
			gbshutdownAbort(true);
		}
	}

	// we need to call writeMap() before we exit
	m_needToWrite = true;

#ifdef GBSANITYCHECK
	// debug
	log(LOG_TRACE,"db: addmap k=%s keysize=%" PRId32" offset=%" PRId64" pagenum=%" PRId32,
	    KEYSTR(key,m_ks),recSize,m_offset,pageNum);
#endif

	// we now call RdbList::checkList_r() in RdbDump::dumpList()
	// and that checks the order of the keys
	// . sanity check
	// . a key of 0 is valid, so watch out for m_lastKey's sake
	// i changed from <= to < because i saw it core when two linkdb
	// keys were the same. no idea how that happened. maybe a timing
	// coincidence thingy.
	if (KEYCMP(key, m_lastKey, m_ks) < 0 && KEYCMP(m_lastKey, KEYMIN(), m_ks) != 0) {
		m_badKeys++;

		log(LOG_LOGIC,"build: RdbMap: added key out of order. count=%" PRId64" file=%s/%s.",
			m_badKeys, m_file.getDir(), m_file.getFilename());
		log(LOG_LOGIC,"build: offset=%" PRId64" k1=%s k2=%s", m_offset, KEYSTR(m_lastKey,m_ks), KEYSTR(key,m_ks));

		/// @todo Temporarily added for tracking down bug. We want the core. Keep permanently?
		gbshutdownCorrupted();

		// if being called from RdbDump.cpp...
//		g_errno = ECORRUPTDATA;
//		return false;
	}

	// remember the lastKey in the whole file
	KEYSET(m_lastKey,key,m_ks);

	// set m_numPages to the last page num we touch plus one
	m_numPages = lastPageNum + 1;

	if (KEYNEG(key)) {
		// keep a global tally on # of recs that are deletes (low bit cleared)
		m_numNegativeRecs++;
	} else {
		// keep a global tally on # of recs that are NOT deletes
		m_numPositiveRecs++;
	}

	// increment the size of the data file
	m_offset += recSize ;

	// . reset all pages above pageNum that we touch
	// . store -1 in offset to indicate it's continuation of key which
	//   started on another page
	// . store -1 on lastPageNum PLUS 1 incase we just take up lastPageNum
	//   ourselves and the next key will start on lastPageNum+1 at offset 0
	// . also by storing -1 for offset this page becomes available for
	//   keys/recs to follow
	for (int32_t i = pageNum + 1; i <= lastPageNum; i++) {
		setKey(i, key);
	}

	// . return now if we're NOT the first key wholly on page #pageNum
	// . add crc of this rec
	// . this offset will be -1 for unstarted pages
	// . tally the crc until we hit a new page
	if (getOffset(pageNum) >= 0) {
		return true;
	}

	// . if no key has claimed this page then we'll claim it
	// . by claiming it we are the first key to be wholly on this page
	setOffset(pageNum, (m_offset - recSize) & (m_pageSize - 1));
	setKey(pageNum, key);

	// success!
	return true;
}


// . call addRecord() or addKey() for each record in this list
bool RdbMap::prealloc ( RdbList *list ) {
	// sanity check
	if ( list->getKeySize() != m_ks ) { g_process.shutdownAbort(true); }
	// bail now if it's empty
	if ( list->isEmpty() ) return true;

	if ( m_reducedMem ) { g_process.shutdownAbort(true); }

	// what is the last page we touch?
	int32_t lastPageNum = (m_offset + list->getListSize() - 1) / m_pageSize;
	// . need to pre-alloc up here so malloc does not fail mid stream
	// . TODO: only do it if list is big enough
	while ( lastPageNum + 2 >= m_maxNumPages ) {
		if ( ! addSegment() ) {
			log( LOG_WARN, "db: Failed to add segment to map file %s.", m_file.getFilename() );
			return false;
		}
	}
	return true;
}

// . call addRecord() or addKey() for each record in this list
bool RdbMap::addList(RdbList *list) {
	logTrace(g_conf.m_logTraceRdbMap, "BEGIN. listStartKey=%s listEndKey=%s",
	         KEYSTR(list->getStartKey(), list->getKeySize()), KEYSTR(list->getEndKey(), list->getKeySize()));

	// sanity check
	if (list->getKeySize() != m_ks) {
		g_process.shutdownAbort(true);
	}

	// . reset list to beginning to make sure
	// . no, because of HACK in RdbDump.cpp we set m_listPtrHi < m_list
	//   so our first key can be a half key, calling resetListPtr()
	//   will reset m_listPtrHi and fuck it up
	//list->resetListPtr();

	// bail now if it's empty
	if ( list->isEmpty() ) {
		logTrace(g_conf.m_logTraceRdbMap, "END");
		return true;
	}

	if (g_conf.m_logTraceRdbMap) {
		char firstKey[MAX_KEY_BYTES];
		list->getCurrentKey(firstKey);

		char lastKey[MAX_KEY_BYTES];
		getLastKey(lastKey);

		logTrace(g_conf.m_logTraceRdbMap, "BEGIN. listFirstKey=%s mapLastKey=%s",
		         KEYSTR(firstKey, list->getKeySize()), KEYSTR(lastKey, list->getKeySize()));
	}

	// what is the last page we touch?
	int32_t lastPageNum = (m_offset + list->getListSize() - 1) / m_pageSize;

	if ( m_reducedMem ) { g_process.shutdownAbort(true); }

	// . need to pre-alloc up here so malloc does not fail mid stream
	// . TODO: only do it if list is big enough
	while ( lastPageNum + 2 >= m_maxNumPages ) {
		if ( ! addSegment() ) {
			log( LOG_WARN, "db: Failed to add segment to map file %s.", m_file.getFilename() );
			logTrace(g_conf.m_logTraceRdbMap, "END. Failed to add segment to map");
			return false;
		}
	}

#ifdef GBSANITYCHECK
	// print the last key from lasttime
	log("map: lastkey=%s",KEYSTR(m_lastKey,m_ks));
#endif

	char key[MAX_KEY_BYTES];
	for (; !list->isExhausted(); list->skipCurrentRecord()) {
		list->getCurrentKey(key);

		int32_t recSize = list->getCurrentRecSize();
		char *rec = list->getCurrentRec();
		if (!addRecord(key, rec, recSize)) {
			log(LOG_WARN, "db: Failed to add record to map: %s.", mstrerror(g_errno));
			logTrace(g_conf.m_logTraceRdbMap, "END. Failed to add record to map");
			// allow caller to try to fix the tree in the case of dumpinga tree to a file on disk
			return false;
		}
	}

	// sanity check -- i added this for debug but i think it was corrupted buckets!!
	//verifyMap2();

	list->resetListPtr();

	if (g_conf.m_logTraceRdbMap) {
		char lastKey[MAX_KEY_BYTES];
		getLastKey(lastKey);
		logTrace(g_conf.m_logTraceRdbMap, "END listLastKey=%s mapLastKey=%s",
		         KEYSTR(key, list->getKeySize()), KEYSTR(lastKey, list->getKeySize()));
	}
	return true;
}

// . set *rsp and *rep so if we read from first key on *rsp to first key on
//   *rep all records will have their key in [startKey,endKey]
// . the relative offset (m_offset[sp]) may be -1
// . this can now return negative sizes
int64_t RdbMap::getMinRecSizes ( int32_t   sp       ,
			      int32_t   ep       ,
			      const char  *startKey ,
			      const char  *endKey   ,
			      bool   subtract) const {
	// . calculate first page, "sp", whose key is >= startKey
	// . NOTE: sp may have a relative offset of -1
	// . in this case, just leave it be!
	//while ( sp <  ep && getKey(sp) <  startKey ) sp++;
	while ( sp <  ep && KEYCMP(getKeyPtr(sp),startKey,m_ks)<0 ) sp++;
	// now calculate endpg whose key is <= endKey
	int32_t ep1 = ep;
	//while ( ep >  sp && getKey(ep) >  endKey   ) ep--;
	while ( ep >  sp && KEYCMP(getKeyPtr(ep),endKey,m_ks)>0   ) ep--;
	// . if ep has a relative offset of -1 we can advance it
	// . we cannot have back-to-back -1 offset with DIFFERENT keys
	while ( ep <= ep1 && ep < m_numPages && getOffset(ep) == -1 ) ep++;
	// now getRecSizes on this contrained range
	return getRecSizes ( sp , ep , subtract );
}

// . like above, but sets an upper bound for recs in [startKey,endKey]
int64_t RdbMap::getMaxRecSizes ( int32_t   sp       ,
			      int32_t   ep       ,
			      const char  *startKey ,
			      const char  *endKey   ,
			      bool   subtract ) const {
	// . calculate first page, "sp", whose key is >= startKey
	// . NOTE: sp may have a relative offset of -1
	// . in this case, just leave it be!
	//while ( sp > 0 && getKey(sp) >  startKey ) sp--;
	while ( sp > 0 && KEYCMP(getKeyPtr(sp),startKey,m_ks)>0 ) sp--;
	// now calculate endpg whose key is > endKey
	//while ( ep < m_numPages && getKey(ep) <  endKey   ) ep++;
	while ( ep < m_numPages && KEYCMP(getKeyPtr(ep),endKey,m_ks)<0 ) ep++;
	// . if ep has a relative offset of -1 we can advance it
	// . we cannot have back-to-back -1 offset with DIFFERENT keys
	while ( ep < m_numPages && getOffset(ep) == -1 ) ep++;
	// now getRecSizes on this contrained range
	return getRecSizes ( sp , ep , subtract );
}

// . how many bytes in the range?
// . range is from first key on startPage UP TO first key on endPage
// . if endPage is >= m_numPages then range is UP TO the end of the file
// . this can now return negative sizes
int64_t RdbMap::getRecSizes ( int32_t startPage ,
				int32_t endPage ,
				bool subtract ) const {
	// . assume a minimum of one page if key range not well mapped
	// . no, why should we?
	// . if pages are the same, there's no recs between them!
	// . this seemed to cause a problem when startPage==endPage == lastPage
	//   and we started in the middle of a dump, so instead of reading
	//   0 bytes, since offset was the end of the file, the dump dumped
	//   some and we read that. And the # of bytes we read was not
	//   divisible by sizeof(key96_t) and RdbList::checkList_r() complained
	//   about the last key out of order, but that last key's last 8
	//   bytes were garbage we did NOT read from disk... phew!
	if ( startPage == endPage ) return 0; // return (int32_t)m_pageSize;

	int64_t offset1;
	int64_t offset2;

	if ( ! subtract ) {
		offset1 = getAbsoluteOffset ( startPage );
		offset2 = getAbsoluteOffset ( endPage   );
		return offset2 - offset1;
	}

	// . but take into account delete keys, so we can have a negative size!
	// . use random sampling
	int64_t size = 0;
	const char *k;
	for ( int32_t i = startPage ; i < endPage ; i++ ) {
		// get current page size
		offset1 = getAbsoluteOffset ( i     );
		offset2 = getAbsoluteOffset ( i + 1 );
		// get startKey for this page
		k = getKeyPtr ( i );
		// if key is a delete assume all in page are deletes
		//if ( (k.n0)&0x01 == 0LL) size -= (offset2 - offset1);
		if ( KEYNEG(k) )         size -= (offset2 - offset1);
		else                     size += (offset2 - offset1);
	}
	// return the size
	return size;
}

// if page has relative offset of -1, use the next page
int64_t RdbMap::getAbsoluteOffset(int32_t page) const {
	for(;;) {
		if ( page >= m_numPages ) return m_offset; // fileSize
		int64_t offset = (int64_t)getOffset(page) +
		                 (int64_t)m_pageSize * (int64_t)page;
		if ( getOffset(page) != -1 ) return offset + m_fileStartOffset;
		// just use end of page if in the middle of a record
		while ( page < m_numPages && getOffset(page) == -1 )
			page++;
	}
}

// . get offset of next known key after the one in page
// . do a while to skip rec on page "page" if it spans multiple pages
// . watch out for eof
int64_t RdbMap::getNextAbsoluteOffset(int32_t page) const {
	// advance to next page
	page++;
	// inc page as long as we need to
	while ( page < m_numPages && getOffset(page) == -1 ) page++;
	// . if we hit eof then return m_offset
	// . otherwise, we hit another key
	return getAbsoluteOffset ( page );
}

// . [startPage,*endPage] must cover [startKey,endKey]
// . by cover i mean have all recs with those keys
// . returns the endPage #
int32_t RdbMap::getEndPage(int32_t startPage, const char *endKey) const {
	// use "ep" for the endPage we're computing
	int32_t ep = startPage;
	// advance if "ep"'s key <= endKey
	//while ( ep < m_numPages && getKey(ep) <= endKey ) ep++;
	while ( ep < m_numPages && KEYCMP(getKeyPtr(ep),endKey,m_ks)<=0 ) ep++;
	// now we may have ended up on a page with offset of -1
	// which is not good so, even if page's key is > endKey, advance it
	while ( ep < m_numPages && getOffset(ep) == -1 ) ep++;
	// now we're done
	return ep;
}

// . convert a [startKey,endKey] range to a [startPage,endPage] range
// . this says that if you read from first key offset on *startPage UP TO
//   first key offset on *endPage you'll get the keys/recs you want
// . if *endPage equals m_numPages then you must read to the end of file
// . returns false if no keys in [startKey,endKey] are present
// . *maxKey will be an APPROXIMATION of the max key we have
bool RdbMap::getPageRange ( const char  *startKey  ,
			    const char  *endKey    ,
			    int32_t  *startPage ,
			    int32_t  *endPage   ,
			    char  *maxKey    ,
			    int64_t oldTruncationLimit ) const {
	// the first key on n1 is usually <= startKey, but can be > startKey
	// if the page (n-1) has only 1 rec whose key is < startKey
	int32_t n1 = getPage ( startKey );
	// . get the ending page for this scan
	// . tally up the deleted keys as we go
	int32_t n2 = getPage ( endKey   );
	// . set maxKey if we need to
	// . ensure that it is in [startKey,endKey] because it is used for
	//   determining what the length of an IndexList would have been
	//   if it was not truncated
	// . that is, we use maxKey for interpolation
	if ( maxKey ) {
		int32_t n3 = n2;
		if ( oldTruncationLimit >= 0 ) {
			int32_t nn = n1 + (oldTruncationLimit*6LL) / m_pageSize;
			if ( n3 > nn ) n3 = nn;
		}
		//while ( n3 > n1 && getKey(n3) > endKey ) n3--;
		while ( n3 > n1 && KEYCMP(getKeyPtr(n3),endKey,m_ks)>0 ) n3--;
		//*maxKey = getKey ( n3 );
		KEYSET(maxKey,getKeyPtr(n3),m_ks);
	}
	// . if the first key appearing on this page is <= endKey we inc n2
	// . make m_keys[n2] > endKey since we read up to first key on n2
	// . n2 can be equal to m_numPages (means read to eof then)
	//while ( n2 < m_numPages && getKey ( n2 ) <= endKey ) n2++;
	while ( n2 < m_numPages && KEYCMP(getKeyPtr(n2),endKey,m_ks)<=0 ) n2++;
	// skip n2 over any -1 offset
	while ( n2 < m_numPages && getOffset ( n2 ) == -1 ) n2++;
	// neither n1 nor n2 should have a -1 offset
	//if ( m_offsets[n1] == -1 || m_offsets[n2] == -1 ) {
	//log("getPageRange: bad engineer"); exit (-1); }
	// if n1 == n2 then it's a not found since the key on page n1 is >
	// startKey and > endKey AND all keys on page n1-1 are < startKey
	//if ( n1 == n2 ) return false;
	// otherwise set our stuff and return true
	*startPage = n1;
	*endPage   = n2;
	return true;
}


// . return a page number, N
// . if m_keys[N] < startKey then m_keys[N+1] is > startKey
// . if m_keys[N] > startKey then all keys before m_keys[N] in the rdb file
//   are < startKey
// . if m_keys[N] > startKey then m_keys[N-1] spans multiple pages so that
//   the key immediately after it on disk is in fact, m_keys[N]
int32_t RdbMap::getPage( const char *startKey) const {
	// if the key exceeds our lastKey then return m_numPages
	//if ( startKey > m_lastKey ) return m_numPages;
	if ( KEYCMP(startKey,m_lastKey,m_ks)>0 ) return m_numPages;
	// . find the disk offset based on "startKey"
	// . b-search over the map of pages
	// . "n"   is the page # that has a key <= "startKey"
	// . "n+1" has a key that is > "startKey"
	int32_t n     = ( m_numPages  ) / 2;
	int32_t step  = n / 2;
	while ( step > 0 ) {
		//if   ( startKey <= getKey ( n ) ) n -= step;
		//else                              n += step;
		if   ( KEYCMP(startKey,getKeyPtr(n),m_ks)<=0 ) n -= step;
		else                                        n += step;
		step >>= 1; // divide by 2
	}
	// . let's adjust for the inadaquecies of the above algorithm...
	// . increment n until our key is >= the key in the table
	//while ( n < m_numPages - 1 &&  getKey(n) < startKey ) n++;
	while ( n<m_numPages - 1 && KEYCMP(getKeyPtr(n),startKey,m_ks)<0 ) n++;
	// . decrement n until page key is LESS THAN OR EQUAL to startKey
	// . it is now <= the key, not just <, since, if the positive
	//   key exists it, then the negative should not be in this file, too!
	//while ( n > 0              &&  getKey(n) > startKey ) n--;
	while ( n>0              && KEYCMP(getKeyPtr(n),startKey,m_ks)>0 ) n--;
	// debug point
	//if ( m_offsets[n] == -1 && m_keys[n] == startKey &&
	//m_keys[n-1] != startKey )
	//log("debug point\n");
	// . make sure we're not in the middle of the data
	// . decrease n until we're on a page that has the start of a key
	while ( n > 0  &&  getOffset(n) == -1 ) n--;
	// this is the page we should start reading at
	return n;
	// . return immediately if this is our key (exact match)
	//if ( m_keys[n] == startKey ) return n;
	// . now m_keys[n] should be < startKey
	// . the next m_key, however, should be BIGGER than our key
	// . but if m_keys[n] spans multiple pages then skip over it
	//   because the next key in the map IMMEDIATELY follows it
	//if ( n < m_numPages - 1 &&  m_offsets[n+1] == -1 )
	//while ( n < m_numPages - 1 ) n++;
	// . now m_keys[n] may actually be bigger than startKey but it's
	//   only because the previous key on disk is less than startKey
	//return n;
}

void RdbMap::printMap () {
	int32_t h = 0;
	for ( int i = 0 ; i < m_numPages; i++ ) {
		//log(LOG_INFO,"page=%i) key=%" PRIu64"--%" PRIu32", offset=%hi\n",
		//	i,getKey(i).n0,getKey(i).n1,getOffset(i));
		// for comparing
		char buf[1000];
		sprintf(buf,"page=%i) key=%" PRIx64" %" PRIx64", offset=%hi",
		    i,KEY1(getKeyPtr(i),m_ks),KEY0(getKeyPtr(i)),
		    getOffset(i));
		h = hash32 ( buf , strlen(buf) , h );
		log(LOG_INFO,"%s",buf);
	}
	log(LOG_INFO,"map checksum = 0x%" PRIx32,h);
}


int64_t RdbMap::getMemAllocated() const {
	// . how much space per segment?
	// . each page has a key and a 2 byte offset
	int64_t space = PAGES_PER_SEGMENT * (m_ks + 2);
	// how many segments we use * segment allocation
	return (int64_t)m_numSegments * space;
}

bool RdbMap::addSegmentPtr ( int32_t n ) {
	if ( m_reducedMem ) { g_process.shutdownAbort(true); }
	// realloc
	if ( n >= m_numSegmentPtrs ) {
		char **k;
		int32_t nn = (int32_t)((float)n * 1.20) + 1;
		k = (char **) mrealloc (m_keys,
					m_numSegmentPtrs * sizeof(char *) ,
					nn * sizeof(char *) ,
					"MapPtrs1" );
		// failed?
		if ( ! k ) return false;
		// succeeded
		m_numSegmentPtrs = nn;
		m_keys = k;
	}

	// try offsets
	if ( n >= m_numSegmentOffs ) {
		int16_t **o;
		int32_t nn = (int32_t)((float)n * 1.20) + 1;
		o = (int16_t **) mrealloc (m_offsets,
					 m_numSegmentOffs * sizeof(int16_t *) ,
					 nn * sizeof(int16_t *) ,
					 "MapPtrs2" );
		// failed?
		if ( ! o ) return false;
		// succeeded
		m_numSegmentOffs = nn;
		m_offsets = o;
	}
	return true;
}

// try to save memory when there are many collections with tiny files on disk
void RdbMap::reduceMemFootPrint () {
	if ( m_numSegments != 1 ) {
		return;
	}

	if ( m_numPages >= 100 ) {
		return;
	}

	// if already reduced, return now
	if ( m_newPagesPerSegment > 0 ) {
		return;
	}

	// if it is like posdb0054.map then it is being merged into and
	// we'll resume a killed merge, so don't mess with it, we'll need to add more pages.
	const char *s = m_file.getFilename();
	for ( ; s && *s && ! is_digit(*s) ; s++ )
		;

	int id = 0;
	if ( s ) {
		id = atoi(s);
	}

	// id can be zero like for spiderdb0000.map
	if ( (id % 2) == 0 ) {
		return;
	}

	 // log("map: reducing mem footprint for %s/%s",
	 //     m_file.getDir(),
	 //     m_file.getFilename());

	// seems kinda buggy now..
	m_reducedMem = true;

	char *oldKeys = m_keys[0];
	short *oldOffsets = m_offsets[0];
	int pps = m_numPages;

	m_keys   [0] = (char *)mmalloc ( m_ks * pps , "RdbMap" );
	m_offsets[0] = (short *)mmalloc ( 2 * pps , "RdbMap" );

	// copy over
	memcpy ( m_keys   [0] , oldKeys    , m_ks * pps );
	memcpy ( m_offsets[0] , oldOffsets , 2    * pps );

	int oldPPS = PAGES_PER_SEGMENT;
	mfree ( oldKeys    , m_ks * oldPPS , "RdbMap" );
	mfree ( oldOffsets ,    2 * oldPPS , "RdbMap" );

	m_newPagesPerSegment = m_numPages;
}

// . add "n" segments
// . returns false and sets g_errno on error
bool RdbMap::addSegment (  ) {
	// a helper variable
	int32_t ks = m_ks;

	// easy variables
	int32_t n   = m_numSegments;
	int32_t pps = PAGES_PER_SEGMENT;

	if ( m_reducedMem ) {
		g_process.shutdownAbort(true);
	}

	// the array of up to MAX_SEGMENT pool ptrs is now dynamic too!
	// because diffbot uses thousands of collections, this will save
	// over 1GB of ram!
	if ( ! addSegmentPtr ( n ) ) {
		log( LOG_WARN, "db: Failed to allocate memory for adding seg ptr for map file %s.", m_file.getFilename() );
		return false;
	}

	// alloc spaces for each key segment
	// allocate new segments now
	m_keys[n]    = (char*) mmalloc ( ks * pps , "RdbMap" );
	m_offsets[n] = (int16_t         *) mmalloc ( 2  * pps , "RdbMap" );

	// free up the segment on any problem
	if ( ! m_offsets[n] || ! m_keys[n] ) {
		// set to NULL so we know if accessed illegally
		if ( m_keys   [n] ) {
			mfree ( m_keys[n]   , ks*pps, "RdbMap" );
			m_keys   [n] = NULL;
		}

		if ( m_offsets[n] ) {
			mfree ( m_offsets[n], 2*pps , "RdbMap" );
			m_offsets[n] = NULL;
		}

		log( LOG_WARN, "db: Failed to allocate memory for adding to map file %s.", m_file.getFilename());
		return false;
	}

	// set all new offsets to -1
	for ( int32_t j = 0 ; j < PAGES_PER_SEGMENT ; j++ ) {
		m_offsets[n][j] = -1;
	}

	// reset m_maxNumPages and m_numSegments
	m_numSegments++;
	m_maxNumPages += PAGES_PER_SEGMENT;

	return true;
}

// . chop off any segment COMPLETELY before pageNum
// . if pageNum is -1 free ALL segments
// . fileHeadSize should equal MAX_PART_SIZE #define'd in BigFile.h
// . MAX_PART_SIZE is the max size of a little file that is part of a BigFile
bool RdbMap::chopHead ( int32_t fileHeadSize ) {
	// ensure fileHeadSize is valid
	if ( fileHeadSize != MAX_PART_SIZE ) {
		log( LOG_LOGIC, "db: rdbmap: chopHead: fileHeadSize of %" PRId32" is invalid.", fileHeadSize );
		return false;
	}

	// what segment does this page fall on?
	int32_t segNum = (fileHeadSize / m_pageSize) / PAGES_PER_SEGMENT;

	// . must match exactly
	// . not any more i guess, we can still have a segment that
	//   corresponds in part to a PART file no longer with us
	//if ( fileHeadSize * m_pageSize * PAGES_PER_SEGMENT != segNum )
	//return log("RdbMap::chopHead: file head isn't multiple");
	// return true if nothing to delete
	if ( segNum == 0 ) return true;
	// . we need to call writeMap() before we exit
	// . not any more! if the merge is killed or saved in the middle then
	//   verifyMap() will now call chopHead() until the head of the map
	//   matches the head PART file of the data file we map
	//m_needToWrite = true;
	// a helper variable
	int32_t ks = m_ks;
	// remove segments before segNum
	for ( int32_t i = 0 ; i < segNum ; i++ ) {
		mfree ( m_keys   [i] , ks * PAGES_PER_SEGMENT , "RdbMap" );
		mfree ( m_offsets[i] , 2  * PAGES_PER_SEGMENT , "RdbMap" );
		// set to NULL so we know if accessed illegally
		m_keys   [i] = NULL;
		m_offsets[i] = NULL;
	}
	// adjust # of segments down
	m_numSegments -= segNum;
	// same with max # of used pages
	m_maxNumPages -= PAGES_PER_SEGMENT * segNum ;
	// same with # of used pages, since the head was ALL used
	m_numPages    -= PAGES_PER_SEGMENT * segNum ;
	// this could be below zero if last segment was chopped
	if ( m_numPages < 0 ) m_numPages = 0;
	// if 0 return now
	// if ( m_numSegments == 0 ) return true;
	// bury the stuff we chopped
	int32_t sk = sizeof(char  *);
	int32_t ss = sizeof(int16_t *);
	memmove ( &m_keys   [0] , &m_keys   [segNum] , m_numSegments * sk );
	memmove ( &m_offsets[0] , &m_offsets[segNum] , m_numSegments * ss );
	// adjust the m_fileStartOffset so getAbsoluteOffset(),... is ok
	m_fileStartOffset += segNum * PAGES_PER_SEGMENT * m_pageSize;
	return true;
}

// . attempts to auto-generate from data file, f
// . returns false and sets g_errno on error
bool RdbMap::generateMap ( BigFile *f ) {
	reset();

	if (g_conf.m_readOnlyMode) {
		return false;
	}

	log(LOG_INFO, "db: Generating map for %s/%s", f->getDir(), f->getFilename());

	/// @todo ALC remove this when we don't remove partfile during merge anymore
	// we don't support headless datafiles right now
	bool allowHeadless = true;
	if (m_fixedDataSize != 0 || m_ks != 18) {
		allowHeadless = false;
	}

	// allow posdb to go through
	if (!f->doesPartExist(0) && !allowHeadless) {
		g_errno = EBADENGINEER;
		log(LOG_WARN, "db: Cannot generate map for this headless data file yet");
		return false;
	}

	// scan through all the recs in f
	int64_t offset = 0;
	int64_t fileSize = f->getFileSize();

	// if file is length 0, we don't need to do much
	// g_errno should be set on error
	if (fileSize == 0 || fileSize < 0) {
		return (fileSize == 0);
	}

	// find first existing part file
	bool firstRead = true;
	int32_t firstFilePartNum = 0;
	for (;; firstFilePartNum++) {
		// stop when the part file exists
		if (f->doesPartExist(firstFilePartNum)) {
			break;
		}
	}

	if (firstFilePartNum > 0) {
		offset = MAX_PART_SIZE * firstFilePartNum;
	}

	// don't read in more than 10 megs at a time initially
	int64_t bufSize = fileSize;
	if (bufSize > 10 * 1024 * 1024) {
		bufSize = 10 * 1024 * 1024;
	}
	char *buf = (char *)mmalloc(bufSize, "RdbMap");

	// use extremes
	const char *startKey = KEYMIN();
	const char *endKey = KEYMAX();

	// a rec needs to be at least this big
	int32_t minRecSize = 0;

	// negative keys do not have the dataSize field... so undo this
	if (m_fixedDataSize == -1) {
		minRecSize += 0;
	} else {
		minRecSize += m_fixedDataSize;
	}

	// POSDB
	if (m_ks == 18) {
		minRecSize += 6;
	} else if (m_useHalfKeys) {
		minRecSize += m_ks - 6;
	} else {
		minRecSize += m_ks;
	}

	// for parsing the lists into records
	char key[MAX_KEY_BYTES];
	int32_t  recSize = 0;
	char *rec     = buf;
	int64_t next = 0LL;

	// read in at most "bufSize" bytes with each read
readLoop:
	// keep track of how many bytes read in the log
	if (offset >= next) {
		if (next != 0) {
			logf(LOG_INFO, "db: Read %" PRId64" bytes [%s]", next, f->getFilename());
		}

		next += 500000000; // 500MB
	}
	
	// our reads should always block
	int64_t readSize = fileSize - offset;
	if ( readSize > bufSize ) {
		readSize = bufSize;
	}

	// if the readSize is less than the minRecSize, we got a bad cutoff
	// so we can't go any more
	if (readSize < minRecSize) {
		mfree(buf, bufSize, "RdbMap");
		return true;
	}

	// otherwise, read it in
	if (!f->read(buf, readSize, offset)) {
		mfree(buf, bufSize, "RdbMap");
		log(LOG_WARN, "db: Failed to read %" PRId64" bytes of %s at offset=%" PRId64". Map generation failed.",
		    bufSize, f->getFilename(), offset);
		return false;
	}

	RdbList list;

	// if we were headless then first key on that page could be cut
	if ( firstFilePartNum > 0 && firstRead ) {
		firstRead = false;

		// scan the buffer to find the right key.
		int32_t fullKeyOff = findNextFullPosdbKeyOffset (buf,readSize);

		// if none found, bail
		if ( fullKeyOff < 0 ) {
			logError("Could not get a full key in the first %" PRId64" bytes read of headless file [%s]", readSize, f->getFilename());
			return false;
		}

		// for each page before add a -1 entry i guess
		int32_t p = 0;
		int32_t pageNum = 0;
		for (; p + m_pageSize < fullKeyOff; p += m_pageSize) {
			// add a dummy entry indicating a continuation of
			// a previous thing. we never had the full posdb key
			// so we don't know what the top 6 bytes were so
			// just stick -1 in there
			setOffset(pageNum, -1);
			setKey(pageNum, key);
			pageNum++;
		}

		// tell rdbmap where "list" occurs in the big file
		m_offset = offset + fullKeyOff;

		// set the list special here
		list.set(buf + fullKeyOff, readSize - fullKeyOff, buf, readSize, startKey, endKey, m_fixedDataSize, false, m_useHalfKeys, m_ks);
	} else {
		// set the list
		list.set(buf, readSize, buf, readSize, startKey, endKey, m_fixedDataSize, false, m_useHalfKeys, m_ks);
	}

	// . HACK to fix useHalfKeys compression thing from one read to the nxt
	// . "key" should still be set to the last record we read last read
	if ( offset > 0 ) {
		// ... fix for posdb!!!
		if (m_ks == 18) {
			list.setListPtrLo(key + (m_ks - 12));
		}
		list.setListPtrHi(key + (m_ks - 6));
	}

	// . parse through the records in the list
	// . stolen from RdbMap::addList()
nextRec:
	rec = list.getCurrentRec ();
	if (rec + 64 > list.getListEnd() && offset + readSize < fileSize) {
		// set up so next read starts at this rec that MAY have been
		// cut off
		offset += (rec - buf);
		goto readLoop;
	}

	// WARNING: when data is corrupted these may cause segmentation faults?
	list.getCurrentKey(key);
	recSize = list.getCurrentRecSize();

	// don't chop keys
	if ( recSize < 6 ) {
		log( LOG_WARN, "db: Got negative recsize of %" PRId32" at offset=%" PRId64" lastgoodoff=%" PRId64,
		    recSize , offset + (rec-buf), m_offset );

		// it truncates to m_offset!
		if (truncateFile(f)) {
			goto done;
		}
		return false;
	}

	// do we have a breech?
	if (rec + recSize > buf + readSize) {
		// save old
		int64_t oldOffset = offset;

		// set up so next read starts at this rec that got cut off
		offset += (rec - buf);

		// . if we advanced nothing, then we'll end up looping forever
		// . this will == 0 too, for big recs that did not fit in our
		//   read but we take care of that below
		// . this can happen if merge dumped out half-ass
		// . the write split a record...
		if (rec - buf == 0 && recSize <= bufSize) {
			log(LOG_WARN, "db: Map generation failed because last record in data file was split. Power failure while "
				"writing? Truncating file to %" PRId64" bytes. (lost %" PRId64" bytes)", offset, fileSize - offset);

			// when merge resumes it call our getFileSize()
			// in RdbMerge.cpp::gotLock() to set the dump offset
			// otherwise, if we don't do this and write data
			// in the middle of a split record AND then we crash
			// without saving the map again, the second call to
			// generateMap() will choke on that boundary and
			// we'll lose a massive amount of data like we did
			// with newspaperarchive
			m_offset = offset;
			goto done;
		}

		// is our buf big enough to hold this type of rec?
		if (recSize > bufSize) {
			mfree(buf, bufSize, "RdbMap");
			bufSize = recSize;
			buf = (char *) mmalloc(bufSize, "RdbMap");
			if (!buf) {
				log(LOG_WARN, "db: Got error while generating the map file: %s. offset=%" PRIu64".",
				    mstrerror(g_errno), oldOffset);
				return false;
			}
		}
		// read agin starting at the adjusted offset
		goto readLoop;
	}

	if (!addRecord(key, rec, recSize)) {
		// if it was key out of order, it might be because the
		// power went out and we ended up writing a a few bytes of
		// garbage then a bunch of 0's at the end of the file.
		// if the truncate works out then we are done.
		if (g_errno == ECORRUPTDATA && truncateFile(f)) {
			goto done;
		}

		// otherwise, give it up
		mfree(buf, bufSize, "RdbMap");
		log(LOG_WARN, "db: Map generation failed: %s.", mstrerror(g_errno));
		return false;
	}

	// skip current good record now
	if (list.skipCurrentRecord()) {
		goto nextRec;
	}

	// advance offset
	offset += readSize;

	// loop if more to go
	if ( offset < fileSize ) {
		goto readLoop;
	}

done:
	// don't forget to free this
	mfree(buf, bufSize, "RdbMap");

	// if there was bad data we probably added out of order keys
	if (m_needVerify) {
		logError("Fixing map for [%s]. Added at least %" PRId64" bad keys.", f->getFilename(), m_badKeys);

		verifyMap2();
		m_needVerify = false;
	}

	// otherwise, we're done
	return true;
}

// 5MB is a typical write buffer size, so do a little more than that
#define MAX_TRUNC_SIZE 6000000

bool RdbMap::truncateFile(BigFile *f) {
	// how big is the big file
	int64_t fileSize = f->getFileSize();
	int64_t tail = fileSize - m_offset;

	// up to 20MB is ok to remove if most just bytes that are zeroes
	log("db: Counting bytes that are zeroes in the tail.");
	int64_t count = 0;
	char buf [100000];
	int64_t off = m_offset;

	do {
		int32_t readSize = fileSize - off;
		if (readSize > 100000) {
			readSize = 100000;
		}

		if (!f->read(buf, readSize, off)) {
			logError("Failed to read %" PRId32" bytes of [%s] at offset=%" PRId64".", readSize, f->getFilename(), off);
			return false;
		}

		// count the zero bytes
		for (int32_t i = 0; i < readSize; i++) {
			if (buf[i] == 0) {
				count++;
			}
		}

		// read more if we can
		off += readSize;
	} while (off < fileSize);
		
	// remove those from the size of the tail
	tail -= count;

	// if too much remains, do not truncate it
	if (tail > MAX_TRUNC_SIZE) {
		logError("Cannot truncate data file because bad tail is %" PRId64" bytes > %" PRId32". That excludes bytes that are zero.",
		         tail, MAX_TRUNC_SIZE);
		return false;
	}

	// how many parts does it have?
	int32_t numParts = f->getNumParts();

	// what part num are we on?
	int32_t partnum = f->getPartNum(m_offset);
	File *p = f->getFile2(partnum);

	if (!p) {
		logError("Unable to get part %" PRId32" of file [%s]", partnum, f->getFilename());
		return false;
	}

	// get offset relative to the part file
	int32_t newSize = m_offset % (int64_t)MAX_PART_SIZE;

	// log what we are doing
	int32_t oldSize = p->getFileSize();
	int32_t lost    = oldSize - newSize;
	
	log(LOG_WARN, "db: Removing %" PRId32" bytes at the end of %s. Power outage probably corrupted it.",
		lost, p->getFilename());
	    
	log(LOG_WARN, "db: Doing a truncate(%s,%" PRId32").", p->getFilename(), newSize);

	// we must always be the last part of next to last part
	if ( partnum != numParts-1 && partnum != numParts-2 ) {
		logError("This file is not the last part or next to last part for this file. aborting truncation.");
		return false;
	}
			   
	// sanity check. if we are not the last part file, but are the next
	// to last one, then the the last part file must be less than
	// MAX_TRUNC_SIZE bytes big
	File *p2 = NULL;
	if (partnum == numParts - 2) {
		p2 = f->getFile2(partnum + 1);
		if (!p2) {
			logError("Could not get next part %" PRId32" of file [%s]", partnum + 1, f->getFilename());
			return false;
		}


		if (p2->getFileSize() > MAX_TRUNC_SIZE) {
			logError("db: Next part file is bigger than %" PRId32" bytes.", MAX_TRUNC_SIZE);
			return false;
		}
	}

	// do the truncation
	if (truncate(p->getFilename(), newSize)) {
		// return false if had an error
		logError("truncate(%s,%" PRId32"): %s.", p->getFilename(), newSize, mstrerror(errno));
		return false;
	}
			   
	// if we are not the last part, remove it
	if (partnum == numParts - 2) {
		log(LOG_DEBUG, "db: Removing tiny last part. unlink (%s).", p2->getFilename());

		// ensure it is smaller than 1k
		if (!p2->unlink()) {
			log(LOG_WARN, "db: Unlink of tiny last part failed.");
			return false;
		}
	}

	// reset file size, parts, etc on the big file since we truncated
	// a part file and possibly removed another part file
	if (!f->reset()) {
		log(LOG_WARN, "db: Failed to reset %s.", f->getFilename());
		return false;
	}

	// success
	return true;
}

int64_t RdbMap::findNextFullPosdbKeyOffset ( char *buf, int32_t bufSize ) {

	char *p = buf;
	char *lastKeyLo;
	char *lastKeyMe;
	char *lastKeyHi;
	int32_t keyCount;
	char *bufEnd = buf + bufSize;
	int32_t numWinners = 0;
	int64_t winnerOffset = -1;
	// try an offset of 0
	int64_t tryOffset = -2;
	bool printed;
	int64_t firstFullKeyOff;

 offsetLoop:

	printed = false;
	firstFullKeyOff = -1;

	tryOffset += 2;

	// only need to try 18 of them
	if ( tryOffset >= 18 )
		goto done;

	keyCount = 0;

	lastKeyLo = NULL;
	lastKeyMe = NULL;
	lastKeyHi = NULL;

 keyLoop:

	// posdbkey
	//key144_t *kp = (key144_t *)p;

	if ( p + 18 >= bufEnd )
		goto bufExhausted;

	char lastKey[18];
	if ( lastKeyHi ) {
		memcpy ( lastKey      , lastKeyLo , 6 );
		memcpy ( lastKey + 6  , lastKeyMe , 6 );
		memcpy ( lastKey + 12 , lastKeyHi , 6 );
	}

	char thisKey[18];

	// get lower compression bits
	if ( (p[0] & 0x04) ) {
		// make the full key to compare
		if ( lastKeyHi ) {
			memcpy ( thisKey      , p , 6 );
			memcpy ( thisKey + 6  , lastKeyMe , 6 );
			memcpy ( thisKey + 12 , lastKeyHi , 6 );
			if ( KEYCMP ( lastKey , thisKey , m_ks ) >= 0 ) {
				log("rdbmap: key out of order 1 tryoff of %i",
				    (int)tryOffset);
				goto offsetLoop;
			}
			keyCount++;
			//log("rdbmap: good key6 tryoff %i",(int)tryOffset);
		}
		lastKeyLo = p;
		p += 6;
		goto keyLoop;
	}
	// a 12 byte key?
	if ( (p[0] & 0x02) ) {
		// make the full key to compare
		if ( lastKeyHi ) {
			memcpy ( thisKey      , p , 12 );
			memcpy ( thisKey + 12 , lastKeyHi , 6 );
			if ( KEYCMP ( lastKey , thisKey , m_ks ) >= 0 ) {
				log("rdbmap: key out of order 2 @ %i "
				    "tryoff of %i",
				    (int)(p-buf),(int)tryOffset);
				goto offsetLoop;
			}
			keyCount++;
			//log("rdbmap: good key12 tryoff %i",(int)tryOffset);
		}
		lastKeyLo = p;
		lastKeyMe = p + 6;
		p += 12;
		goto keyLoop;
	}

	// did we have a key before us?
	if ( lastKeyHi && KEYCMP ( lastKey , p , 18 ) >= 0 ) {
		log("rdbmap: tryoffset of %i is bogus",(int)tryOffset);
		// keys out of order must not be a good 'tryOffset'
		goto offsetLoop;
	}

	// ensure it is valid with alignment bits


	keyCount++;
	if ( ! printed ) {
		log("rdbmap: good key18 @ %i tryoff %i",
		    (int)(p-buf),(int)tryOffset);
		printed = true;
		firstFullKeyOff = p-buf;
	}

	lastKeyLo = p;
	lastKeyMe = p + 6;
	lastKeyHi = p + 12;

	p += 18;

	// if ( keyCount >= 1000 ) {
	// 	log("rdbmap: got good tryoffset of %i",(int)tryOffset);
	// 	goodTry = tryOffset;
	// 	goodOnes++;
	// 	goto offsetLoop;
	// }

	goto keyLoop;

 bufExhausted:

	// only one key compared successfully, forget it then, not a winner
	if ( keyCount > 1 ) {
		// got a winner?
		numWinners++;
		winnerOffset = firstFullKeyOff;
		log("rdbmap: got winner @ %i at tryoff %i (keycount=%i)",
		    (int)winnerOffset,(int)tryOffset,(int)keyCount);
	}

	goto offsetLoop;

 done:

	if ( numWinners != 1 ) {
		log("rdbmap: could not figure out offset of first full key "
		    "in headless posdb file (numWinners=%i)",
		    (int)numWinners);
		return -1;
	}

	return winnerOffset;
}
