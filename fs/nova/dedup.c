#include "nova.h"
#include "inode.h"
#include "dedup.h"

/******************** FACT DRAM data structure *****************/
struct DeNOVA_bm *FACT_free_list; // For allocating new  indirect access area 

/******************** DEDUP QUEUE ********************/
struct nova_dedup_queue dqueue;

// Initialize Dedup Queue
int nova_dedup_queue_init(void){
	INIT_LIST_HEAD(&dqueue.head.list);
	mutex_init(&dqueue.lock);
	dqueue.head.write_entry_address = 0;
	return 0;
}

// Insert Write Entries to Dedup Queue
int nova_dedup_queue_push(u64 new_address, u64 target_inode_number){
	struct nova_dedup_queue_entry *new_data;

	mutex_lock(&dqueue.lock);
	new_data = kmalloc(sizeof(struct nova_dedup_queue_entry), GFP_KERNEL);
	list_add_tail(&new_data->list, &dqueue.head.list);
	new_data->write_entry_address = new_address;
	new_data->target_inode_number = target_inode_number;
	mutex_unlock(&dqueue.lock);

	//printk("dqueue-PUSH(Write Entry Address: %llu, Inode Number: %llu)\n",new_address,target_inode_number);
	return 0;
}

// Get next write entry to dedup
u64 nova_dedup_queue_get_next_entry(u64 *target_inode_number){
	struct nova_dedup_queue_entry *ptr;
	u64 ret = 0;

	mutex_lock(&dqueue.lock);
	if(!list_empty(&dqueue.head.list)){
		ptr = list_entry(dqueue.head.list.next, struct nova_dedup_queue_entry, list);

		ret = ptr->write_entry_address;
		*target_inode_number = ptr->target_inode_number;

		list_del(dqueue.head.list.next);
		kfree(ptr);
		//printk("dqueue-POP(Write Entry Address: %llu, Inode Number: %llu)\n",ret,*target_inode_number);
	}
	mutex_unlock(&dqueue.lock);
	return ret;
}

/******************** SHA1 ********************/
static struct sdesc *init_sdesc(struct crypto_shash *alg)
{
	struct sdesc *sdesc;
	int size;

	size = sizeof(struct shash_desc) + crypto_shash_descsize(alg);
	sdesc = kmalloc(size, GFP_KERNEL);
	if (!sdesc)
		return ERR_PTR(-ENOMEM);
	sdesc->shash.tfm = alg;
	sdesc->shash.flags = 0x0;
	return sdesc;
}
static int calc_hash(struct crypto_shash *alg,
		const unsigned char *data, unsigned int datalen,
		unsigned char *digest)
{
	struct sdesc *sdesc;
	int ret;

	sdesc = init_sdesc(alg);
	if (IS_ERR(sdesc)) {
		pr_info("can't alloc sdesc\n");
		return PTR_ERR(sdesc);
	}

	ret = crypto_shash_digest(&sdesc->shash, data, datalen, digest);
	kfree(sdesc);
	return ret;
}
int nova_dedup_fingerprint(unsigned char* datapage, unsigned char * ret_fingerprint){
	struct crypto_shash *alg;
	char *hash_alg_name = "sha1";
	int ret;

	alg = crypto_alloc_shash(hash_alg_name,0,0);
	if(IS_ERR(alg)){
		pr_info("can't alloc alg %s\n",hash_alg_name);
		return PTR_ERR(alg);
	}
	ret = calc_hash(alg,datapage,DATABLOCK_SIZE,ret_fingerprint);
	crypto_free_shash(alg);

	return ret;
}

int nova_dedup_compare_fingerprint(unsigned char* a, unsigned char *b){
	int ret=0;
	int i;
	for(i=0;i<FINGERPRINT_SIZE;i++){
		if(a[i] != b[i])
			ret = 1;
	}
	return ret;
}

int nova_dedup_copy_fingerprint(unsigned char*src, unsigned char *dst){
	int i;
	for(i=0;i<FINGERPRINT_SIZE;i++)
		dst[i]=src[i];
	return 0;
}

