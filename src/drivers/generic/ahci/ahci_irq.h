/*
 * drivers/generic/ahci/ahci_irq.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <driver_interface/pci.h>

bool ahci_irq_checker(struct irq* i, void* userdata);
void ahci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql);
extern pci_resource* PCIIrqResource;
