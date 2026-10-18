// mkromfs <romfs folder> <romfs.bin>: the RomFS image the runtime serves (what the start screen's game-file setup
// builds), from the command line.
#include <stdio.h>

#include "romfs_build.h"

static void progress(uint64_t done, uint64_t total) {
    fprintf(stderr, "\r%llu / %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <romfs folder> <romfs.bin>\n", argv[0]);
        return 2;
    }
    char why[256] = "";
    if (!romfs_build(argv[1], argv[2], progress, why, sizeof why)) {
        fprintf(stderr, "\nfailed: %s\n", why);
        return 1;
    }
    fprintf(stderr, "\ndone\n");
    return 0;
}
