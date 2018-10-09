#include "Dir.h"

#include "Log.h"
#include "Errno.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fnmatch.h>

Dir::Dir ( ) {
	m_dirname = NULL;
	m_dir     = NULL;
	memset(m_dentryBuffer, 0, sizeof(m_dentryBuffer));
}


Dir::~Dir ( ) {
	reset();
}

void Dir::reset ( ) {
	close();
	free(m_dirname);
	m_dirname = NULL;
}

bool Dir::set ( const char *d1, const char *d2 ) {
	reset ();
	char tmp[1024];
	if ( strlen(d1) + strlen(d2) + 1 > 1024 ) {
		log(LOG_WARN, "disk: Could not set directory, directory name \"%s/%s\" "
		    "is too big.",d1,d2);
		return false;
	}
	sprintf ( tmp , "%s/%s", d1 , d2 );
	return set ( tmp );
}

bool Dir::set ( const char *dirname ) {
	reset ();
	m_dirname = strdup ( dirname );
	if ( m_dirname ) return true;
	log(LOG_WARN, "disk: Could not set directory, directory name to \"%s\": %s.",
	    dirname,mstrerror(g_errno));
	return false;
}

void Dir::close() {
	if ( m_dir )
		closedir ( m_dir );
	m_dir = NULL;
}

bool Dir::open ( ) {
	close ( );
	if ( ! m_dirname ) return false;
	do {
		m_dir = opendir ( m_dirname );
		// interrupted system call
	} while( ! m_dir && errno == EINTR );

	if ( ! m_dir ) {
		g_errno = errno;
		log( LOG_WARN, "disk: opendir(%s) : %s", m_dirname,strerror( g_errno ) );
		return false;
	}

	return true;
}

//Disable deprecated-declarations about use of readdir_r(). the glib maintainers deprecate readdir_r() because the linux
//version of readdir() is thread-safe. But the maintainers didn't think of other platforms, so that deprecation is
//linux-centric and not portable.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
const char *Dir::getNextFilename(const char *pattern) {
	if (!m_dir) {
		log("dir: m_dir is NULL so can't find pattern %s", pattern);
		return NULL;
	}

	//Note: m_dentryBuffer has a fixed sized. The reccommended way is to
	//use a dynamic size and use sysconf() to determine how large it should
	//be. I just take a wild guess that no paths are longer than 1024
	//characters.
	struct dirent *ent;
	while (readdir_r(m_dir, (dirent *)m_dentryBuffer, &ent) == 0 && ent) {
		const char *filename = ent->d_name;

		if (!pattern || (fnmatch(pattern, filename, FNM_PATHNAME) == 0)) {
			return filename;
		}
	}

	return NULL;
}
#pragma GCC diagnostic pop
