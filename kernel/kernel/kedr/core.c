/*
 * ========================================================================
 * Copyright (C) 2016-2018, Evgenii Shatokhin <eugene.shatokhin@yandex.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 * ========================================================================
 */

/*
 * Some parts of this code may be based on the implementation of livepatch
 * in the mainline kernel.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/ftrace.h>
#include <linux/list.h>
#include <linux/kallsyms.h>
#include <linux/rcupdate.h>
#include <linux/preempt.h>
#include <linux/kobject.h>

#include <linux/kedr.h>
/* ====================================================================== */

MODULE_AUTHOR("Evgenii Shatokhin");
MODULE_LICENSE("GPL");
/* ====================================================================== */

enum kedr_func_state {
	KEDR_FUNC_DISABLED,
	KEDR_FUNC_ENABLED,
	KEDR_FUNC_UNREGISTERED
};

struct kedr_object {
	struct list_head list;
	struct module *mod;	/* NULL for vmlinux, non-NULL for modules */
	struct list_head funcs;
};

struct kedr_func {
	struct list_head list;
	struct ftrace_ops ops;

	/* This handler will be called instead of the stub. */
	void *handler;

	/* Address of the stub the handler is attached to. */
	unsigned long addr;

	/* Information about the function: for messages, etc. */
	char *info;

	enum kedr_func_state state;
};
/* ====================================================================== */

/*
 * This mutex protects the global lists defined here with all the data they
 * refer to, as well as kedr_enabled variable.
 */
static DEFINE_MUTEX(kedr_mutex);

static LIST_HEAD(kedr_objects);

static bool kedr_enabled;
/* ====================================================================== */

/* Start and end addresses of the kernel code. */
unsigned long kedr_stext;
unsigned long kedr_etext;
/* ====================================================================== */

/*
 * Mark the event handlers with '__kedr_handler' (place it before the return
 * type). This way the definitions of the appropriate stubs will be
 * automatically generated in kedr_helpers.*.
 */
#define __kedr_handler

/*
 * Event handlers.
 * Preemption is disabled there, that allows us to use synchronize_sched()
 * later to wait for all running handlers to complete.
 */

/*
 * Called after the memory area has been allocated, gets the address and
 * size of the area as arguments. If the allocation has failed, 'addr'
 * will be 0.
 */
static __kedr_handler void kedr_handle_alloc(
	unsigned long addr, unsigned long size, void *loc)
{
	//struct kedr_local *local = loc;
	unsigned long pc;

	preempt_disable();
	// ??? is it needed to save pc in kedr_local? Each handler must get PC
	// on its own because KEDR core can attach to the kernel at any moment - 
	// no guarantee that a pre-handler has run if the post-handler is now running,
	// for example.
	pc = (unsigned long)__builtin_return_address(0);
	// TODO

	//<>
	pr_info("[DBG] alloc at %lx (%pS) for size %lu => addr %lx\n", pc, (void *)pc, size, addr);
	//<>
	preempt_enable();
}

/*
 * Memory deallocation handler.
 * Called before deallocation starts. 'addr' - the address of the memory
 * area to be freed. May be 0.
 */
static __kedr_handler void kedr_handle_free(unsigned long addr, void *loc)
{
	//struct kedr_local *local = loc;
	unsigned long pc;

	preempt_disable();
	pc = (unsigned long)__builtin_return_address(0);
	// TODO

	//<>
	pr_info("[DBG] free at %lx (%pS) for addr %lx\n", pc, (void *)pc, addr);
	//<>
	preempt_enable();
}

/*
 * The handlers for krealloc and __krealloc can be rather complex. Let
 * the KEDR core implement them for now.
 */
static __kedr_handler void kedr_handle_krealloc_pre(
	const void *p, unsigned long new_size, void *loc)
{
	//struct kedr_local *local = loc;
	unsigned long pc;

	preempt_disable();
	pc = (unsigned long)__builtin_return_address(0);
	// TODO
	preempt_enable();
}

static __kedr_handler void kedr_handle_krealloc_post(
	const void *ret, const void *p, unsigned long new_size, void *loc)
{
	//struct kedr_local *local = loc;
	unsigned long pc;

	preempt_disable();
	pc = (unsigned long)__builtin_return_address(0);
	// TODO
	preempt_enable();
}

/* This one is called after __krealloc() returns. */
static __kedr_handler void kedr_handle___krealloc(
	const void *ret, const void *p, unsigned long new_size, void *loc)
{
	//struct kedr_local *local = loc;
	unsigned long pc;

	preempt_disable();
	pc = (unsigned long)__builtin_return_address(0);
	// TODO
	preempt_enable();
}
/* ====================================================================== */

