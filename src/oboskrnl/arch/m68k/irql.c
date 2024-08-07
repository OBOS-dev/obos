/*
 * oboskrnl/arch/m68k/irql.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

// Emulates IRQLs for the m68k.

#include <int.h>

#include <scheduler/cpu_local.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <arch/m68k/asm_helpers.h>
#include <arch/m68k/cpu_local_arch.h>

irql* Core_GetIRQLVar();

void Arch_SetHardwareIPL(uint8_t to)
{
    to &= 0b111;
    setSR(getSR() | (to << 8));
}
uint8_t Arch_GetHardwareIPL()
{
    return (getSR() << 8) & 0b111;
}
bool CoreS_EnterIRQHandler(interrupt_frame *frame)
{
    m68k_dirq* irqs = CoreS_GetCPULocalPtr()->arch_specific.irqs;
    m68k_dirq_list* deferred = &CoreS_GetCPULocalPtr()->arch_specific.deferred;
    if (*Core_GetIRQLVar() >= (frame->intNumber / 32))
    {
        // IRQL >= irq means that the IRQ should be defferred
        if (!irqs[frame->intNumber].nDefers)
        {
            // Add it to the list
            Arch_SetHardwareIPL(7); // 7=masked
            m68k_dirq *node = &irqs[frame->intNumber];
            if (deferred->tail && deferred->tail->irql < node->irql)
            {
                // Append it
                if (deferred->tail)
                    deferred->tail->next = node;
                if (!deferred->tail)
                    deferred->tail = node;
                node->prev = deferred->tail;
                deferred->tail = node;
                deferred->nNodes++;
            }
            else if (deferred->tail && deferred->tail->irql > node->irql)
            {
                // Prepend it
                if (deferred->tail)
                    deferred->tail->prev = node;
                if (!deferred->tail)
                    deferred->tail = node;
                node->next = deferred->tail;
                deferred->tail = node;
                deferred->nNodes++;
            }
            else
            {
                // Find the node that this node should go after.
                m68k_dirq* found = nullptr, * n = deferred->tail;
                while (n && n->next)
                {
                    if (n->irql < node->irql &&
                        n->next->irql > node->irql)
                    {
                        found = n;
                        break;
                    }

                    n = n->next;
                }
                if (!found)
                {
                    // Append it
                    if (deferred->tail)
                        deferred->tail->next = node;
                    if (!deferred->tail)
                        deferred->tail = node;
                    node->prev = deferred->tail;
                    deferred->tail = node;
                    deferred->nNodes++;
                    goto done;
                }
                if (found->next)
                    found->next->prev = node;
                if (deferred->tail == found)
                    deferred->tail = node;
                node->next = found->next;
                found->next = node;
                node->prev = found;
                deferred->nNodes++;
            }
            done:
            Arch_SetHardwareIPL(0);
        }
        irqs[frame->intNumber].nDefers++; // the amount of times the IRQ has occurred.
        return false;
    }
    if (irqs[frame->intNumber].nDefers && !(--irqs[frame->intNumber].nDefers))
    {
        // Last defer.
        if (&irqs[frame->intNumber] == deferred->head)
            deferred->head = irqs[frame->intNumber].next;
        if (&irqs[frame->intNumber] == deferred->tail)
            deferred->tail = irqs[frame->intNumber].prev;
        if (irqs[frame->intNumber].prev)
            irqs[frame->intNumber].prev->next = irqs[frame->intNumber].next;
        if (irqs[frame->intNumber].next)
            irqs[frame->intNumber].next->prev = irqs[frame->intNumber].prev;
        deferred->nNodes--;
    }
    Arch_SetHardwareIPL(0);
    return true; // it can run.
}
void CoreS_ExitIRQHandler(interrupt_frame *frame)
{
    OBOS_UNUSED(frame);
    Arch_SetHardwareIPL(7);
    return;
}
// TODO:
OBOS_WEAK extern void Arch_CallDeferredIrq(m68k_dirq* irq);
void CoreS_SetIRQL(uint8_t to)
{
    if (to == IRQL_MASKED)
        Arch_SetHardwareIPL(7); // avoid a lot of needless deferring.
    else
        Arch_SetHardwareIPL(0);
    if (to >= *Core_GetIRQLVar())
        return;
    m68k_dirq_list* deferred = &CoreS_GetCPULocalPtr()->arch_specific.deferred;
    // If to < current irql, run all defferred IRQs.
    // Start at the highest priority IRQs.
    for (m68k_dirq* curr = deferred->tail; curr; )
    {
        for (size_t i = 0; i < curr->nDefers; i++)
            Arch_CallDeferredIrq(curr);
        curr = curr->prev;
    }
}
uint8_t CoreS_GetIRQL()
{
    return *Core_GetIRQLVar();
}