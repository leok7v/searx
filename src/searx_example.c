// Embed searx.c as a single-file library: SEARX_LIB drops its main().
#define SEARX_LIB
#include "searx.c"

int main(int argc, char * argv[], char * env[]) {
    return searx_run(argc, argv, env);
}