/******************** Check Integrity of Inode, Write Entry, Data page ********************/
// Cross check if 'Inode', 'WriteEntry', 'Datapage' was invalidated
// Return 1 if Inode-writeentry-datapage is all valid
int nova_dedup_crosscheck(struct nova_file_write_entry *entry
		,struct nova_inode_info_header *sih, unsigned long pgoff){
	struct nova_file_write_entry *referenced_entry;
	void ** pentry;
	pentry = radix_tree_lookup_slot(&sih->tree, pgoff);
	if(!pentry) // Entry has been deleted
		return 0;
	referenced_entry = radix_tree_deref_slot(pentry);

	if(referenced_entry == entry) // Entry has been modified
		return 1;
	else{
		printk("NOVA ERROR: Invalid DataPage Detected\n");
		return 0;
	}
}


/******************** FACT ********************/

// Clear FACT, set FACT_free_table, FACT locks
int nova_dedup_FACT_init(struct super_block *sb){
	unsigned long i;
	unsigned long start = 0;
	unsigned long end = FACT_TABLE_INDEX_MAX;
	unsigned long irq_flags=0;
	unsigned long target_index;
	struct fact_entry *target_entry;

	unsigned char fill[64];
	memset(fill,0,64);

	FACT_free_list = kzalloc(sizeof(struct DeNOVA_bm),GFP_KERNEL);
	FACT_free_list->bitmap_size = FACT_TABLE_INDEX_MAX;
	FACT_free_list->bitmap = kvzalloc(FACT_TABLE_INDEX_MAX,GFP_KERNEL);

	for(i =start; i<=end;i++){
		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + i * NOVA_FACT_ENTRY_SIZE;
		target_entry = (struct fact_entry*)nova_get_block(sb,target_index);

		nova_memunlock_range(sb,target_entry,64,&irq_flags);
		memcpy_to_pmem_nocache(target_entry, &fill,64);
		nova_memlock_range(sb,target_entry,64,&irq_flags);
	}
	return 1;
}

// Recover FACT
int nova_dedup_FACT_reorder_undo(struct super_block *sb, u64 head_index){
	// Scan through 'next' to fix the prev of each node
	unsigned long prev_index = head_index;
	unsigned long target_index=0;
	unsigned long curr_index = head_index;
	struct fact_entry *target_entry;

	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + curr_index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry*)nova_get_block(sb,target_index);
	curr_index = target_entry->next;

	do{
			target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + curr_index * NOVA_FACT_ENTRY_SIZE;
			target_entry = (struct fact_entry*)nova_get_block(sb,target_index);
			target_entry->prev = prev_index;
			prev_index = curr_index;
			curr_index = target_entry->next;
	}while(target_entry->next != head_index);

	return 0;
}

int nova_dedup_FACT_reorder_recover(struct super_block *sb, u64 head_index, u64 end_index){
	// Scan through 'prev' to fix the next of each node
	unsigned long target_index =0;
	unsigned long next_index = head_index;
	unsigned long curr_index = head_index;
	struct fact_entry *target_entry;
	
	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + curr_index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry*)nova_get_block(sb,target_index);
	curr_index = target_entry->prev;

	do{
		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + curr_index * NOVA_FACT_ENTRY_SIZE;
		target_entry = (struct fact_entry*)nova_get_block(sb,target_index);
		target_entry->next = next_index;
		next_index = curr_index;
		curr_index = target_entry->prev;
	}while(target_entry->prev != head_index);

	return 0;
}

