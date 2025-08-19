/*
 * init/nm.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>

#include <cjson/cJSON.h>

#include <arpa/inet.h>

#include "nm.h"

static ip_addr get_ip_addr_field(cJSON* parent, const char* field)
{
    cJSON* child = cJSON_GetObjectItem(parent, field);
    if (!child) return (ip_addr){.addr=0};
    const char* str = cJSON_GetStringValue(child);
    if (!str) return (ip_addr){.addr=0};
    return (ip_addr){.addr=inet_addr(str)};
}
static const char* get_str_field(cJSON* parent, const char* field)
{
    cJSON* child = cJSON_GetObjectItem(parent, field);
    if (!child) return NULL;
    const char* str = cJSON_GetStringValue(child);
    return str;
}
static bool get_boolean_field(cJSON* parent, const char* field, bool def)
{
    cJSON* obj = cJSON_GetObjectItem(parent, field);
    if (!obj) return def;
    return cJSON_IsTrue(obj);
}

/*
 * Format:
{
    "address": "x.x.x.x",
    "broadcast-address": "x.x.x.x",
    "subnet-mask": "x.x.x.x",
    // Default: true
    "ipv4-forwarding": boolean,
    // Default: true
    "arp-reply": boolean,
    // Default: true
    "icmp-echo-reply": boolean
}
*/
ip_table_entry_user nm_parse_ip_table_entry(cJSON* obj)
{
    ip_table_entry_user ret = {};
    ret.address = get_ip_addr_field(obj, "address");   
    ret.broadcast = get_ip_addr_field(obj, "broadcast-address");   
    ret.subnet = get_ip_addr_field(obj, "subnet-mask").addr;
    if (get_boolean_field(obj, "ipv4-forwarding", true))
        ret.ip_entry_flags |= IP_ENTRY_IPv4_FORWARDING;
    if (get_boolean_field(obj, "arp-reply", true))
        ret.ip_entry_flags |= IP_ENTRY_ENABLE_ARP_REPLY;
    if (get_boolean_field(obj, "icmp-echo-reply", true))
        ret.ip_entry_flags |= IP_ENTRY_ENABLE_ICMP_ECHO_REPLY;
    return ret;
}

/*
 * Format:
{
    "source": "x.x.x.x",
    "router": "x.x.x.x"
}
*/
gateway_user nm_parse_gateway(cJSON* obj)
{
    gateway_user ret = {};
    ret.src = get_ip_addr_field(obj, "source");
    ret.dest = get_ip_addr_field(obj, "router");
    return ret;
}

ip_table_entry_user* nm_get_ip_table(cJSON* top_level, size_t* size_array)
{
    *size_array = 0;
    cJSON* obj = NULL;
    cJSON* json_table = cJSON_GetObjectItem(top_level, "ip-table");
    if (!json_table) return NULL;
    ip_table_entry_user* table = calloc(cJSON_GetArraySize(json_table), sizeof(ip_table_entry_user));
    *size_array = cJSON_GetArraySize(json_table);
    size_t i = 0;
    cJSON_ArrayForEach(obj, json_table)
        table[i++] = nm_parse_ip_table_entry(obj);
    return table;
}
gateway_user* nm_get_gateways(cJSON* top_level, size_t* size_array)
{
    *size_array = 0;
    cJSON* obj = NULL;
    cJSON* json_table = cJSON_GetObjectItem(top_level, "static-routes");
    if (!json_table) return NULL;
    gateway_user* table = calloc(cJSON_GetArraySize(json_table), sizeof(gateway_user));
    *size_array = cJSON_GetArraySize(json_table);
    size_t i = 0;
    cJSON_ArrayForEach(obj, json_table)
        table[i++] = nm_parse_gateway(obj);
    return table;
}

/*
 * Format:
{
    "interface": "(interface name)",
// Default: false
    "dynamic-config": (boolean),
    // Ignored if dynamic-config = true
    "ip-table": [
        // See nm_parse_ip_table_entry
    ],
    "default-router": "x.x.x.x",
    "static-routes": [
        // See nm_parse_gateway
    ]
}
*/

