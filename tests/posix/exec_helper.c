/* Item 1 execve yardimcisi: ayri bir statik PIE ELF64 ikilisi */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *env = getenv("ARCTIAN_EXEC_OK");
    if (argc >= 2 && strcmp(argv[1], "merhaba-exec") == 0 &&
        env && strcmp(env, "1") == 0) {
        puts("[posix-test]   exec_helper: argv+env OK");
        return 77;
    }
    puts("[posix-test]   exec_helper: FAIL");
    return 1;
}
