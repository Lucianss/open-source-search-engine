#include "RdbIndex.h"
#include "Posdb.h"
#include "Log.h"
#include "Conf.h"
#include "Mem.h"
#include "GbUtil.h"
#include "Version.h"
#include <libgen.h>
#include <limits.h>

static void print_usage(const char *argv0) {
	fprintf(stdout, "Usage: %s [-h] FILE\n", argv0);
	fprintf(stdout, "Dump records in RDB index\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "  -h, --help     display this help and exit\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ) {
		print_usage(argv[0]);
		return 1;
	}

    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0 ) {
        printVersion(basename(argv[0]));
        return 1;
    }

	char filepath[PATH_MAX];

	char dir[PATH_MAX];
	strcpy(filepath, argv[1]);
	strcpy(dir, dirname(filepath));

	char filename[PATH_MAX];
	strcpy(filepath, argv[1]);
	strcpy(filename, basename(filepath));

	// initialize library
	g_mem.init();
	hashinit();

	g_conf.init(NULL);

	BigFile bigFile;
	bigFile.set(dir, filename);

	RdbIndex index;
	if (starts_with(filename, "posdb")) {
		index.set(dir, filename, Posdb::getFixedDataSize(), Posdb::getUseHalfKeys(), Posdb::getKeySize(), RDB_POSDB, true);
		if (!index.readIndex()) {
			fprintf(stdout, "Unable to load index\n");
			return 1;
		}

		index.printIndex();
	} else {
		fprintf(stdout, "Unsupported rdb type\n");
		return 1;
	}

	return 0;
}
