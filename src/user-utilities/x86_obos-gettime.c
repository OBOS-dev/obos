#include <time.h>
#include <stdio.h>
#include <obos/syscall.h>

int main()
{
    long secs = 0;
    long nsecs = 0;
    syscall3(SysS_ClockGet, 0, &secs, &nsecs);
    struct tm* time = localtime(&secs);
    printf("%d-%02d-%02d %02d:%02d:%02d\n", time->tm_year+1900, time->tm_mon+1, time->tm_mday, time->tm_hour, time->tm_min, time->tm_sec);
    return 0;
}