static void notrace kedr_ftrace_handler(unsigned long ip,
				       unsigned long parent_ip,
				       struct ftrace_ops *fops,
				       struct pt_regs *regs)
{
	struct kedr_func *func;

	func = container_of(fops, struct kedr_func, ops);
	kedr_arch_set_pc(regs, (unsigned long)func->handler);
}

/* ====================================================================== */

/* Note. mod == NULL corresponds to the kernel proper here. */
static struct kedr_object *kedr_find_object(struct module *mod)
{
	struct kedr_object *obj;

	list_for_each_entry(obj, &kedr_objects, list) {
		if (obj->mod == mod)
			return obj;
	}
	return NULL;
}

/* Note. We assume here that the object for 'mod' does not exist yet. */
static struct kedr_object *kedr_create_object(struct module *mod)
{
	struct kedr_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (obj) {
		obj->mod = mod;
		INIT_LIST_HEAD(&obj->funcs);
		list_add(&obj->list, &kedr_objects);
	}
	return obj;
}

static void kedr_destroy_func(struct kedr_func *func)
{
	if (!func)
		return;
	kfree(func->info);
	kfree(func);
}

static struct kedr_func *kedr_create_func(
	void (*handler)(struct kedr_local *local),
	unsigned long addr,
	const char *name,
	const char *module_name)
{
	struct kedr_func *func;
	int len;
	static const char *fmt = "%s at %p (%s)";

	func = kzalloc(sizeof(*func), GFP_KERNEL);
	if (!func)
		return NULL;

	len = snprintf(NULL, 0, fmt, name, (void *)addr, module_name) + 1;
	func->info = kzalloc(len, GFP_KERNEL);
	if (!func->info) {
		kfree(func);
		return NULL;
	}
	snprintf(func->info, len, fmt, name, (void *)addr, module_name);

	func->ops.func = kedr_ftrace_handler;
	func->ops.flags = FTRACE_OPS_FL_SAVE_REGS |
			  FTRACE_OPS_FL_DYNAMIC |
			  FTRACE_OPS_FL_IPMODIFY;
	func->handler = handler;
	func->addr = addr;
	func->state = KEDR_FUNC_DISABLED;
	return func;
}

/*
 * Convert a function address into the appropriate ftrace location.
 *
 * Usually this is just the address of the function, but on some architectures
 * it's more complicated so allow them to provide a custom behaviour.
 */
#ifndef kedr_get_ftrace_location
static unsigned long kedr_get_ftrace_location(unsigned long faddr)
{
	return faddr;
}
#endif

static int kedr_func_detach(struct kedr_func *func)
{
	unsigned long ftrace_loc;
	int ret;

	if (func->state != KEDR_FUNC_ENABLED) {
		pr_info(KEDR_PREFIX
			"Handler for the function %s is not enabled.\n",
			func->info);
		return -EINVAL;
	}

	ftrace_loc = kedr_get_ftrace_location(func->addr);
	if (!ftrace_loc) {
		pr_err(KEDR_PREFIX
			"Failed to find ftrace hook for the function %s\n",
			func->info);
		return -EINVAL;
	}

	/*
	 * If the previous attempts to detach the function failed in
	 * ftrace_set_filter_ip(), do not try to unregister the function
	 * again.
	 */
	if (func->state != KEDR_FUNC_UNREGISTERED) {
		ret = unregister_ftrace_function(&func->ops);
		if (ret) {
			pr_warning(KEDR_PREFIX
				"Failed to unregister ftrace handler for function %s (error: %d)\n",
				func->info, ret);
			return ret;
		}
	}
	func->state = KEDR_FUNC_UNREGISTERED;

	ret = ftrace_set_filter_ip(&func->ops, ftrace_loc, 1, 0);
	if (ret) {
		pr_warning(KEDR_PREFIX
			"Failed to remove ftrace filter for function %s (error: %d)\n",
			func->info, ret);
		return ret;
	}

	return ret;
}