int nova_dedup_FACT_recovery(struct super_block *sb){
	unsigned long i, start = 0;
	unsigned long end = FACT_TABLE_INDEX_MAX;
	unsigned long target_index;
	unsigned long irq_flags=0;
	unsigned long u_count, r_count;


	struct fact_entry *target_entry;

	FACT_free_list = kzalloc(sizeof(struct DeNOVA_bm),GFP_KERNEL);
	FACT_free_list->bitmap_size = FACT_TABLE_INDEX_MAX;
	FACT_free_list->bitmap = kvzalloc(FACT_TABLE_INDEX_MAX,GFP_KERNEL);

	for(i = start; i<=end;i++){
		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + i * NOVA_FACT_ENTRY_SIZE;
		target_entry = (struct fact_entry*)nova_get_block(sb,target_index);

		r_count = target_entry->count>>32;
		u_count = target_entry->count & (((long)1<<32)-1);

		// Rebuild FACT_free_list
		if(r_count > 0){
			// TODO Check if block is in free list?

			set_bit(target_index,FACT_free_list->bitmap); // set the bit of index
		}

		// Set Update Count to 0
		if(r_count != 0){
			nova_memunlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE, &irq_flags);
			PERSISTENT_BARRIER();
			target_entry->count -= r_count;
			nova_flush_buffer(&target_entry->count,CACHELINE_SIZE,1);
			nova_memlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE, &irq_flags);
		}

		// Check reordering process
		if(target_index < FACT_TABLE_INDIRECT_AREA_START_INDEX && target_entry->prev != 0){
			if(target_entry->prev == target_index){
				// Undo reorder process
				nova_dedup_FACT_reorder_undo(sb,target_index);
			}
			else{
				// continue reorder
				nova_dedup_FACT_reorder_recover(sb, target_index, target_entry->prev);
			}
		}
	}

	return 1;
}

int nova_dedup_FACT_reorder(struct super_block *sb, u64 head_index){
	struct fact_entry* target_entry;
	u64 curr_index = head_index;
	u64 target_index;
	u64 last_index=0;
	unsigned long *linked_list;
	int *reorder;
	unsigned long irq_flags=0;
	int hops=0;

	printk("Reorder Start\n");
	
	do{
		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + curr_index * NOVA_FACT_ENTRY_SIZE;
		target_entry = (struct fact_entry *)nova_get_block(sb,target_index);
		hops++;
		curr_index = target_entry->next;
	}while(target_entry->next != head_index);

	linked_list = kmalloc(hops * sizeof(unsigned long),GFP_KERNEL);
	reorder = kmalloc(hops * sizeof(int),GFP_KERNEL);

	// head 'prev' to head_index
	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + head_index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry *)nova_get_block(sb,target_index);

	nova_memunlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE,&irq_flags);
	PERSISTENT_BARRIER();
	target_entry->prev = head_index;
	nova_flush_buffer(&target_entry->prev,CACHELINE_SIZE,1);
	nova_memlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE,&irq_flags);

	// TODO Modify the prev of all nodes

	// head 'prev' to last node
	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + head_index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry *)nova_get_block(sb,target_index);

	nova_memunlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE,&irq_flags);
	PERSISTENT_BARRIER();
	target_entry->prev = last_index;
	nova_flush_buffer(&target_entry->prev,CACHELINE_SIZE,1);
	nova_memlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE,&irq_flags);

	// TODO Modify the next of all nodes

	// head 'prev' to 0
	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + head_index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry *)nova_get_block(sb,target_index);

	nova_memunlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE,&irq_flags);
	PERSISTENT_BARRIER();
	target_entry->prev = 0;
	nova_flush_buffer(&target_entry->prev,CACHELINE_SIZE,1);
	nova_memlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE,&irq_flags);

	kfree(linked_list);
	kfree(reorder);

	printk("Reorder End\n");

	return 1;
}

// Check FACT index range(of FACT)
int nova_dedup_FACT_index_check(u64 index){
	if(index > FACT_TABLE_INDEX_MAX){
		printk("FACT Index Out of Range: %llu(maximum %llu)\n",index,(unsigned long long int)FACT_TABLE_INDEX_MAX);
		return 1;
	}
	return 0;
}

// Update Count after tail has been updated. 
int nova_dedup_FACT_update_count(struct super_block *sb, u64 index){
	u64 count = 0;
	u64 compare = ((unsigned long)1<<32)-1;
	struct fact_entry* target_entry;
	unsigned long irq_flags=0;
	u64 target_index;
	u64 temp_index;
	// Check index is in range
	if(nova_dedup_FACT_index_check(index))
		return 1;

	// Read Actual Index
	// If this place has something, meaning that the entry has something
	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry *)nova_get_block(sb,target_index);
	target_index = target_entry->delete_entry;

	// Check index is in range
	if(nova_dedup_FACT_index_check(target_index))
		return 1;

	// Read Count of Actual Index
	temp_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + target_index * NOVA_FACT_ENTRY_SIZE;
	target_entry = (struct fact_entry*) nova_get_block(sb,temp_index);
	count = target_entry->count;

	// IF update Count > 0
	if(compare & count){
		count += compare; // Ucount--, Rcount++
		// Reference count, update count Atomic Update
		nova_memunlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE, &irq_flags);
		PERSISTENT_BARRIER();
		target_entry->count = count;
		nova_flush_buffer(&target_entry->count,CACHELINE_SIZE,1);
		nova_memlock_range(sb,target_entry,NOVA_FACT_ENTRY_SIZE, &irq_flags);
	}
	return 0;
}

