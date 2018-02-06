#include "gb-include.h"

#include "Mem.h"
#include "UnicodeProperties.h"

UCPropTable g_ucLowerMap(sizeof(UChar32), 9);
UCPropTable g_ucUpperMap(sizeof(UChar32), 9);
UCPropTable g_ucProps(sizeof(UCProps), 8);
UCPropTable g_ucScripts(sizeof(UCScript), 10);
static UCPropTable g_ucKDIndex(sizeof(int32_t), 8);
// JAB: we now have Kompatible and Canonical decomposition

// Kompatible Decomposition
static char 	  *s_ucKDData = NULL;
static u_int32_t      s_ucKDDataSize = 0;
static u_int32_t      s_ucKDAllocSize = 0;

static uint32_t calculateChecksum(char *buf, int32_t bufLen){
	uint32_t sum = 0;
	for(int32_t i = 0; i < bufLen>>2;i++)
		sum += ((uint32_t*)buf)[i];
	return sum;
}


bool saveUnicodeTable(UCPropTable *table, const char *filename) {
	size_t tableSize = table->getStoredSize();
	char *buf = (char*)mmalloc(tableSize,"UP1");
	if (!buf){
		log(LOG_WARN, "uni: Couldn't allocate %" PRId32" bytes "
		    "for storing %s", (int32_t)tableSize,filename);
		return false;
	}
	
	if (!table->serialize(buf, tableSize)) {
		mfree(buf,tableSize,"UP1");
		log(LOG_WARN, "uni: Error serializing %s", 
		    filename);
		return false;
	}
	
	FILE *fp = fopen(filename, "w");
	if (!fp) {
		mfree(buf,tableSize,"UP1");
		log(LOG_WARN, "uni: "
		    "Couldn't open %s for writing: %s", 
		       filename, strerror(errno));
		return false;
	}
	
	size_t nwrite = fwrite(buf, tableSize, 1, fp);
	if (nwrite != 1) {
		log(LOG_WARN, "uni: Error writing %s", 
		    filename);
		mfree(buf,tableSize,"UP1");
		fclose(fp);
		return false;
	}
	mfree(buf,tableSize,"UP1");
	fclose(fp);
	return true;


}


bool loadUnicodeTable(UCPropTable *table, const char *filename, bool useChecksum, uint32_t expectedChecksum) {

	FILE *fp = fopen(filename, "r");
	if (!fp) {
		log(LOG_WARN, "Couldn't open [%s] for reading", filename );
		return false;
	}
	fseek(fp,0,SEEK_END);
	long fileSize = ftell(fp);
	if( fileSize < 0 ) {
		fclose(fp);
		log(LOG_WARN, "Getting size of [%s] failed", filename);
		return false;
	}
	rewind(fp);
	char *buf = (char*)mmalloc(fileSize, "Unicode");
	if (!buf) {
		fclose(fp);
		log(LOG_WARN, "uni: No memory to load %s", filename);
		return false;
	}
	size_t nread = fread(buf, 1, fileSize, fp);
	if (nread != (size_t)fileSize) {
		fclose(fp);
		mfree(buf, fileSize, "Unicode");
		log(LOG_WARN, "uni: error reading %s", filename);
		return false;
	}

	uint32_t chksum = calculateChecksum(buf, fileSize);
	if (useChecksum && (expectedChecksum != chksum)) {
		fclose(fp);
		mfree(buf, fileSize, "Unicode");
		log(LOG_WARN, "uni: checksum failed for %s", filename);
		return false;
	}

	if (!table->deserialize(buf, fileSize)) {
		fclose(fp);
		mfree(buf, fileSize, "Unicode");
		log(LOG_WARN, "uni: error deserializing %s", filename);
		return false;
	}

	fclose(fp);
	mfree(buf, fileSize, "Unicode");
	return true;
}


static const UChar32 *getKDValue(UChar32 c, int32_t *decompCount, bool *fullComp = NULL) {
	*decompCount = 0;
	if (fullComp) *fullComp = false;
	int32_t *pos = (int32_t*)g_ucKDIndex.getValue(c);
	if (!pos || !*pos) return NULL;
	*decompCount = (*(int32_t*)(&s_ucKDData[*pos])) & 0x7fffffff;
	if (fullComp) *fullComp = (*(int32_t*)(&s_ucKDData[*pos])) & 0x80000000;
	return (UChar32*) (&s_ucKDData[*pos+sizeof(int32_t)]);
}

