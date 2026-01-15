/*
 * user-utilities/obos-ifconfig.c
 *
 * Copyright (c) 2026 Omar Berrow
 *
 * Network Interface Configure command
*/

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

const char* usage = "Usage: %s [-h] -i iface [command args...]\n";

enum {
    IOCTL_IFACE_MAC_REQUEST = 0xe100,
    IOCTL_IFACE_ADD_IP_TABLE_ENTRY,
    IOCTL_IFACE_REMOVE_IP_TABLE_ENTRY,
    IOCTL_IFACE_ADD_ROUTING_TABLE_ENTRY,
    IOCTL_IFACE_REMOVE_ROUTING_TABLE_ENTRY,
    IOCTL_IFACE_SET_IP_TABLE_ENTRY,
    IOCTL_IFACE_CLEAR_ARP_CACHE,
    IOCTL_IFACE_CLEAR_ROUTE_CACHE,
    IOCTL_IFACE_GET_IP_TABLE,
    IOCTL_IFACE_GET_ROUTING_TABLE,
    IOCTL_IFACE_SET_DEFAULT_GATEWAY,
    IOCTL_IFACE_UNSET_DEFAULT_GATEWAY,
    IOCTL_IFACE_INITIALIZE,
};

typedef union ip_addr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    struct {
        uint8_t comp1;
        uint8_t comp2;
        uint8_t comp3;
        uint8_t comp4;
    };
#else
    struct {
        uint8_t comp4;
        uint8_t comp3;
        uint8_t comp2;
        uint8_t comp1;
    };
#endif
    uint32_t addr;
} __attribute__((packed)) ip_addr;

#define IP_ADDRESS_FORMAT "%d.%d.%d.%d"
#define IP_ADDRESS_ARGS(addr) addr.comp1,addr.comp2,addr.comp3,addr.comp4

enum {
    IP_ENTRY_ENABLE_ICMP_ECHO_REPLY = 0b001,
          IP_ENTRY_ENABLE_ARP_REPLY = 0b010,
           IP_ENTRY_IPv4_FORWARDING = 0b100,
};

typedef struct ip_table_entry {
    ip_addr address;
    ip_addr broadcast;
    uint32_t subnet;
    uint32_t ip_entry_flags;
} ip_table_entry;

typedef struct gateway {
    ip_addr src;
    ip_addr dest;
} gateway;

static const char* flag_str(int b)
{
    return b ? "1" : "0";
}
char* entry_flags_str(uint32_t flags)
{
    char* ret = NULL;
    size_t len = 0;

    const char* flags_buf[3] = {};
    int i = 0;

    const char* format = "0b%s%s%s";

    if (flags & IP_ENTRY_ENABLE_ICMP_ECHO_REPLY)
        flags_buf[i++] = "ICMP_ECHO_REPLY";
    if (flags & IP_ENTRY_ENABLE_ARP_REPLY)
        flags_buf[i++] = "ARP_REPLY";
    if (flags & IP_ENTRY_IPv4_FORWARDING)
        flags_buf[i++] = "IPv4_FORWARD";

    if (flags_buf[0]) format = "0b%s%s%s (%s)";
    if (flags_buf[1]) format = "0b%s%s%s (%s|%s)";
    if (flags_buf[2]) format = "0b%s%s%s (%s|%s|%s)";


    len = snprintf(ret, len, format, flag_str(flags & 1), flag_str(flags & 2), flag_str(flags & 4), flags_buf[0], flags_buf[1], flags_buf[2]);    
    ret = malloc(++len);
    snprintf(ret, len, format, flag_str(flags & 1), flag_str(flags & 2), flag_str(flags & 4), flags_buf[0], flags_buf[1], flags_buf[2]);    

    return ret;
}

typedef uint8_t mac_address[6];
#define MAC_ADDRESS_FORMAT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ADDRESS_ARGS(addr) addr[0],addr[1],addr[2],addr[3],addr[4],addr[5]

