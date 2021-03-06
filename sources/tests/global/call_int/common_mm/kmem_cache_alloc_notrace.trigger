[group]
function.name = kmem_cache_alloc_notrace
trigger.code =>>
	void *p;
	struct kmem_cache* mem_cache = 
		kmem_cache_create("kedr_mem_cache", 32, 32, 0, NULL);

	if (mem_cache != NULL)
	{
		p = kmem_cache_alloc_notrace(mem_cache, GFP_KERNEL);
		if (p != NULL)
		{
			kmem_cache_free(mem_cache, p);
		}
		else
		{
			printk(KERN_INFO 
			"Cannot allocate object from our own kmem_cache.");
		}
		kmem_cache_destroy(mem_cache);
	}
	else
	{
		printk(KERN_INFO "Cannot create kmem_cache.");
	}
<<
