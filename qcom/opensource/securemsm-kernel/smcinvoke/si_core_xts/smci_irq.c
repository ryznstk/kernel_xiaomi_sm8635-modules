// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
#include <linux/firmware/qcom/qcom_scm.h>
#else
#include <linux/qcom_scm.h>
#endif
#include <linux/dma-buf.h>
#include <linux/qtee_shmbridge.h>
#include <linux/firmware/qcom/si_core_xts.h>
#include <linux/firmware/qcom/si_object.h>
#include "smcinvoke_object.h"

#define NUM_PAGES	1
#define IRQ_KT_SLEEP	0
#define IRQ_KT_WAKE	1
#define SI_OBJECT_OP_CALLRUNNABLE	0
#define SI_DOORBELL_UID	439
#define SI_REGISTER_TASK_RUNNER_UID	437

struct doorbell_mo_info {
	struct si_object *object;
	void *vaddr;
	phys_addr_t paddr;
	size_t size;
};

struct task_runner_cbo {
	struct si_object object;
};

struct worker_kthread {
	atomic_t counter_wq;
	wait_queue_head_t kthread_wq;
	struct task_struct *kthread_task;
};

typedef struct doorbell_msg {
	uint32_t tag;
	uint32_t len;
	uint8_t buff[];
} doorbell_msg;

static struct worker_kthread irq_worker;
static struct si_object_invoke_ctx oic;
static struct si_object *doorbell_service_obj, *task_runner_service_obj;
struct doorbell_mo_info *mo_info;


#define to_task_runner_cbo(o) container_of((o), struct task_runner_cbo, object)

static int op_callrunnable(struct si_arg args[])
{
	if (size_of_arg(args) != 1 || args[0].type != SI_AT_IO)
		return -EINVAL;

	int ret, result = 0;
	static struct si_object_invoke_ctx oic;
	struct si_arg in_args[1] = { 0 };

	/* invocation on input object of type IRunnable, IRunnable_OP_run is 0 */
	ret = si_object_do_invoke(&oic, args[0].o, 0, in_args, &result);
	if (ret || result) {
		pr_err("%s failed with result %d(ret = %d).\n", __func__, result, ret);
		return -EINVAL;
	}

	return 0;
}

static void task_runner_cbo_notify(unsigned int context_id, struct si_object *object, int status)
{

}

static void task_runner_cbo_release(struct si_object *object)
{
	struct task_runner_cbo *cbo_obj = to_task_runner_cbo(object);

	kfree(cbo_obj);
}

static int task_runner_cbo_dispatch(unsigned int context_id,
	struct si_object *object, unsigned long op, struct si_arg args[])
{
	int ret = 0;

