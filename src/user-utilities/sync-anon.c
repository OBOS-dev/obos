#include <obos/syscall.h>

int main(int argc, char** argv)
{
    syscall0(Sys_SyncAnonPages);
    return 0;
}