// Reading a specific FACT entry by index, mainly for debugging
int nova_dedup_FACT_read(struct super_block *sb, u64 index){
	int r_count,u_count;
	u64 block_address;
	u64 next,prev;
	struct fact_entry* target;
	u64 target_index;

	if(nova_dedup_FACT_index_check(index))
		return 1;

	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
	target = (struct fact_entry*)nova_get_block(sb,target_index);

	r_count = target->count>>32;
	u_count = target->count & (((long)1<<32) - 1);
	block_address = target->block_address;
	next = target->next;
	prev = target->prev;
	
	printk("read!\n");
	//printk("index:%lld, ref_count:%d, up_count:%d, prev:%lld, next:%lld, block_address: %lld\n",
	//index,r_count,u_count,prev,next,block_address);
	return 0;
}

int nova_dedup_FACT_insert(struct super_block *sb, struct fingerprint_lookup_data* lookup){
	unsigned long irq_flags=0;
	unsigned long head_index=0;
	struct fact_entry  te; // target entry
	struct fact_entry* pmem_te; // pmem target entry
	u64 index = 0;
	u64 prev_index = 0;
	u64 target_index;
	int ret=0;
	/* Index SIZE */
	/* 4GB Environment - 19 bit */
	if(FACT_TABLE_INDEX_MAX == 1048575){
		index = lookup->fingerprint[0];
		index = index << 8 | lookup->fingerprint[1];
		index = index << 3 | ((lookup->fingerprint[2] & 224)>>5);
	}
	/* 1TB, 750GB Environment - 27 bit */
	else if(FACT_TABLE_INDEX_MAX == 196607999 || FACT_TABLE_INDEX_MAX == 268435455){    
		index = lookup->fingerprint[0];
		index = index << 8 | lookup->fingerprint[1];
		index = index << 8 | lookup->fingerprint[2];
		index = index << 3 | ((lookup->fingerprint[3] & 224)>>5);
	}
	// Index out of range
	if(nova_dedup_FACT_index_check(index))
		return 2;

	head_index = index;
	// Read Entries until it finds a match, or finds a empty slot
	do{
		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
		pmem_te = (struct fact_entry*)nova_get_block(sb,target_index);

		__copy_to_user(&te,pmem_te,sizeof(struct fact_entry));  

		if(nova_dedup_compare_fingerprint(te.fingerprint, lookup->fingerprint)==0 && (te.count != 0)){ // duplicate found	
			ret = 1;
			printk("hit\n");
			break;
		}
		else if(te.next != 0 && te.next != head_index){ // next exists
			index = te.next;
		}
		else{ // need new entry
			ret = 0;
			break;
		}
	}while(1);

	if(ret){ // duplicate data page detected
		nova_memunlock_range(sb,pmem_te,NOVA_FACT_ENTRY_SIZE,&irq_flags);
		PERSISTENT_BARRIER();
		pmem_te->count += 1; // increase update count
		nova_flush_buffer(&pmem_te->count,CACHELINE_SIZE,1);
		nova_memlock_range(sb,pmem_te, NOVA_FACT_ENTRY_SIZE,&irq_flags);
	}
	else{ // new entry should be written
		if(index == head_index){ // write in DAA
			prev_index = 0;
		}
		else{	// write in IAA
			prev_index = index;
			index = find_next_zero_bit(FACT_free_list->bitmap,FACT_free_list->bitmap_size,FACT_TABLE_INDIRECT_AREA_START_INDEX);
			set_bit(index,FACT_free_list->bitmap);
		}
		
		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
		pmem_te = (struct fact_entry*)nova_get_block(sb,target_index);

		__copy_to_user(&te,pmem_te,sizeof(struct fact_entry));  
		
		nova_dedup_copy_fingerprint(te.fingerprint,lookup->fingerprint);
		te.block_address = lookup->block_address;
		te.count = 1;
		te.prev = prev_index;
		te.next = head_index;

		// copy target_entry to pmem
		nova_memunlock_range(sb,pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);
		memcpy_to_pmem_nocache(pmem_te, &te, NOVA_FACT_ENTRY_SIZE - 12); // don't write delete, pdding
		nova_memlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);
	
		if(index != head_index){
			target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + prev_index * NOVA_FACT_ENTRY_SIZE;
			pmem_te = (struct fact_entry*)nova_get_block(sb,target_index);
		
			// set previous index's next field to 'index'
			nova_memunlock_range(sb,pmem_te,NOVA_FACT_ENTRY_SIZE,&irq_flags);
			PERSISTENT_BARRIER();
			pmem_te->next = index;
			nova_flush_buffer(&pmem_te->next,CACHELINE_SIZE,1);
			nova_memlock_range(sb,pmem_te, NOVA_FACT_ENTRY_SIZE,&irq_flags);		
		}
	}

	// update lookup data(used in deduplication process)
	lookup->index = index;
	lookup->block_address = te.block_address;

	// Add 'delete entry'
	if(ret == 0){
		// Check range
		if(nova_dedup_FACT_index_check(te.block_address))
			return 2;

		target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + te.block_address * NOVA_FACT_ENTRY_SIZE;
		pmem_te = (struct fact_entry*)nova_get_block(sb,target_index);

		nova_memunlock_range(sb,pmem_te,NOVA_FACT_ENTRY_SIZE,&irq_flags);
		PERSISTENT_BARRIER();
		pmem_te->delete_entry = index;
		nova_flush_buffer(&pmem_te->delete_entry,CACHELINE_SIZE,1);
		nova_memlock_range(sb,pmem_te, NOVA_FACT_ENTRY_SIZE,&irq_flags);
	}
	return ret;
}