	switch (ObjectOp_methodID(op)) {
	case SI_OBJECT_OP_CALLRUNNABLE: {
		ret = op_callrunnable(args);
	}
		break;
	default:
		/* The operation is not supported! */
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct si_object_operations task_runner_cbo_ops = {
	.notify = task_runner_cbo_notify,
	.release = task_runner_cbo_release,
	.dispatch = task_runner_cbo_dispatch
};

static int irq_kthread_func(void *data)
{
	u32 doorbell_id = 0;
	u32 msg_id = 0;
	doorbell_msg *msg;
	void* buff;
	int ret;

        while (!kthread_should_stop()) {
		wait_event_interruptible(
			irq_worker.kthread_wq,
			kthread_should_stop() ||
			(atomic_read(&irq_worker.counter_wq) > 0));

		if(atomic_read(&irq_worker.counter_wq) > 0){
			pr_debug("reading the contents of doorbell msg buffer\n");
			if(!mo_info->vaddr) {
				pr_err("mo_info is null \n");
				return -1;
			}

			msg = (doorbell_msg *)mo_info->vaddr;
			buff = msg->buff;

			ret = process_doorbell_msg(buff);
			if(ret) {
				pr_err("process_doorbell_msg failed with %d!\n", ret);
				return -1;
			}

			/* Send ACK to notify QTEE doorbell msg has been consumed */
			ret = qcom_scm_invoke_ack_doorbell(doorbell_id, msg_id);
			if(ret) {
				pr_err("doorbell_ack scm call failed with %d!\n", ret);
				return -1;
			}

			atomic_sub(1, &irq_worker.counter_wq);
		}
	}

	pr_warn("irq_worker thread exiting.\n");
	return 0;
}

int irq_kthread_create(void)
{
	int ret;
	init_waitqueue_head(&irq_worker.kthread_wq);
	atomic_set(&irq_worker.counter_wq, 0);

	irq_worker.kthread_task = kthread_run(irq_kthread_func, NULL, "irq_worker_thread");
	if (IS_ERR(irq_worker.kthread_task)) {
		ret = PTR_ERR(irq_worker.kthread_task);
		pr_err("fail to create irq worker kthread, ret = %x\n", ret);
		return ret;
	}

	return 0;
}

void irq_kthread_destroy(void)
{
	kthread_stop(irq_worker.kthread_task);
}

static irqreturn_t qtee_irq_handler(int irq, void *data)
{
	atomic_add(1, &irq_worker.counter_wq);

	wake_up_interruptible(&irq_worker.kthread_wq);

	return IRQ_HANDLED;
}

static void doorbell_mo_release(void *private)
{
	struct doorbell_mo_info *mo_info = private;

	if (mo_info && mo_info->vaddr)
		free_pages_exact(mo_info->vaddr, mo_info->size);

	kfree(mo_info);
}

static int doorbell_mo_create(struct si_object **doorbell_mo)
{
	int ret;

	mo_info = kzalloc(sizeof(*mo_info), GFP_KERNEL);
	if (!mo_info) {
		return -ENOMEM;
	}

	mo_info->size = NUM_PAGES * PAGE_SIZE;

	mo_info->vaddr = alloc_pages_exact(mo_info->size, GFP_KERNEL);
	if (!mo_info->vaddr) {
		ret = -ENOMEM;
		goto exit_release;
	}

	mo_info->paddr = virt_to_phys(mo_info->vaddr);

	mo_info->object = init_si_mem_object(mo_info->paddr,
			mo_info->size,
			doorbell_mo_release,
			mo_info);
	if (!mo_info->object) {
		pr_err("init_si_mem_object failed.\n");
		ret =  -EINVAL;
		goto exit_release_vaddr;
	}

	*doorbell_mo = mo_info->object;

	get_si_object(mo_info->object);

	return 0;

exit_release_vaddr:
	free_pages_exact(mo_info->vaddr, mo_info->size);
exit_release:
	kfree(mo_info);
	mo_info = NULL;

	return ret;
}

static int qtee_task_runner_cbo_register(void)
{
	int ret, result;
	uint32_t flags = 0;
	struct si_object *client_env;
	struct task_runner_cbo *cbo_obj;

	ret = si_core_get_client_env(&oic, &client_env);
	if (ret) {
		pr_err("si_core_get_client_env failed (ret = %d).\n", ret);
		return ret;
	}

	ret = si_core_client_env_open(&oic, client_env, SI_REGISTER_TASK_RUNNER_UID, &task_runner_service_obj);
	if (ret) {
		pr_err("si_core_client_env_open failed (ret = %d).\n", ret);
		goto out_client;
	}

	cbo_obj = kzalloc(sizeof(*cbo_obj), GFP_KERNEL);
	if (!cbo_obj) {
		pr_err("failed to allocate memory (ret = %d).\n",ret);
		ret = -ENOMEM;
		goto out_service;
	}

	init_si_object_user(&cbo_obj->object, SI_OT_CB_OBJECT,
			    &task_runner_cbo_ops, "task_runner_cbo_obj");

	struct si_arg args[3] = { 0 };
	args[0].b = (struct si_buffer) { {&flags}, sizeof(flags) };
	args[0].type = SI_AT_IB;
	args[1].o = &cbo_obj->object;
	args[1].type = SI_AT_IO;
	args[2].type = SI_AT_END;

	/* IRegisterTaskRunner_OP_registerRunner is 0. */
	ret =  si_object_do_invoke(&oic, task_runner_service_obj, 0, args, &result);
	if (ret) {
		pr_err("unable to register task_runner_cbo with QTEE (ret = %d).\n", ret);
		goto out_service;
	}
	put_si_object(client_env);

	return 0;

out_service:
	put_si_object(task_runner_service_obj);
out_client:
	put_si_object(client_env);

	return ret;
}

static int qtee_doorbell_register(uint32_t irq)
{
	int ret, result;
	struct si_object *client_env, *doorbell_mo;

	ret = si_core_get_client_env(&oic, &client_env);
	if (ret) {
		pr_err("si_core_get_client_env failed (ret = %d).\n", ret);
		return ret;
	}

	ret = si_core_client_env_open(&oic, client_env, SI_DOORBELL_UID, &doorbell_service_obj);
	if (ret) {
		pr_err("si_core_client_env_open failed (ret = %d).\n", ret);
		goto out_client;
	}

	ret = doorbell_mo_create(&doorbell_mo);
	if (ret) {
		pr_err("failed to create memobj (ret = %d).\n",ret);
		goto out_service;
	}

	struct si_arg args[3] = { 0 };
	args[0].b = (struct si_buffer) { {&irq}, sizeof(irq) };
	args[0].type = SI_AT_IB;
	args[1].o = doorbell_mo;
	args[1].type = SI_AT_IO;
	args[2].type = SI_AT_END;

	/* IDoorbell_OP_registerInterrupt is 0. */
	ret =  si_object_do_invoke(&oic, doorbell_service_obj, 0, args, &result);
	if (ret) {
		pr_err("unable to register doorbell with QTEE (ret = %d).\n", ret);
		goto out_mo;
	}
	put_si_object(client_env);

	return 0;

out_mo:
	put_si_object(doorbell_mo);
out_service:
	put_si_object(doorbell_service_obj);
out_client:
	put_si_object(client_env);

	return ret;
}

int smci_irq_setup(struct platform_device *pdev)
{
	uint32_t irq;
	int ret = -1;
	struct device *dev = &pdev->dev;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq < 0) {
		ret = irq;
		return ret;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL, qtee_irq_handler,
			IRQF_ONESHOT, "qcom,smcinvoke", dev);
	if (ret < 0) {
		pr_err("failed to request qcom-scm irq (ret = %d).\n", ret);
		return ret;
	}

	/* platform_get_irq_optional returns linux virtual irq, send hw_irq to QTEE */
	struct irq_data *irqd = irq_get_irq_data(irq);
	uint32_t hw_irq = (uint32_t)irqd->hwirq;

	ret = qtee_doorbell_register(hw_irq);
	if (ret) {
		devm_free_irq(dev, irq, dev);
		return ret;
	}

	ret = qtee_task_runner_cbo_register();
	if (ret) {
		devm_free_irq(dev, irq, dev);
		return ret;
	}

	return 0;
}
