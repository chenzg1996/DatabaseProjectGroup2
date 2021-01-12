#include "pml_hash.h"
#include <math.h>
#include <fstream>
/**
 * PMLHash::PMLHash 
 * 
 * @param  {char*} file_path : the file path of data file
 * if the data file exist, open it and recover the hash
 * if the data file does not exist, create it and initial the hash
 */
uint64_t MAX = (FILE_SIZE/2 - sizeof(metadata)) / sizeof(pm_table);

PMLHash::PMLHash(const char* file_path) {
	ifstream file;
	file.open(file_path);
	size_t mapped_len;							//if the data file does not exist, create it and initial the hash
    	int is_pmem;
   	start_addr = pmem_map_file(file_path, FILE_SIZE, PMEM_FILE_CREATE, 0666, &mapped_len, &is_pmem);
	if(file){
		meta = (metadata*)start_addr;
		
		table_arr = (pm_table*)(start_addr + sizeof(metadata));
	}
	else{
		meta = new metadata;
		meta->level = 0;
		meta->next = 0;
		meta->overflow_num = 0;
		meta->size = HASH_SIZE;
		pmem_memcpy_persist(start_addr, meta, sizeof(metadata));
		table_arr = new pm_table[MAX];
		for(uint64_t i=0; i<MAX; i++){
			table_arr[i].next_offset = 0;
			table_arr[i].fill_num = 0;
		}
		pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
	}
	overflow_addr = start_addr + FILE_SIZE / 2 + meta->overflow_num * sizeof(pm_table);
}
/**
 * PMLHash::~PMLHash 
 * 
 * unmap and close the data file
 */
PMLHash::~PMLHash() {
    pmem_unmap(start_addr, FILE_SIZE);
}
/**
 * PMLHash 
 * 
 * split the hash table indexed by the meta->next
 * update the metadata
 */
void PMLHash::split() {
    // fill the split table
    // update the next of metadata
    	pm_table p = table_arr[meta->next]; //jilu fenlie dian
	meta->next++;//fenlie dian houyi
	pmem_memcpy_persist(start_addr, meta, sizeof(metadata));
	for(uint64_t i=1; i<=p.fill_num; i++){//bei fenlie dian de yuansu geshu
		uint64_t index = hashFunc(p.kv_arr[i-1].key, HASH_SIZE);
		if(index != meta->next-1){				// fill the new table
			insert(p.kv_arr[i-1].key, p.kv_arr[i-1].value);
			meta->next--;
			pmem_memcpy_persist(start_addr, meta, sizeof(metadata));
			remove(p.kv_arr[i-1].key);
			meta->next++;
			pmem_memcpy_persist(start_addr, meta, sizeof(metadata));			
		}	
	}
	if(p.next_offset!=0){				//have overflow bucket
		pm_table* op = (pm_table*)(start_addr + p.next_offset); //op is p's overflow bucket
		for(uint64_t i=0; i<op->fill_num; i++){
			insert(op->kv_arr[i].key, op->kv_arr[i].value);
			meta->next--;
			pmem_memcpy_persist(start_addr, meta, sizeof(metadata));
			remove(op->kv_arr[i].key);
			meta->next++;
			pmem_memcpy_persist(start_addr, meta, sizeof(metadata));	
		}	
	}
	meta->size++;
	if(meta->size==HASH_SIZE*pow(2, meta->level)){
		meta->level++;
		meta->next = 0;
	}
	pmem_memcpy_persist(start_addr, meta, sizeof(metadata));
}
/**
 * PMLHash 
 * 
 * @param  {uint64_t} key     : key
 * @param  {size_t} hash_size : the N in hash func: idx = hash % N
 * @return {uint64_t}         : index of hash table array
 * 
 * need to hash the key with proper hash function first
 * then calculate the index by N module
 */
uint64_t PMLHash::hashFunc(const uint64_t &key, const size_t &hash_size) {
	uint64_t a;
	a = hash_size * pow(2, meta->level);
	uint64_t index = key % a;
	if(index>=meta->next){
		a = hash_size * pow(2, meta->level);
	}
	else{
		a = hash_size * pow(2, meta->level + 1);
	}
		return key%a;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} offset : the file address offset of the overflow hash table
 *                             to the start of the whole file
 * @return {pm_table*}       : the virtual address of new overflow hash table
 */
