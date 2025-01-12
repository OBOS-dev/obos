#include <errno.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char** argv, char** envp)
{
    printf("testing fork\n");
    pid_t pid = fork();
    if (pid == 0)
        printf("in child, we are %d\n", getpid());
    else if (pid > 0)
        printf("in parent, child is %d\n", pid);
    else
        printf("error\n");
    return 0;
}
