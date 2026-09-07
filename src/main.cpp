#include <cstdio>

// CLI dispatch (init/backup/list/restore/verify/stats/bench) lands here as
// each subcommand's backing component is built. For now this is just a
// placeholder so the dedup-backup target links.
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "dedup-backup: no subcommand given (not implemented yet)\n");
        return 1;
    }
    std::fprintf(stderr, "dedup-backup: subcommand '%s' not implemented yet\n", argv[1]);
    return 1;
}
