/*
 * drivers/generic/slowfat/alloc.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include "structs.h"

// All functions in this file, unless otherwise specified, should take the fat lock in the fat_cache struct.
// returns UINT32_MAX if no sector was found
uint32_t AllocateClusters(fat_cache* volume, size_t nClusters);
// Returns true if the cluster region was extended, otherwise you need to reallocate the clusters.
bool ExtendClusters(fat_cache* volume, uint32_t cluster, size_t nClusters, size_t oldClusterCount);
void TruncateClusters(fat_cache* volume, uint32_t cluster, size_t newClusterCount, size_t oldClusterCount);
void FreeClusters(fat_cache* volume, uint32_t cluster, size_t nClusters);
void InitializeCacheFreelist(fat_cache* volume);