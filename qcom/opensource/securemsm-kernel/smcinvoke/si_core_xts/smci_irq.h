// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __SMCI_IRQ_H__
#define __SMCI_IRQ_H__

int smci_irq_setup(struct platform_device *pdev);
int irq_kthread_create(void);
void irq_kthread_destroy(void);

#endif /* __SMCI_IRQ_H__ */