static int kedr_func_attach(struct kedr_func *func)
{
	unsigned long ftrace_loc;
	int ret;

	if (func->state != KEDR_FUNC_DISABLED) {
		pr_err(KEDR_PREFIX
		       "Handler for the function %s is already enabled.\n",
		       func->info);
		return -EINVAL;
	}

	ftrace_loc = kedr_get_ftrace_location(func->addr);
	if (!ftrace_loc) {
		pr_err(KEDR_PREFIX
			"Failed to find ftrace hook for the function %s\n",
			func->info);
		return -EINVAL;
	}

	ret = ftrace_set_filter_ip(&func->ops, ftrace_loc, 0, 0);
	if (ret) {
		pr_warning(KEDR_PREFIX
			   "Failed to set ftrace filter for function %s (error: %d)\n",
			   func->info, ret);
		return -EINVAL;
	}

	ret = register_ftrace_function(&func->ops);
	if (ret) {
		pr_warning(
			KEDR_PREFIX
			"Failed to register ftrace handler for function %s (error: %d)\n",
			func->info, ret);
		ftrace_set_filter_ip(&func->ops, ftrace_loc, 1, 0);
		return -EINVAL;
	}

	func->state = KEDR_FUNC_ENABLED;
	return 0;
}

/*
 * Detaches all handlers attached via this object, if any. Frees all the
 * memory allocated for the respective kedr_func instances.
 * Does not free the object itself.
 */
static int kedr_cleanup_object(struct kedr_object *obj)
{
	struct kedr_func *func;
	struct kedr_func *tmp;
	int ret;

	list_for_each_entry_safe(func, tmp, &obj->funcs, list) {
		ret = kedr_func_detach(func);
		if (ret)
			return ret;
		list_del(&func->list);
		kedr_destroy_func(func);
	}
	return 0;
}

static int kedr_destroy_all_objects(void)
{
	struct kedr_object *obj;
	struct kedr_object *tmp;
	int ret;

	list_for_each_entry_safe(obj, tmp, &kedr_objects, list) {
		ret = kedr_cleanup_object(obj);
		if (ret)
			return ret;
		list_del(&obj->list);
		kfree(obj);
	}
	return 0;
}

struct kedr_handler_table_item
{
	const char *event_name;
	void *handler;
};

static struct kedr_handler_table_item handler_table[] = {
	{
		.event_name	= "alloc",
		.handler	= kedr_handle_alloc,
	},
	{
		.event_name	= "free",
		.handler	= kedr_handle_free,
	},
	{
		.event_name	= "krealloc_pre",
		.handler	= kedr_handle_krealloc_pre,
	},
	{
		.event_name	= "krealloc_post",
		.handler	= kedr_handle_krealloc_post,
	},
	{
		.event_name	= "__krealloc",
		.handler	= kedr_handle___krealloc,
	},
};

static void *find_handler(const char *event_name)
{
	size_t i;

	/* TODO: may be optimize the lookup somehow? */
	for (i = 0; i < ARRAY_SIZE(handler_table); ++i) {
		if (strcmp(event_name, handler_table[i].event_name) == 0)
			return handler_table[i].handler;
	}

	return NULL;
}

static int kedr_kallsyms_callback(void *data, const char *name,
				  struct module *mod, unsigned long addr)
{
	static const char stub_prefix[] = "kedr_stub_handle_";
	static size_t prefix_len = ARRAY_SIZE(stub_prefix) - 1;
	struct module *target = data;
	struct kedr_object *obj;
	void *handler;
	struct kedr_func *func;

	/*
	 * If 'target' is NULL, we need to check all the symbols.
	 * If 'target' is non-NULL, it specifies the module we are
	 * interested in.
	 */
	if (target && target != mod)
		return 0;

	if (strncmp(name, stub_prefix, prefix_len) != 0)
		return 0;

	handler = find_handler(name + prefix_len);
	if (!handler) {
		pr_info(KEDR_PREFIX "Unknown KEDR stub \"%s\" in %s.\n",
			name, module_name(mod));
		return 0;
	}

	obj = kedr_find_object(mod);
	if (!obj) {
		obj = kedr_create_object(mod);
		if (!obj)
			return -ENOMEM;
	}

	func = kedr_create_func(handler, addr, name, module_name(mod));
	if (!func)
		return -ENOMEM;

	list_add(&func->list, &obj->funcs);
	return 0;
}

/*
 * Detach the handlers from the KEDR stubs in the given module (if mod is
 * not NULL) or everywhere (if mod is NULL).
 */
static int kedr_detach_handlers(struct module *mod)
{
	struct kedr_object *obj;
	int ret;

	if (!mod)
		return kedr_destroy_all_objects();

	obj = kedr_find_object(mod);
	if (obj) {
		ret = kedr_cleanup_object(obj);
		if (ret)
			return ret;
		list_del(&obj->list);
		kfree(obj);
	}
	return 0;
}

