/*
 * drivers/generic/slowfat/alloc.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <driver_interface/header.h>

#include "structs.h"

// All functions in this file, unless otherwise specified, should take the fat lock in the fat_cache struct.
// returns UINT32_MAX if no sector was found
uint32_t AllocateClusters(fat_cache* volume, size_t nClusters);
// Returns true if the cluster region was extended, otherwise you need to reallocate the clusters.
bool ExtendClusters(fat_cache* volume, uint32_t cluster, size_t nClusters, size_t oldClusterCount);
void TruncateClusters(fat_cache* volume, uint32_t cluster, size_t newClusterCount, size_t oldClusterCount);
void FreeClusters(fat_cache* volume, uint32_t cluster, size_t nClusters);
void InitializeCacheFreelist(fat_cache* volume);

// if status is passed as OBOS_STATUS_SUCCESS, the cluster passed is valid.
// if status is passed as OBOS_STATUS_EOF, the cluster passed is valid, and is the last cluster of the chain.
// if status is passed as OBOS_STATUS_ABORTED, the cluster passed is not valid, as an error has occurred following the chain.
typedef iterate_decision(*clus_chain_cb)(uint32_t cluster, obos_status status, void* userdata);
obos_status NextCluster(fat_cache* cache, uint32_t cluster, uint8_t* sec_buf, uint32_t* ret);
uint32_t ClusterSeek(fat_cache* cache, uint32_t cluster, uint32_t nClusters);
void FollowClusterChain(fat_cache* volume, uint32_t clus, clus_chain_cb callback, void* userdata);