pm_table* PMLHash::newOverflowTable(uint64_t &offset) {
	pm_table* a = new pm_table;          
	a->fill_num = 0;
	pmem_memcpy_persist(start_addr+offset, a, sizeof(pm_table));
	meta->overflow_num++;
	pmem_memcpy_persist(start_addr, meta, sizeof(metadata));
	overflow_addr += sizeof(pm_table);
	return a;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : inserted key
 * @param  {uint64_t} value : inserted value
 * @return {int}            : success: 0. fail: -1
 * 
 * insert the new kv pair in the hash
 * 
 * always insert the entry in the first empty slot
 * 
 * if the hash table is full then split is triggered
 */
int PMLHash::insert(const uint64_t &key, const uint64_t &value) {
	uint64_t index = hashFunc(key, HASH_SIZE);
	if(table_arr[index].fill_num < TABLE_SIZE){										//tong mei man
		table_arr[index].kv_arr[table_arr[index].fill_num].key = key;
		table_arr[index].kv_arr[table_arr[index].fill_num].value = value;
		table_arr[index].fill_num++;
		pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
		return 0;
	}
	else{													   //tong man  
		if(table_arr[index].next_offset>0){						//you yichu tong 
			pm_table* p = (pm_table*)(start_addr + table_arr[index].next_offset);    //overflow bucket
			p->kv_arr[p->fill_num].key = key;
			p->kv_arr[p->fill_num].value = value;
			p->fill_num++;
			pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
			
		}
		else{		//meiyou yichu tong
			table_arr[index].next_offset = FILE_SIZE / 2 + meta->overflow_num * sizeof(pm_table);
			newOverflowTable(table_arr[index].next_offset);
			pm_table* p = (pm_table*)(start_addr + table_arr[index].next_offset);    //overflow bucket
			p->kv_arr[p->fill_num].key = key;
			p->kv_arr[p->fill_num].value = value;
			p->fill_num++;
			pmem_memcpy_persist(start_addr + table_arr[index].next_offset, p, sizeof(pm_table));
			
		}
		split();
		return 0;
	}
	return -1;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : the searched key
 * @param  {uint64_t} value : return value if found
 * @return {int}            : 0 found, -1 not found
 * 
 * search the target entry and return the value
 */
int PMLHash::search(const uint64_t &key, uint64_t &value){
	uint64_t index = hashFunc(key, HASH_SIZE);
	for(uint64_t i=0; i<table_arr[index].fill_num; i++){
		if(table_arr[index].kv_arr[i].key==key){
			value = table_arr[index].kv_arr[i].value;
			return 0;
		}
	}
	if(table_arr[index].next_offset>0){
		pm_table* p = (pm_table*)(start_addr + table_arr[index].next_offset);    //overflow bucket
		for(uint64_t i=0; i<p->fill_num; i++){
			if(p->kv_arr[i].key==key){
				value = p->kv_arr[i].value;
				return 0;
			}
		}
	}
	return -1;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key : target key
 * @return {int}          : success: 0. fail: -1
 * 
 * remove the target entry, move entries after forward
 * if the overflow table is empty, remove it from hash
 */
int PMLHash::remove(const uint64_t &key) {
	uint64_t index = hashFunc(key, HASH_SIZE);
	uint64_t j = 0;
	for(uint64_t i=0; i<table_arr[index].fill_num; i++){
			if(table_arr[index].kv_arr[i].key!=key){
			table_arr[index].kv_arr[j].key = table_arr[index].kv_arr[i].key;
			table_arr[index].kv_arr[j].value = table_arr[index].kv_arr[i].value;
			j++;
			pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
		}
	}
	if(table_arr[index].next_offset>0){
		pm_table* p = (pm_table*)(start_addr + table_arr[index].next_offset);    //overflow bucket
		if(j==table_arr[index].fill_num){         //the entry that we want to remove is in overflow bucket
			j = 0;
			for(uint64_t i=0; i<p->fill_num; i++){
				if(p->kv_arr[i].key!=key){
					p->kv_arr[j].key = p->kv_arr[i].key;
					p->kv_arr[j].value = p->kv_arr[i].value;
					j++;
					pmem_memcpy_persist(start_addr + table_arr[index].next_offset, p, sizeof(pm_table));
				}
			}
			if(j==p->fill_num){
				return -1;
			}
			else{
				p->fill_num--;
				pmem_memcpy_persist(start_addr + table_arr[index].next_offset, p, sizeof(pm_table));
				if(p->fill_num==0){
					table_arr[index].next_offset = 0;
					pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
				}
				return 0;
			}
		}
		else{
			table_arr[index].kv_arr[j].key = p->kv_arr[0].key;
			table_arr[index].kv_arr[j].value = p->kv_arr[0].value;
			pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
			j = 0;
			for(uint64_t i=1; i<p->fill_num; i++){
				p->kv_arr[j].key = p->kv_arr[i].key;
				p->kv_arr[j].value = p->kv_arr[i].value;
				j++;
				pmem_memcpy_persist(start_addr + table_arr[index].next_offset, p, sizeof(pm_table));
			}
			p->fill_num--;
			pmem_memcpy_persist(start_addr + table_arr[index].next_offset, p, sizeof(pm_table));
			if(p->fill_num==0){
				table_arr[index].next_offset = 0;
				pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));

			}
			return 0; 		//always success
		}
	}
	else{
		if(j==table_arr[index].fill_num){
			
			return -1;
		}
		else{
			
			table_arr[index].fill_num--;
			pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
			return 0;
		}
	}
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : target key
 * @param  {uint64_t} value : new value
 * @return {int}            : success: 0. fail: -1
 * 
 * update an existing entry
 */
int PMLHash::update(const uint64_t &key, const uint64_t &value) {
	uint64_t index = hashFunc(key, HASH_SIZE);
	for(uint64_t i=0; i<table_arr[index].fill_num; i++){
		if(table_arr[index].kv_arr[i].key==key){
			table_arr[index].kv_arr[i].value = value;
			pmem_memcpy_persist(start_addr+sizeof(metadata), table_arr, MAX*sizeof(pm_table));
			return 0;
		}
	}
	if(table_arr[index].next_offset>0){
		pm_table* p = (pm_table*)(start_addr + table_arr[index].next_offset);    //overflow bucket
		for(uint64_t i=0; i<p->fill_num; i++){
			if(p->kv_arr[i].key==key){
				p->kv_arr[i].value = value;
				pmem_memcpy_persist(start_addr + table_arr[index].next_offset, p, sizeof(pm_table));
				return 0;
			}
		}
	}
	return -1;
}