// Update FACT table + dedup_flags in write entry, of Target Write Entry
int nova_dedup_TWE_update(struct super_block *sb,struct nova_inode_info_header *sih,u64 curr_p, short *duplicate_check){
	int i;
	unsigned long irq_flags=0;
	unsigned int num=0;
	unsigned long start_index;
	unsigned long curr_index;

	void *addr;
	struct nova_file_write_entry *entry;

	addr = (void*)nova_get_block(sb,curr_p);
	entry = (struct nova_file_write_entry *)addr;
	// Update dedup flag to 'in_process'
	nova_memunlock_range(sb,entry,CACHELINE_SIZE,&irq_flags);
	PERSISTENT_BARRIER();
	entry->dedup_flag = IN_PROCESS;
	nova_flush_buffer(&entry->dedup_flag,CACHELINE_SIZE,1);
	nova_memlock_range(sb,entry,CACHELINE_SIZE,&irq_flags);

	// Update unique FACT entry counts
	num = entry->num_pages;
	start_index = entry->block >> PAGE_SHIFT;
	for(i=0;i<num;i++){
		if(duplicate_check[i] != 0)
			continue;
		curr_index = start_index+i;
		nova_dedup_FACT_update_count(sb,curr_index);
	}
	// Update dedup flag to 'dedup finished'
	nova_memunlock_range(sb,entry,CACHELINE_SIZE,&irq_flags);
	PERSISTENT_BARRIER();
	entry->dedup_flag = DEDUP_DONE;
	nova_flush_buffer(&entry->dedup_flag,CACHELINE_SIZE,1);
	nova_memlock_range(sb,entry,CACHELINE_SIZE,&irq_flags);

	return 0;
}