int32_t recursiveKDExpand(UChar32 c, UChar32 *buf, int32_t bufSize) {
	int32_t decompCount = 0;
	const UChar32 *decomp = getKDValue(c, &decompCount);
	if (!decompCount) {
		buf[0] = c;
		return 1;
	}

	int32_t decompIndex = 0;
	for (int i=0;i<decompCount;i++) {
		decompIndex += recursiveKDExpand(decomp[i], 
						 buf+decompIndex,
						 bufSize-decompIndex);
	}
	return decompIndex;
}

// JAB: we now have Kompatible and Canonical decomposition
bool saveKDecompTable(const char *baseDir) {
	if (!s_ucKDData) return false;
	//char *filename = "ucdata/kd_data.dat";
	char filename[384];
	if (!baseDir) baseDir = ".";
	strcpy(filename, baseDir);
	strcat(filename, "/ucdata/kd_data.dat");
	size_t fileSize = s_ucKDDataSize;
	FILE *fp = fopen(filename, "w");
	if (!fp) {
		log(LOG_WARN, "uni: "
		    "Couldn't open %s for writing: %s", 
		       filename, strerror(errno));
		return false;
	}
	
	size_t nwrite = fwrite(s_ucKDData, fileSize, 1, fp);
	if (nwrite != 1) {
		log(LOG_WARN, "uni: Error writing %s "
		    "(filesize: %" PRId32")",
		    filename, (int32_t)fileSize);
		fclose(fp);
		return false;
	}
	fclose(fp);
	strcpy(filename, baseDir);
	strcat(filename, "/ucdata/kdmap.dat");
	return saveUnicodeTable(&g_ucKDIndex, filename);
}

// JAB: we now have Kompatible and Canonical decomposition
void resetDecompTables() {
	mfree(s_ucKDData, s_ucKDAllocSize, "UnicodeData");
	s_ucKDData = NULL;
	s_ucKDAllocSize = 0;
	s_ucKDDataSize = 0;
	g_ucKDIndex.reset();
}

// JAB: we now have Kompatible and Canonical decomposition
static bool loadKDecompTable(const char *baseDir) {
	if (s_ucKDData) {
		//reset table if already loaded
		resetDecompTables();
	}
	
	//char *filename = "ucdata/kd_data.dat";
	char filename[384];
	if (!baseDir) baseDir = ".";
	strcpy(filename, baseDir);
	strcat(filename, "/ucdata/kd_data.dat");

	FILE *fp = fopen(filename, "r");
	if (!fp) {
		log(LOG_WARN, "uni: Couldn't open %s for reading: %s", filename, strerror(errno));
		return false;
	}
	fseek(fp,0,SEEK_END);
	long fileSize = ftell(fp);
	if( fileSize <= 0 ) {
		fclose(fp);
		log(LOG_WARN, "uni: File [%s] not found or 0 bytes", filename);
		return false;
	}
	rewind(fp);
	char *buf = (char*)mmalloc(fileSize, "UnicodeProperties");
	if (!buf) {
		fclose(fp);
		log(LOG_WARN, "uni: No memory to load %s", filename);
		return false;
	}
	size_t nread = fread(buf, 1, fileSize, fp);
	if (nread != (size_t)fileSize) {
		fclose(fp);
		mfree(buf, fileSize, "UnicodeProperties");
		log(LOG_WARN, "uni: error reading %s", filename);
		return false;
	}
	fclose(fp);
	
	strcpy(filename, baseDir);
	strcat(filename, "/ucdata/kdmap.dat");
	if (!loadUnicodeTable(&g_ucKDIndex, filename)) {
		mfree(buf, fileSize, "UnicodeProperties");
		return false;
	}
	s_ucKDData = buf;
	s_ucKDDataSize = nread;
	s_ucKDAllocSize = nread;
	return true;

}

// JAB: we now have Kompatible and Canonical decomposition
bool loadDecompTables(const char *baseDir) {
	return loadKDecompTable(baseDir);
}