static int kedr_attach_all_for_object(struct kedr_object *obj)
{
	struct kedr_func *func;
	int ret = 0;

	list_for_each_entry(func, &obj->funcs, list) {
		if (func->state != KEDR_FUNC_DISABLED)
			continue;
		ret = kedr_func_attach(func);
		if (ret)
			break;
	}
	return ret;
}

/*
 * Find KEDR stubs in the code and attach the appropriate handlers to them.
 * If mod is non-NULL, search the given module only, otherwise search
 * everywhere.
 */
static int kedr_attach_handlers(struct module *mod)
{
	int ret;
	struct kedr_object *obj;

	/*
	 * This is unlikely but possible in case KEDR failed to detach from
	 * a module completely when that module was unloaded, and now it is
	 * loaded once again.
	 */
	if (mod && kedr_find_object(mod)) {
		pr_err(KEDR_PREFIX
		       "Unable to attach handlers to the reloaded module %s.\n",
			module_name(mod));
		return -EBUSY;
	}

	ret = mutex_lock_killable(&module_mutex);
	if (ret)
		return ret;

	ret = kallsyms_on_each_symbol(kedr_kallsyms_callback, mod);
	mutex_unlock(&module_mutex);
	if (ret)
		return ret;

	/*
	 * Ftrace code may lock module_mutex too, e.g., when calling
	 * set_all_modules_text_rw(), so we cannot attach the handlers
	 * in the kallsyms callback itself. Do it here instead.
	 */
	if (mod) {
		obj = kedr_find_object(mod);
		if (obj)
			ret = kedr_attach_all_for_object(obj);
	}
	else {
		list_for_each_entry(obj, &kedr_objects, list) {
			ret = kedr_attach_all_for_object(obj);
			if (ret)
				break;
		}
	}

	if (ret)
		kedr_detach_handlers(mod);

	return ret;
}
/* ====================================================================== */

static int kedr_module_notify(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	struct module *mod = data;
	int ret = 0;

	/* Do not let this code trip over itself. */
	if (mod == THIS_MODULE)
		return 0;

	/*
	 * We check kedr_enabled here just in case this notification came
	 * right before KEDR was disabled. kedr_mutex is used to serialize
	 * the events w.r.t. enabling/disabling KEDR.
	 */
	mutex_lock(&kedr_mutex);
	if (!kedr_enabled) {
		mutex_unlock(&kedr_mutex);
		return 0;
	}

	switch(action) {
	case MODULE_STATE_COMING:
		kedr_modmap_on_coming(mod);
		ret = kedr_attach_handlers(mod);
		if (ret)
			pr_warning(
				KEDR_PREFIX
				"Failed to attach handlers to \"%s\", errno: %d.\n",
				module_name(mod), ret);
		break;
	case MODULE_STATE_LIVE:
		/* Handle unloading of the module's init area here, if needed. */
		break;
	case MODULE_STATE_GOING:
		ret = kedr_detach_handlers(mod);
		if (ret)
			pr_warning(
				KEDR_PREFIX
				"Failed to detach handlers from \"%s\", errno: %d.\n",
				module_name(mod), ret);
		break;
	default:
		break;
	}

	mutex_unlock(&kedr_mutex);
	return ret;
}

static struct notifier_block kedr_module_nb = {
	.notifier_call = kedr_module_notify,
	.priority = -1, /* let others do their work first */
};
/* ====================================================================== */

/* Set up and enable event handling. */
static int kedr_enable(void)
{
	int ret;

	ret = mutex_lock_killable(&kedr_mutex);
	if (ret) {
		pr_warning(KEDR_PREFIX "Failed to lock kedr_mutex.\n");
		return ret;
	}

	if (kedr_enabled)
		goto out;

	/*
	 * Make sure the core module cannot be unloaded while the events
	 * are enabled.
	 */
	if (!try_module_get(THIS_MODULE)) {
		ret = -EBUSY;
		goto out;
	}

	ret = kedr_attach_handlers(NULL);
	if (ret) {
		module_put(THIS_MODULE);
		goto out;
	}

	kedr_create_modmap();

	kedr_enabled = true;
	pr_debug(KEDR_PREFIX "KEDR has been enabled.\n");

out:
	mutex_unlock(&kedr_mutex);
	return ret;
}