// Update FACT table + dedup_flags in write entry, of new write entry
int nova_dedup_entry_update(struct super_block *sb, struct nova_inode_info_header *sih, u64 begin_tail){
	void *addr;
	struct nova_file_write_entry *entry;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);
	unsigned long irq_flags=0;
	unsigned long curr_index;
	unsigned long start_index;
	unsigned int num=0;
	int i;

	while(curr_p && curr_p != sih->log_tail){
		if(is_last_entry(curr_p,entry_size))
			curr_p = next_log_page(sb,curr_p);
		if(curr_p ==0)
			break;
		addr = (void*) nova_get_block(sb,curr_p);
		entry = (struct nova_file_write_entry *)addr;

		num = entry->num_pages;
		start_index = entry->block >> PAGE_SHIFT;
		for(i=0;i<num;i++){
			curr_index = start_index + i;
			nova_dedup_FACT_update_count(sb,curr_index); // Update FACT 'update, reference count'
		}
		// Update Write New Write Entry 'dedup_flag'
		nova_memunlock_range(sb,entry,CACHELINE_SIZE,&irq_flags);
		PERSISTENT_BARRIER();
		entry->dedup_flag = DEDUP_DONE;  // Dedup finish
		nova_flush_buffer(&entry->dedup_flag,CACHELINE_SIZE,1);

		nova_update_entry_csum(entry);
		nova_update_alter_entry(sb,entry);
		nova_memlock_range(sb,entry,CACHELINE_SIZE,&irq_flags);
		curr_p += entry_size;
	}
	return 0;
}


// Check if target block has multiple Reference Count & Delete FACT entry
// Return 1 if it's okay to delete - reference count = 0
// Return 0 if it's not okay to delete - reference count > 0
// Return 2 if it's not in FACT table - reference count < 0
int nova_dedup_is_duplicate(struct super_block *sb, unsigned long blocknr, bool check){
	unsigned long irq_flags=0;
	struct fact_entry* pmem_te; // pmem target entry
	u64 index = 0;
	u64 target_index;

	// Check Index Range of delete entry
	if(nova_dedup_FACT_index_check(blocknr))
		return 3;

	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + blocknr * NOVA_FACT_ENTRY_SIZE;
	pmem_te = (struct fact_entry*)nova_get_block(sb,target_index);

	index = pmem_te->delete_entry;

	// Check Index Range of target FACT entry
	if(nova_dedup_FACT_index_check(index))
		return 2;

	target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + index * NOVA_FACT_ENTRY_SIZE;
	pmem_te = (struct fact_entry*)nova_get_block(sb,target_index);

	if((pmem_te->count>>32) <= 0){ // It's not in dedup table, Deleted before Deduplication
		return 2;
	}
	else{ // It's okay to delete, this entry can also be deleted
		if(!check){
			nova_memunlock_range(sb,pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);
			PERSISTENT_BARRIER();
			pmem_te->count -= ((unsigned long)1<<32); // Update Reference Count
			nova_flush_buffer(&pmem_te->count,CACHELINE_SIZE,1);
			nova_memlock_range(sb, pmem_te, NOVA_FACT_ENTRY_SIZE, &irq_flags);
		}

		if((pmem_te->count>>32) == 0){ // Free data page
			/* 
			   Deleting process should be fast, this reordering can take place later
			// Set prev->next to next
			if(te.prev != 0){
			temp_next =te.next;
			target_index = te.prev;
			target_index = NOVA_DEF_BLOCK_SIZE_4K * FACT_TABLE_START + target_index * NOVA_FACT_ENTRY_SIZE;
			pmem_te = (struct fact_entry*)nova_get_block(sb,target_index);

			nova_memunlock_range(sb,pmem_te,NOVA_FACT_ENTRY_SIZE,&irq_flags);
			PERSISTENT_BARRIER();
			pmem_te->next = temp_next;
			nova_flush_buffer(&pmem_te->count,CACHELINE_SIZE,1);
			nova_memlock_range(sb,pmem_te,NOVA_FACT_ENTRY_SIZE,&irq_flags);
			}
			// Set bit to 0 in deleted FACT entry
			clear_bit(index,FACT_free_list->bitmap);// clear the bit of index
			 */
			return 1;
		}
		else // Don't free data page
			return 0; // Can't delete
	}
}