int main(int argc, char** argv)
{
    int opt = 0;
    const char* iface = NULL;
    while ((opt = getopt(argc, argv, "+i:h")) != -1)
    {
        switch (opt)
        {
            case 'i':
                iface = optarg;
                break;
            case 'h':
            default:
                fprintf(stderr, usage, argv[0]);
                return opt != 'h';
        }
    }
    if (!usage)
    {
        if (!usage)
            fprintf(stderr, "Missing '-i'\n");
        fprintf(stderr, usage, argv[0]);
        return -1;
    }

    char** cmd_argv = &argv[optind];
    int cmd_argc = argc-optind;

    size_t iface_path_len = snprintf(NULL, 0, "/dev/%s", iface);
    char* iface_path = malloc(iface_path_len+1);
    snprintf(iface_path, iface_path_len+1, "/dev/%s", iface);
    int dev = open(iface_path, O_RDWR);
    if (dev < 0)
    {
        perror("open(iface_path)");
        return -1;
    }
    free(iface_path);

    const char* cmd = cmd_argv[0];
    if (!cmd)
        cmd = "ip-table";

    int res = 0;
    const char* perror_str = "ioctl";

    if (strcasecmp(cmd, "init") == 0)
        res = ioctl(dev, IOCTL_IFACE_INITIALIZE);
    else if (strcasecmp(cmd, "clear-arp-cache") == 0)
        res = ioctl(dev, IOCTL_IFACE_CLEAR_ARP_CACHE);
    else if (strcasecmp(cmd, "clear-route-cache") == 0)
        res = ioctl(dev, IOCTL_IFACE_CLEAR_ROUTE_CACHE);
    else if (strcasecmp(cmd, "unset-default-router") == 0)
        res = ioctl(dev, IOCTL_IFACE_UNSET_DEFAULT_GATEWAY, NULL);
    else if (strcasecmp(cmd, "ip-table") == 0)
    {
        mac_address iface_phys = {};
        res = ioctl(dev, IOCTL_IFACE_MAC_REQUEST, iface_phys);
        if (res < 0) goto fail;

        struct {ip_table_entry* buf; size_t sz;} table = {
            .buf = NULL,
            .sz = 0,
        };
        res = ioctl(dev, IOCTL_IFACE_GET_IP_TABLE, &table);
        if (res < 0) goto fail;
        table.buf = malloc(table.sz);
        res = ioctl(dev, IOCTL_IFACE_GET_IP_TABLE, &table);
        if (res < 0) goto fail;
        printf("IP table for %s <" MAC_ADDRESS_FORMAT ">:\n", iface, MAC_ADDRESS_ARGS(iface_phys));
        for (size_t i = 0; i < (table.sz / sizeof(ip_table_entry)); i++)
        {
            char* flags_str = entry_flags_str(table.buf[i].ip_entry_flags);
            printf("  "IP_ADDRESS_FORMAT "/%d flags=%s <brd: " IP_ADDRESS_FORMAT "/%d>\n",
                IP_ADDRESS_ARGS(table.buf[i].address),
                32 - __builtin_clz(table.buf[i].subnet),
                flags_str,
                IP_ADDRESS_ARGS(table.buf[i].broadcast),
                32 - __builtin_clz(table.buf[i].subnet)
            );
            free(flags_str);
        }
    }
    else if (strcasecmp(cmd, "routing-table") == 0)
    {
        mac_address iface_phys = {};
        res = ioctl(dev, IOCTL_IFACE_MAC_REQUEST, iface_phys);
        if (res < 0) goto fail;

        struct {struct gateway* buf; size_t sz;} table = {
            .buf = NULL,
            .sz = 0,
        };
        res = ioctl(dev, IOCTL_IFACE_GET_ROUTING_TABLE, &table);
        if (res < 0) goto fail;
        table.buf = malloc(table.sz);
        res = ioctl(dev, IOCTL_IFACE_GET_ROUTING_TABLE, &table);
        if (res < 0) goto fail;
        printf("Routing table for %s <" MAC_ADDRESS_FORMAT ">:\n", iface, MAC_ADDRESS_ARGS(iface_phys));
        for (size_t i = 0; i < (table.sz / sizeof(*table.buf)); i++)
        {
            if (!table.buf[i].src.addr)
                printf("  "IP_ADDRESS_FORMAT " (default gateway)\n", IP_ADDRESS_ARGS(table.buf[i].dest));   
            else
                printf("  "IP_ADDRESS_FORMAT "->" IP_ADDRESS_FORMAT "\n", IP_ADDRESS_ARGS(table.buf[i].src), IP_ADDRESS_ARGS(table.buf[i].dest));   
        }
    }
    else if (strcasecmp(cmd, "ip-address-add") == 0)
    {
        if (cmd_argc < 5)
        {
            fprintf(stderr, "%s needs 4 arguments\n", cmd);
            fprintf(stderr, "Usage: %s address brd_address subnet flags\n", cmd);
            goto fail;
        }
        struct in_addr addr = {};
        struct in_addr brd = {};
        uint32_t subnet = 0;
        uint32_t flags = 0;
        res = inet_aton(cmd_argv[1], &addr);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[1]);
            goto fail; // does not set errno
        }
        res = inet_aton(cmd_argv[2], &brd);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[2]);
            goto fail; // does not set errno
        }
        errno = 0;
        subnet = strtol(cmd_argv[3], NULL, 0);
        if (errno != 0)
        {
            perror_str = "strtol";
            res = -1;
            fprintf(stderr, "%s expected integer address, got %s instead\n", cmd, cmd_argv[3]);
            goto fail;
        }
        errno = 0;
        flags = strtol(cmd_argv[4], NULL, 0);
        if (errno != 0)
        {
            perror_str = "strtol";
            res = -1;
            fprintf(stderr, "%s expected integer address, got %s instead\n", cmd, cmd_argv[4]);
            goto fail;
        }


        ip_table_entry ent = {};
        ent.address.addr = addr.s_addr;
        ent.broadcast.addr = brd.s_addr;
        ent.subnet = (1 << subnet)-1;
        ent.ip_entry_flags = flags & 0b111;

        res = ioctl(dev, IOCTL_IFACE_ADD_IP_TABLE_ENTRY, &ent);
        if (res < 0)
        {
            if (errno == EEXIST)
            {
                res = ioctl(dev, IOCTL_IFACE_SET_IP_TABLE_ENTRY, &ent);
                if (res < 0)
                    goto fail;
            }
            goto fail;
        }
    }
    else if (strcasecmp(cmd, "ip-address-delete") == 0)
    {
        if (cmd_argc < 5)
        {
            fprintf(stderr, "%s needs 4 arguments\n", cmd);
            fprintf(stderr, "Usage: %s address brd_address subnet flags\n", cmd);
            goto fail;
        }
        struct in_addr addr = {};
        struct in_addr brd = {};
        uint32_t subnet = 0;
        uint32_t flags = 0;
        res = inet_aton(cmd_argv[1], &addr);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[1]);
            goto fail; // does not set errno
        }
        res = inet_aton(cmd_argv[2], &brd);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[2]);
            goto fail; // does not set errno
        }
        errno = 0;
        subnet = strtol(cmd_argv[3], NULL, 0);
        if (errno != 0)
        {
            perror_str = "strtol";
            res = -1;
            fprintf(stderr, "%s expected integer address, got %s instead\n", cmd, cmd_argv[3]);
            goto fail;
        }
        errno = 0;
        flags = strtol(cmd_argv[4], NULL, 0);
        if (errno != 0)
        {
            perror_str = "strtol";
            res = -1;
            fprintf(stderr, "%s expected integer address, got %s instead\n", cmd, cmd_argv[4]);
            goto fail;
        }


        ip_table_entry ent = {};
        ent.address.addr = addr.s_addr;
        ent.broadcast.addr = brd.s_addr;
        ent.subnet = (1 << subnet)-1;
        ent.ip_entry_flags = flags & 0b111;

        res = ioctl(dev, IOCTL_IFACE_REMOVE_IP_TABLE_ENTRY, &ent);
        if (res < 0) goto fail;
    }
    else if (strcasecmp(cmd, "set-default-router") == 0)
    {
        if (cmd_argc < 2)
        {
            fprintf(stderr, "%s needs 1 argument\n", cmd);
            fprintf(stderr, "Usage: %s address\n", cmd);
            goto fail;
        }
        struct in_addr addr = {};
        res = inet_aton(cmd_argv[1], &addr);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[1]);
            goto fail; // does not set errno
        }

        res = ioctl(dev, IOCTL_IFACE_SET_DEFAULT_GATEWAY, &addr);
    }
    else if (strcasecmp(cmd, "router-add") == 0)
    {
        if (cmd_argc < 3)
        {
            fprintf(stderr, "%s needs 2 arguments\n", cmd);
            fprintf(stderr, "Usage: %s source destination\n", cmd);
            goto fail;
        }
        struct in_addr addr = {};
        struct in_addr dest = {};
        uint32_t subnet = 0;
        uint32_t flags = 0;
        res = inet_aton(cmd_argv[1], &addr);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[1]);
            goto fail; // does not set errno
        }
        res = inet_aton(cmd_argv[2], &dest);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[2]);
            goto fail; // does not set errno
        }

        struct gateway ent = {};
        ent.src.addr = addr.s_addr;
        ent.dest.addr = dest.s_addr;
        
        res = ioctl(dev, IOCTL_IFACE_ADD_ROUTING_TABLE_ENTRY, &ent);
        if (res < 0) goto fail;
    }
    else if (strcasecmp(cmd, "router-delete") == 0)
    {
        if (cmd_argc < 3)
        {
            fprintf(stderr, "%s needs 2 arguments\n", cmd);
            fprintf(stderr, "Usage: %s source destination\n", cmd);
            goto fail;
        }
        struct in_addr addr = {};
        struct in_addr dest = {};
        uint32_t subnet = 0;
        uint32_t flags = 0;
        res = inet_aton(cmd_argv[1], &addr);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[1]);
            goto fail; // does not set errno
        }
        res = inet_aton(cmd_argv[2], &dest);
        if (res == 0)
        {
            fprintf(stderr, "%s expected ip address, got %s instead\n", cmd, cmd_argv[2]);
            goto fail; // does not set errno
        }

        struct gateway ent = {};
        ent.src.addr = addr.s_addr;
        ent.dest.addr = dest.s_addr;
        
        res = ioctl(dev, IOCTL_IFACE_REMOVE_ROUTING_TABLE_ENTRY, &ent);
        if (res < 0) goto fail;
    }
    else
        fprintf(stderr, "Unrecognized command %s\n", cmd);
    
    fail:
    if (res < 0)
        perror(perror_str);

    return res < 0 ? 1 : 0;
}