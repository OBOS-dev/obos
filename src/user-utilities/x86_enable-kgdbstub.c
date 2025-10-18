#include <obos/syscall.h>
#include <obos/error.h>
#include <unistd.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <strings.h>

static const char* usage = "%s [-d pathspec] [-a address] [-p port] [-i protospec]\nAt least -d or -a, -p, and -i are to be specified, else, the gdb stub bound, possibly causing the start to fail\n";

int main(int argc, char** argv)
{
    int opt = 0;
    const char* devspec = NULL;
    const char* addrspec = NULL;
    const char* portspec = NULL;
    const char* protospec = NULL;
    while ((opt = getopt(argc, argv, "d:a:p:i:h")) != -1)
    {
        switch (opt)
        {
            case 'd':
                devspec = optarg;
                break;
            case 'a':
                addrspec = optarg;
                break;
            case 'p':
                portspec = optarg;
                break;
            case 'i':
                protospec = optarg;
                break;
            case 'h':
            default:
                fprintf(stderr, usage, argv[0]);
                return opt != 'h';
        }
    }
    
    obos_status status = OBOS_STATUS_SUCCESS;
    // Bind the GDB stub.
    if (devspec)
    {
        int fd = open(devspec, O_RDONLY);
        if (fd < 0)
        {
            perror("open");
            return -1;
        }
        status = syscall1(SysS_GDBStubBindDevice, fd);
    }
    else if (addrspec && portspec && protospec)
    {
        struct sockaddr_in addr = {};
        int ip_proto = 0;
        addr.sin_family = AF_INET;
        if (inet_aton(addrspec, &addr.sin_addr) == 0)
        {
            fprintf(stderr, "Invalid addrspec: inet_aton failed.\n");
        }
        errno = 0;
        addr.sin_port = htons((uint16_t)strtoul(portspec, NULL, 0));
        if (errno != 0)
        {
            perror("Invalid portspec: strtoul");
            return -1;
        }
        if (strcasecmp(protospec, "udp") == 0)
            ip_proto = IPPROTO_UDP;
        else if (strcasecmp(protospec, "tcp"))
            ip_proto = IPPROTO_TCP;
        else
        {
            fprintf(stderr, "protospec can only be udp/tcp. Got %s\n", protospec);
            return -1;
        }
        status = syscall2(SysS_GDBStubBindInet, &addr, ip_proto);
    }

    if (obos_is_error(status))
    {
        fprintf(stderr, "While binding GDB Stub, got status %d\n", status);
        return -1;
    }

    fprintf(stderr, "Starting GDB Stub! This will wait for the connection!\n");
    status = syscall0(SysS_GDBStubStart);
    if (obos_is_error(status))
    {
        fprintf(stderr, "While starting GDB Stub, got status %d\n", status);
        return -1;
    }
    
    return 0;
}