/******************** DEDUPLICATION MAIN FUNCTION ********************/
int nova_dedup_test(struct file * filp){
	// Read Super Block
	struct address_space *mapping = filp->f_mapping;	
	struct inode *garbage_inode = mapping->host;
	struct super_block *sb = garbage_inode->i_sb;

	// How many deduplications are going to be done each time?
	int dedup_loop_count = 10;

	// Reorder variable
	int reorder_condition;
	struct timespec t0, t1;

	// For read phase
	struct nova_file_write_entry *target_entry;	// Target write entry to deduplicate
	struct inode *target_inode;		// Inode of target write entry
	u64 entry_address;	// Address of target write entry(TWE)
	u64 target_inode_number=0; // Number of target inode (TI)
	struct nova_inode *target_pi, inode_copy;	// nova_inode of TI
	struct nova_inode_info *target_si;
	struct nova_inode_info_header *target_sih;

	unsigned char *buf;	// Read Buffer
	unsigned char *fingerprint; // Fingerprint result

	unsigned long left;
	pgoff_t index;
	int i, j, num_pages =0;
	unsigned long nvmm;
	void *dax_mem = NULL;

	// For write phase
	int num_new_entry=0;
	struct fingerprint_lookup_data *lookup_data;
	struct nova_inode_update update;
	struct nova_file_write_entry entry_data; // new write entry
	short *duplicate_check;
	u64 file_size;
	unsigned long original_start_blk, start_blk;
	unsigned long blocknr =0;
	unsigned long num_blocks =0;
	unsigned long irq_flags=0;
	u64 begin_tail =0;
	u64 epoch_id;
	u32 time;
	u32 valid_page_num=0;
	ssize_t ret=0;

	// kmalloc buf, fingerprint
	buf = kmalloc(DATABLOCK_SIZE,GFP_KERNEL);
	fingerprint = kmalloc(FINGERPRINT_SIZE,GFP_KERNEL);

	//getnstimeofday(&t0);
	do{
		// Pop TWE(Target Write Entry)
		entry_address = nova_dedup_queue_get_next_entry(&target_inode_number);
		// target_inode_number should exist
		if (target_inode_number < NOVA_NORMAL_INODE_START && target_inode_number != NOVA_ROOT_INO) {
			//nova_info("%s: invalid inode %llu.", __func__,target_inode_number);
			printk("No entry\n");
			break;
		}
		// Read TI(Target Inode)
		target_inode = nova_iget(sb,target_inode_number);
		// Inode Could've been deleted
		if(target_inode == ERR_PTR(-ESTALE)){
			//nova_info("%s: inode %llu does not exist.", __func__,target_inode_number);	
			//iput(target_inode);	// Release Inode
			continue;
		}

		if(entry_address!=0){
			//Initialize variables
			ret =0;
			num_new_entry=0;
			valid_page_num=0;
			original_start_blk = 0;
			begin_tail=0;
			irq_flags=0;

			target_si = NOVA_I(target_inode);
			target_sih = &target_si->header;
			target_pi = nova_get_inode(sb,target_inode);

			// ---------------------------Lock Acquire-----------------------------------------
			sb_start_write(target_inode->i_sb);
			inode_lock(target_inode);

			// Read TWE
			target_entry = nova_get_block(sb, entry_address);
			original_start_blk = target_entry->pgoff;

			index = target_entry->pgoff;
			num_pages = target_entry->num_pages;
			lookup_data = kmalloc(num_pages*sizeof(struct fingerprint_lookup_data),GFP_KERNEL);
			duplicate_check = kmalloc(sizeof(short)*num_pages,GFP_KERNEL);
			memset(duplicate_check,false,sizeof(short)*num_pages);

			// Read Each Data Page from TWE
			for(i=0;i<num_pages;i++){
				if(nova_dedup_crosscheck(target_entry,target_sih,index)==0){
					duplicate_check[i] = 2; // Data page i in invalid, target write entry does not point to it!
					index++;
					continue;
				}
				valid_page_num++;
				memset(buf,0,DATABLOCK_SIZE);
				memset(fingerprint,0,FINGERPRINT_SIZE);

				nvmm = get_nvmm(sb,target_sih,target_entry,index);
				dax_mem = nova_get_block(sb,(nvmm << PAGE_SHIFT));
				
				
				left = __copy_to_user(buf,dax_mem,DATABLOCK_SIZE); // Read data page
				if(left){
					nova_dbg("%s ERROR!: left %lu\n",__func__,left);
					goto out;
				}
				// Fingerprint each datapage
				getrawmonotonic(&t0);
				nova_dedup_fingerprint(buf,fingerprint);
				getrawmonotonic(&t1);	
				//printk("%ld sec, %ld nsec\n",t1.tv_sec-t0.tv_sec,t1.tv_nsec-t0.tv_nsec);
				for(j=0;j<FINGERPRINT_SIZE;j++){
					lookup_data[i].fingerprint[j] = fingerprint[j];
				}
				lookup_data[i].block_address = nvmm;
				index++;
			}

			// Lookup & Add to FACT table
			for(i=0;i<num_pages;i++)
				if(duplicate_check[i] != 2){
					duplicate_check[i] = nova_dedup_FACT_insert(sb,&lookup_data[i]);
					num_new_entry += duplicate_check[i];
				}

			//Test
			for(i=0;i<num_pages;i++)
				if(duplicate_check[i] != 2){
					nova_dedup_FACT_read(sb, lookup_data[i].index);
				}
			// Get the number of new write entries needed to be appended.
			if(num_new_entry == 0){
				//printk("All Unique Data Pages\n");
				nova_dedup_TWE_update(sb,target_sih,entry_address,duplicate_check);
				goto out;
			}

			// ------------------- Write Phase -----------------------
			if(nova_check_inode_integrity(sb,target_sih->ino,target_sih->pi_addr,
						target_sih->alter_pi_addr, &inode_copy,0) <0){
				ret = -EIO;
				goto out;
			}	

			// set time
			target_inode->i_ctime  = current_time(target_inode);
			time = current_time(target_inode).tv_sec;

			epoch_id = nova_get_epoch_id(sb);
			update.tail = target_sih->log_tail;
			update.alter_tail = target_sih->alter_log_tail;
			file_size = cpu_to_le64(target_inode -> i_size);

			// Only add new write entries for duplicate data pages
			for(i=0;i<num_pages;i++){
				if(duplicate_check[i] != 1)
					continue;

				start_blk = original_start_blk + i; 
				num_blocks = 1; 
				blocknr = lookup_data[i].block_address;  

				nova_init_file_write_entry(sb,target_sih, &entry_data, epoch_id,
						start_blk, num_blocks,blocknr, time, file_size);
				entry_data.dedup_flag = IN_PROCESS; // flag is set to 2 - in process
				ret = nova_append_file_write_entry(sb,target_pi,target_inode,&entry_data,&update);

				//printk("NEW WRITE ENTRY(offset: %lu, %lu blocks)\n",start_blk,num_blocks);

				if(ret){
					nova_dbg("%s: append inode entry failed\n",__func__);
					ret = -ENOSPC;
					//goto out;
				}
				if(begin_tail == 0)
					begin_tail = update.curr_entry;
				num_new_entry -= 1;
			}
			if(num_new_entry!=0){ // Not appended pages exists
				printk("Datapage assign error! %d duplicate pages left\n",num_new_entry);
				goto out;
			}

			// Update tail
			nova_memunlock_inode(sb,target_pi,&irq_flags);
			nova_update_inode(sb,target_inode,target_pi,&update,1);
			nova_memlock_inode(sb,target_pi,&irq_flags);

			// Update FACT TABLE + dedup_flag of TWE
			nova_dedup_TWE_update(sb,target_sih,entry_address,duplicate_check);

			// Update FACT TABLE + dedup_flag of new write entries
			nova_dedup_entry_update(sb,target_sih,begin_tail);

			// Update Radix Tree
			ret = nova_reassign_file_tree(sb,target_sih,begin_tail);
			if(ret)
				goto out;

			target_inode->i_blocks = target_sih->i_blocks;
			target_sih->trans_id++;
out:
			if(ret<0)
				nova_cleanup_incomplete_write(sb,target_sih,blocknr,num_blocks,begin_tail,update.tail);

			// Unlock ------------------------------------------------------------
			inode_unlock(target_inode);
			sb_end_write(target_inode->i_sb);

			kfree(lookup_data);
			kfree(duplicate_check);
		}
		iput(target_inode);	// Release Inode
	}while(dedup_loop_count--);

	//getnstimeofday(&t1);
	//printk("%ld sec %ld nsec  spent\n",t1.tv_sec-t0.tv_sec,t1.tv_nsec - t0.tv_nsec);	
	kfree(buf);
	kfree(fingerprint);
	return 0;
}