bool nm_initialize_interface(cJSON* top_level)
{
    if (get_boolean_field(top_level, "dynamic-config", false))
    {
        fprintf(stderr, "NM: Dynamic interface configuration unsupported");
        return false;
    }
    const char* interface_name = get_str_field(top_level, "interface");
    if (!interface_name)
    {
        fprintf(stderr, "NM: No interface name specified\n");
        return false;
    }
    char* interface_path = malloc(strlen(interface_name)+sizeof("/dev/"));
    snprintf(interface_path, strlen(interface_name)+sizeof("/dev/"), "/dev/%s", interface_name);
    printf("NM: Initializing interface %s at %s\n", interface_name, interface_path);
    
    int fd = open(interface_path, O_RDWR);
    if (fd < 0)
    {
        perror("NM: Could not initialize interface: open");
        return false;
    }
    free(interface_path);

    size_t ip_table_len = 0, nGateways = 0;
    ip_table_entry_user* ip_table = nm_get_ip_table(top_level, &ip_table_len);
    gateway_user* gateways = nm_get_gateways(top_level, &nGateways);
    ip_addr default_gateway = get_ip_addr_field(top_level, "default-router");
    
    ioctl(fd, IOCTL_IFACE_INITIALIZE);
    for (size_t i = 0; i < ip_table_len; i++)
    {
        if (ip_table[i].address.addr && ip_table[i].broadcast.addr && ip_table[i].subnet)
            ioctl(fd, IOCTL_IFACE_ADD_IP_TABLE_ENTRY, &ip_table[i]);
        else
            fprintf(stderr, "NM: %s: Skipping invalid ip table entry\n", interface_name);
    }
    for (size_t i = 0; i < nGateways; i++)
    {
        if (gateways[i].src.addr && gateways[i].dest.addr)
            ioctl(fd, IOCTL_IFACE_ADD_ROUTING_TABLE_ENTRY, &gateways[i]);
        else
            fprintf(stderr, "NM: %s: Skipping invalid static route\n", interface_name);
    }
    if (default_gateway.addr)
        ioctl(fd, IOCTL_IFACE_SET_DEFAULT_GATEWAY, &default_gateway);
    else
        fprintf(stderr, "NM: %s: Skipping default gateway\n", interface_name);

    free(gateways);
    free(ip_table);

    return true;
}

void nm_initialize_interfaces(const char* config_file)
{
    int fd = open(config_file, O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        return;
    }
    struct stat st = {};
    fstat(fd, &st);
    char* buf = mmap(NULL, st.st_size, PROT_READ, 0, fd, 0);
    cJSON* top_level = cJSON_ParseWithLength(buf, st.st_size);
    if (!top_level)
    {
        const char* errptr = cJSON_GetErrorPtr();
        int dec = 0;
        for (; dec <= 15; dec++)
        {
            if (errptr > buf)
                errptr--;
            else
                break;
        }
        int inc = 0;
        for (; inc <= 15; inc++)
        {
            if (*(errptr+inc))
                continue;
            break;
        }
        int nToPrint = dec+inc;
        fprintf(stderr, "NM: JSON parsing error in file %s\n%.*s\n", config_file, nToPrint, errptr);
        return;
    }
    munmap(buf, st.st_size);
    close(fd);

    cJSON* obj = NULL;
    cJSON* json_table = cJSON_GetObjectItem(top_level, "interfaces");
    if (!json_table)
    {
        fprintf(stderr, "NM: No interfaces array in %s\n", config_file);
        return;
    }
    size_t nInterfacesInitialized = 0, nInterfaces = 0;
    cJSON_ArrayForEach(obj, json_table)
    {
        nInterfaces++;
        if (nm_initialize_interface(obj))
            nInterfacesInitialized++;
    }
    printf("NM: Initialized %ld interfaces (%ld failed)\n", nInterfacesInitialized, nInterfaces-nInterfacesInitialized);

    cJSON_Delete(top_level);
}