/* Disable event handling. */
static int kedr_disable(void)
{
	int ret = 0;

	mutex_lock(&kedr_mutex);
	if (!kedr_enabled)
		goto out;

	ret = kedr_detach_handlers(NULL);
	if (ret)
		goto out;

	kedr_enabled = false;

	/*
	 * We have detached the handlers, they will no longer start unless
	 * re-attached.
	 *
	 * However, some handlers might have already started before they
	 * were detached, so let us wait for them to finish.
	 *
	 * The handlers disable preemption, so synchronize_sched() should
	 * do the trick here.
	 */
	synchronize_sched();

	/*
	 * ? Is it possible for a handler to be preempted before it has
	 * called preempt_disable() and resume after synchronize_sched()
	 * has already completed? I suppose it is not but I cannot prove it
	 * yet.
	 *
	 * If is it possible though, we need some other means to make sure
	 * the handlers are not running and will not start at this point,
	 * before we cleanup the resources the handlers might use.
	 */

	kedr_free_modmap();

	module_put(THIS_MODULE);
	pr_debug(KEDR_PREFIX "KEDR has been disabled.\n");
out:
	mutex_unlock(&kedr_mutex);
	return ret;
}
/* ====================================================================== */

/* sysfs knobs */
static struct kobject *kedr_kobj;

static ssize_t kedr_enabled_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	ssize_t len;
	int ret;

	ret = mutex_lock_killable(&kedr_mutex);
	if (ret)
		return ret;

	len = sprintf(buf, "%d\n", kedr_enabled);
	mutex_unlock(&kedr_mutex);

	return len;
}

static ssize_t kedr_enabled_store(struct kobject *kobj,
				  struct kobj_attribute *attr, const char *buf,
				  size_t count)
{
	int ret;
	unsigned long enable;

	ret = kstrtoul(buf, 10, &enable);
	if (ret)
		return ret;

	enable = !!enable;

	if (enable)
		ret = kedr_enable();
	else
		ret = kedr_disable();

	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute kedr_enabled_attr =
	__ATTR(enabled, 0644, kedr_enabled_show, kedr_enabled_store);

static struct attribute *kedr_attrs[] = {
	&kedr_enabled_attr.attr,
	NULL,
};

static struct attribute_group kedr_attr_group = {
	.attrs = kedr_attrs,
};
/* ====================================================================== */

/*
 * Find the non-exported kernel symbols that KEDR needs. Ugly, but should
 * be OK for now.
 */
static int __init find_kernel_symbols(void)
{
	/*
	 * Note. .text section of the kernel starts from '_text' rather than
	 * '_stext' (_stext > _text, by the way). This is the case for both
	 * 32- and 64-bit x86 and might be for arm & arm64 as well.
	 */
	kedr_stext = (unsigned long)kallsyms_lookup_name("_text");
	if (!kedr_stext) {
		pr_warning(KEDR_PREFIX "Kernel symbol not found: _text\n");
		return -EINVAL;
	}

	kedr_etext = (unsigned long)kallsyms_lookup_name("_etext");
	if (!kedr_etext) {
		pr_warning(KEDR_PREFIX "Kernel symbol not found: _etext\n");
		return -EINVAL;
	}
	return 0;
}
/* ====================================================================== */

static int __init kedr_init(void)
{
	int ret;

	ret = find_kernel_symbols();
	if (ret)
		return ret;

	ret = register_module_notifier(&kedr_module_nb);
	if (ret) {
		pr_warning(KEDR_PREFIX
			   "Failed to register the module notifier.\n");
		return ret;
	}

	kedr_kobj = kobject_create_and_add("kedr", kernel_kobj);
	if (!kedr_kobj) {
		ret = -ENOMEM;
		goto out_unreg;
	}

	ret = sysfs_create_group(kedr_kobj, &kedr_attr_group);
	if (ret)
		goto out_put;

	// TODO: other initialization tasks.

	return 0;

out_put:
	kobject_put(kedr_kobj);
out_unreg:
	unregister_module_notifier(&kedr_module_nb);
	return ret;
}

static void __exit kedr_exit(void)
{
	sysfs_remove_group(kedr_kobj, &kedr_attr_group);
	kobject_put(kedr_kobj);

	/*
	 * Just in case someone has re-enabled it after the core module
	 * began to unload.
	 */
	kedr_disable();

	unregister_module_notifier(&kedr_module_nb);
	return;
}

module_init(kedr_init);
module_exit(kedr_exit);
/* ====================================================================== */


/*void notrace kedr_thunk_kmalloc_pre(unsigned long size,
				    struct kedr_local *local)
{
	if (!local)
		return;

	local->pc = (unsigned long)__builtin_return_address(0);
	local->size = size;

	if (size == 0)
		return;

	kedr_stub_alloc_pre(local);
}*/
/* ====================================================================== */