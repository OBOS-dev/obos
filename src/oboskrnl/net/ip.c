/*
 * oboskrnl/net/ip.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <locks/rw_lock.h>
#include <locks/event.h>
#include <locks/wait.h>
#include <locks/mutex.h>

#include <net/ip.h>
#include <net/udp.h>
#include <net/arp.h>
#include <net/tables.h>
#include <net/eth.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/tree.h>

RB_GENERATE(address_table, address_table_entry, node, cmp_address_table_entry);
LIST_GENERATE(ip_table, ip_table_entry, node);

uint16_t NetH_OnesComplementSum(void *buffer, size_t size)
{
    uint16_t *p = buffer;
    int sum = 0;
    int i;
    for (i = 0; i < ((int)size & ~(1)); i += 2) {
        sum += be16_to_host(p[i >> 1]);
    }

    if (size & 1) {
        sum += ((uint8_t *)p)[i];
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += sum >> 16;

    uint16_t ret = ~sum;
    return ret;
}

uint16_t Net_IPChecksum(ip_header* hdr)
{
    return NetH_OnesComplementSum(hdr, IPv4_GET_HEADER_LENGTH(hdr));
}

obos_status Net_FormatIPv4Packet(ip_header** phdr, void* data, uint16_t sz, uint8_t precedence, const ip_addr* restrict source, const ip_addr* restrict destination, uint8_t lifetime_seconds, uint8_t protocol, uint8_t service_type, bool override_preferred_size)
{
    if (!phdr || !data || !sz || !source || !destination)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!override_preferred_size && sz > IPv4_PREFERRED_PACKET_LENGTH)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ip_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ip_header)+sz, nullptr);
    hdr->protocol = protocol;
    hdr->id_flags_fragment = 0;
    hdr->time_to_live = lifetime_seconds;
    hdr->service_type = (precedence & 0x3) | (service_type & 0x70);
    hdr->version_hdrlen = (4 << 4) | (sizeof(*hdr) / 4);
    hdr->packet_length = host_to_be16(sz+sizeof(ip_header));
    hdr->src_address.addr = source->addr;
    hdr->dest_address.addr = destination->addr;
    hdr->chksum = host_to_be16(Net_IPChecksum(hdr));
    memcpy(hdr+1, data, sz);
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}

obos_status Net_IPReceiveFrame(const frame* data)
{
    OBOS_UNUSED(data);
    ip_header* hdr = (void*)data->buff;
    // verify the header's integrity.
    uint16_t tmp = hdr->chksum;
    hdr->chksum = 0;
    uint16_t chksum = Net_IPChecksum(hdr);
    hdr->chksum = tmp;
    if (be16_to_host(tmp) != chksum)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid checksum. Expected 0x%04x, got 0x%04x. Dropping packet.\n", 
            data->interface_mac_address[0], data->interface_mac_address[1], data->interface_mac_address[2], 
            data->interface_mac_address[3], data->interface_mac_address[4], data->interface_mac_address[5],
            chksum, be16_to_host(tmp)    
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (IPv4_GET_HEADER_VERSION(hdr) != 4)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has unsupported version IPv%d. Dropping packet.\n", 
            data->interface_mac_address[0], data->interface_mac_address[1], data->interface_mac_address[2], 
            data->interface_mac_address[3], data->interface_mac_address[4], data->interface_mac_address[5],
            IPv4_GET_HEADER_VERSION(hdr)
        );
        return OBOS_STATUS_UNIMPLEMENTED;
    }
    if (IPv4_GET_HEADER_LENGTH(hdr) < 5)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has header size less than 20. Dropping packet.\n", 
            data->interface_mac_address[0], data->interface_mac_address[1], data->interface_mac_address[2], 
            data->interface_mac_address[3], data->interface_mac_address[4], data->interface_mac_address[5]);
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (be16_to_host(hdr->packet_length) > data->sz)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid size (NOTE: Buffer overflow). Dropping packet.\n", 
            data->interface_mac_address[0], data->interface_mac_address[1], data->interface_mac_address[2], 
            data->interface_mac_address[3], data->interface_mac_address[4], data->interface_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (IPv4_GET_HEADER_LENGTH(hdr) > data->sz)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid header size (NOTE: Buffer overflow). Dropping packet.\n", 
            data->interface_mac_address[0], data->interface_mac_address[1], data->interface_mac_address[2], 
            data->interface_mac_address[3], data->interface_mac_address[4], data->interface_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (IPv4_GET_HEADER_LENGTH(hdr) > be16_to_host(hdr->packet_length))
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid header size (NOTE: Larger than header's packet length field). Dropping packet.\n", 
            data->interface_mac_address[0], data->interface_mac_address[1], data->interface_mac_address[2], 
            data->interface_mac_address[3], data->interface_mac_address[4], data->interface_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }

    tables* tables = (void*)(uintptr_t)data->interface_vn->data;
    
    ip_table_entry *entry = nullptr;
    LIST_FOREACH(entry, ip_table, &tables->table)
        if (entry->address.addr == hdr->dest_address.addr)
            break;
    
    obos_status status = OBOS_STATUS_UNIMPLEMENTED;
    if (!entry)
    {
        status = Net_IPv4ForwardPacket(data->interface_vn, hdr);
        goto out;
    }

    frame what = *data;
    what.buff += IPv4_GET_HEADER_LENGTH(hdr);
    what.sz = be16_to_host(hdr->packet_length) - IPv4_GET_HEADER_LENGTH(hdr);
    what.base->refcount++;
    what.source_ip = hdr->src_address.addr;

    switch (hdr->protocol) {
        // UDP
        case 0x11:
            data->base->refcount++;
            status = Net_UDPReceiveFrame(&what, data, entry);
            break;
        default: break;
    }

    out:
    NetH_ReleaseSharedBuffer(data->base);
    return status;
}

obos_status Net_IPv4ForwardPacket(vnode* interface, ip_header* data)
{
    if (data->time_to_live <= 1)
    {
        // TODO: Send ICMP message telling the source that we timed out.

        return OBOS_STATUS_TIMED_OUT;
    }
    data->time_to_live--;
    data->chksum = 0;
    data->chksum = host_to_be16(Net_IPChecksum(data));
    return Net_TransmitIPv4Packet(interface, data);
}

static void submit_arp_request(vnode* interface, ip_addr ip, ip_addr source)
{
    tables* tables = (void*)(uintptr_t)interface->data;

    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);

    arp_header* arp_hdr = nullptr;
    Net_FormatARPRequestIPv4(&arp_hdr, tables->interface_mac, source, ip);
    const size_t arp_size = sizeof(*arp_hdr)+arp_hdr->len_hw_address*2+arp_hdr->len_protocol_address*2;

    mac_address dest = BROADCAST_MAC_ADDRESS;
    ethernet2_header* hdr = nullptr;
    size_t frame_size = 0;
    Net_FormatEthernet2Packet(&hdr, 
                              arp_hdr, arp_size, 
                              &dest, &tables->interface_mac, 
                              ETHERNET2_TYPE_ARP, 
                              &frame_size);
    
    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
    interface->un.device->driver->header.ftable.write_sync(interface->desc, hdr, frame_size, 0, nullptr);

    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, hdr, frame_size);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, arp_hdr, arp_size);

}

static obos_status resolve_mac(vnode* interface, ip_addr ip, ip_addr source, mac_address* out)
{
    // OBOS_Debug("source: %d.%d.%d.%d\n", source.comp1, source.comp2, source.comp3, source.comp4);
    // OBOS_Debug("requested ip: %d.%d.%d.%d\n", ip.comp1, ip.comp2, ip.comp3, ip.comp4);

    tables* tables = (void*)(uintptr_t)interface->data;

    Core_WaitOnObject(WAITABLE_OBJECT(tables->gateway_lock));
    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
    if (ip.addr == tables->gateway_entry->address.addr && tables->gateway_phys)
    {
        // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
        memcpy(*out, tables->gateway_phys->phys, sizeof(mac_address));
        Core_EventSet(&tables->gateway_lock, true);
        return OBOS_STATUS_SUCCESS;
    }
    Core_EventSet(&tables->gateway_lock, true);

    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);

    address_table_entry what = {.addr={.addr=ip.addr}};
    Core_RwLockAcquire(&tables->address_to_phys_lock, true);
    address_table_entry* ent = RB_FIND(address_table, &tables->address_to_phys, &what);
    Core_RwLockRelease(&tables->address_to_phys_lock, true);
    if (ent)
    {
        // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
        Core_WaitOnObject(WAITABLE_OBJECT(tables->gateway_lock));
        if (ip.addr == tables->gateway_address.addr)
            tables->gateway_phys = ent;
        Core_EventSet(&tables->gateway_lock, true);
        memcpy(*out, tables->gateway_phys->phys, sizeof(mac_address));
        return OBOS_STATUS_SUCCESS;
    }

    arp_ip_request *req = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(arp_ip_request), nullptr);
    req->evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    req->target = ip;
    req->interface = interface;
    Core_MutexAcquire(&Net_InFlightARPRequestsLock);
    RB_INSERT(arp_ip_request_tree, &Net_InFlightARPRequests, req);
    Core_MutexRelease(&Net_InFlightARPRequestsLock);

    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
    submit_arp_request(interface, ip, source);
    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
    Core_WaitOnObject(WAITABLE_OBJECT(req->evnt));
    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
    
    ent = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(arp_ip_request), nullptr);
    ent->addr = ip;
    memcpy(*out, req->response, sizeof(mac_address));
    memcpy(ent->phys, req->response, sizeof(mac_address));
    Core_RwLockAcquire(&tables->address_to_phys_lock, false);
    RB_INSERT(address_table, &tables->address_to_phys, ent);
    Core_RwLockRelease(&tables->address_to_phys_lock, false);

    Core_WaitOnObject(WAITABLE_OBJECT(tables->gateway_lock));
    // OBOS_Debug("%s:%d\n", __FILE__, __LINE__);
    if (ip.addr == tables->gateway_address.addr)
        tables->gateway_phys = ent;
    Core_EventSet(&tables->gateway_lock, true);

    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, req, sizeof(*req));

    return OBOS_STATUS_SUCCESS;
}

static obos_status get_destination_mac(vnode *interface, ip_addr ip, mac_address *out)
{
    tables* tables = (void*)(uintptr_t)interface->data;

    ip_table_entry *entry = nullptr;
    LIST_FOREACH(entry, ip_table, &tables->table)
        if ((ip.addr & BITS(0, entry->subnet_mask)) == entry->address.addr)
            break;
    // OBOS_Debug("entry = %p\n", entry);
    if (entry)
        return resolve_mac(interface, ip,  entry->address, out);
    if (!tables->gateway_entry)
        return OBOS_STATUS_HOST_UNREACHABLE;

    Core_WaitOnObject(WAITABLE_OBJECT(tables->gateway_lock));
    ip_addr gateway_address = tables->gateway_address;
    ip_addr source_address = tables->gateway_entry->address;
    Core_EventSet(&tables->gateway_lock, true);

    return resolve_mac(interface, gateway_address, source_address, out);
}

obos_status Net_TransmitIPv4Packet(vnode* interface, ip_header* hdr)
{
    tables* tables = (void*)(uintptr_t)interface->data;
    if (!tables)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (tables->magic != IP_TABLES_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    ip_addr destination_ip = hdr->dest_address;
    mac_address destination = {};
    get_destination_mac(interface, destination_ip, &destination);

    ethernet2_header* eth_hdr = nullptr;
    size_t frame_size = 0;
    Net_FormatEthernet2Packet(&eth_hdr, 
                              hdr, be16_to_host(hdr->packet_length), 
                              &destination, &tables->interface_mac, 
                              ETHERNET2_TYPE_IPv4, 
                              &frame_size);

    obos_status status = interface->un.device->driver->header.ftable.write_sync(interface->desc, eth_hdr, frame_size, 0, nullptr);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, eth_hdr, frame_size);
    return status;
}

LIST_GENERATE(frame_queue, frame, node);
