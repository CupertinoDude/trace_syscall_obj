#include "config_pemu.h"
#include <vector>
#include <stack>
#include <stdio.h>
#include <stdlib.h> 
#include <string.h>
#include <map>
#include <string>
#include "linux.h"
#include <iostream>
#include <ext/functional>
#include <sstream>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <assert.h>
#include "algorithm"
#include <sys/time.h>


extern "C" {
#include "taint.h"
#include "heap_shadow.h"

struct PEMU_INST {
	unsigned int PEMU_inst_pc;
	xed_state_t PEMU_dstate;
	xed_decoded_inst_t PEMU_xedd_g;
	char PEMU_inst_buf[15];
	char PEMU_inst_str[128];
};

extern struct PEMU_INST pemu_inst;
xed_error_enum_t disas_one_inst_ex(unsigned int pc, struct PEMU_INST *inst);
int operand_is_mem(const xed_operand_enum_t op_name, uint32_t* mem_addr, int); 
int operand_is_reg(const xed_operand_enum_t op_name, xed_reg_enum_t * reg_id);
uint32_t PEMU_get_reg(xed_reg_enum_t reg_id);
int PEMU_read_mem(uint32_t vaddr, int len, void *buf);
uint32_t get_kernel_esp(void);
uint32_t PEMU_get_cr3(void);
}
using namespace std;

struct Elem {
	unsigned int addr;
	size_t type;
	int objsize;
	unsigned int pc;
};

struct Elem1 {
	string name;
	int size;
	unsigned int caller_pc;
	unsigned int addr;
};

//static vector<unsigned int> g_callsite;
//static vector<unsigned int> g_callstack;
static vector<unsigned int> g_ret_stack;
static map<unsigned int, struct Elem1> g_obj;
static map<unsigned int, struct Elem1> g_obj1;
static map<unsigned int, int> g_all_obj;
static map<unsigned int, size_t> g_obj_pool;
static map<unsigned int, string> g_heap_types;
static map<string, unsigned int> g_heap_types_s;
static map<unsigned int, string> g_syscall;
static map<unsigned int, int> g_proc_intr;
static map<unsigned, vector<unsigned int> > g_all_callstacks;
static map<unsigned, vector<unsigned int> > g_all_retstacks;
static map<unsigned int, string> g_trace_kmem;
static map<size_t, string > read_hash_callstack;
static map<size_t, string > write_hash_callstack;
static map<size_t, string > delete_hash_callstack;

static map<unsigned int, int> syscall_call_funcs;

static map<unsigned int, struct info> g_all_info;
static vector<unsigned int> *p_cur_retstack;
static vector<unsigned int> *p_cur_callstack;
int s_int_level = -1;
int s_in_sched = -1;
int s_in_syscall = -1;
int s_trace = -1;
int s_esp = 0;
int s_sysnum = -1;
int s_proc_addr = 0;

#define in_syscall_context()	\
	s_in_syscall == 1


extern "C"{
FILE *output_file;
void dump_rets(FILE *file, unsigned pc);
void dump_all_rets(unsigned int esp, unsigned pc);
//size_t hash_callstack_ebp(unsigned int);
string get_callstack_ebp(uint32_t pc);

////////////////////////////////
size_t hash_callstack(uint32_t pc)
{
//	dump_rets(stdout, pc);
	hash<std::string> hash_fn;
#ifdef TRACE_SYSCALL_OBJ
	string str;
	stringstream ss;

	for(vector<unsigned int>:: iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); it++) {
		ss << *it;
	}
	ss << pc;
	str = ss.str();
	return hash_fn(str);
#else
	return hash_fn(get_callstack_ebp(pc));
	//return hash_callstack_ebp(pc);
#endif
}

size_t hash_read_callstack(uint32_t pc)
{
	hash<std::string> hash_fn;
	string str;
	stringstream ss;
	size_t hash;

	for(vector<unsigned int>:: iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); it++) {
		ss << hex <<*it;
		ss << "->";
	}
	ss << pc;
	str = ss.str();
	hash = hash_callstack(pc);
	read_hash_callstack[hash] = str;
	return hash;
}

size_t hash_write_callstack(uint32_t pc)
{
	hash<std::string> hash_fn;
	string str;
	stringstream ss;
	size_t hash;

	for(vector<unsigned int>:: iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); it++) {
		ss << hex <<*it;
		ss << "->";
	}
	ss << pc;
	str = ss.str();
	hash = hash_callstack(pc);
	write_hash_callstack[hash] = str;
	return hash;
}

size_t hash_delete_callstack(uint32_t pc)
{
	hash<std::string> hash_fn;
	string str;
	stringstream ss;
	size_t hash;

	for(vector<unsigned int>:: iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); it++) {
		ss << hex <<*it;
		ss << "->";
	}
	ss << pc;
	str = ss.str();
	hash = hash_callstack(pc);
	delete_hash_callstack[hash] = str;
	return hash;
}

uint32_t is_dup_call_kmem_cache_alloc(void)
{
#ifdef TRACE_SYSCALL_OBJ
	for (vector<unsigned int>::reverse_iterator rit = (*p_cur_callstack).rbegin();
			rit != (*p_cur_callstack).rend(); ++rit) {
		if((*rit == KMEM_CACHE_ALLOC || *rit == __KMALLOC || *rit == __KMALLOC_TRACK_CALLER)
				&& rit != (*p_cur_callstack).rbegin()) {
			//in these cases, use trace_kmalloc
			return 1;
		}
		if(*rit == KMEM_CACHE_ALLOC_TRACE && rit != (*p_cur_callstack).rbegin()) {//recursive call
			return 1;
		}
	}
#else

#endif
	return 0;
}

void insert_obj1(unsigned int pc, int size)
{
	struct Elem1 elem;
	elem.size = size;
	g_obj1[pc] = elem;
}

int get_obj1(unsigned int pc)
{
	return g_obj1.count(pc);
}

int get_size1(unsigned int pc)
{
	return g_obj1[pc].size;
}

void delete_obj1(unsigned int pc)
{
	g_obj1.erase(pc);
}

////////////////////////////////
void insert_obj(unsigned int pc, unsigned int addr, int size, char *name)
{
	struct Elem1 elem;
	elem.addr = addr;
	elem.size = size;
	elem.name = name;
	g_obj[pc] = elem;
}

int get_obj(unsigned int pc)
{
	return g_obj.count(pc);
}

void get_name(unsigned int pc, char *name)
{
	strcpy(name, g_obj[pc].name.c_str());
}

int get_size(unsigned int pc)
{
	return g_obj[pc].size;
}

int get_obj_addr(unsigned int pc)
{
	return g_obj[pc].addr;
}

unsigned int get_caller_pc(unsigned int pc)
{
	return g_obj[pc].caller_pc;
}

void delete_obj(unsigned int pc)
{
	g_obj.erase(pc);
}

void insert_all_obj(unsigned int pc)
{
	if(g_all_obj.count(pc) == 0) {
		g_all_obj[pc] = 1;
	} else {
		if(g_all_obj[pc] <= 0) {
			printf("error:insert_all_obj %x\n", g_all_obj[pc]);
			assert(0);
		}
		g_all_obj[pc] ++;
	}
}

int get_all_obj(unsigned int pc)
{
	return g_all_obj.count(pc);
}

void delete_all_obj(unsigned int pc)
{
	if(g_all_obj[pc] == 1) {
		g_all_obj.erase(pc);
	} else {
		if(g_all_obj[pc] <= 0) {
			printf("error:delete_all_obj %x\n", g_all_obj[pc]);
			assert(0);
		}
		g_all_obj[pc]--;
	}
}
////////////////////////////////

//static map<unsigned int, string> g_trace_kmem;
void insert_kmem_obj(unsigned int pc, char *name)
{
	if(g_trace_kmem.count(pc) == 0) {
		g_trace_kmem[pc] = string(name);
	} else {
	}
}

int get_kmem_obj(unsigned int pc)
{
	return g_trace_kmem.count(pc);
}

void get_kmem_name(unsigned int pc, char *name)
{
	strcpy(name, g_trace_kmem[pc].c_str());
	return;
}

void delete_kmem(unsigned int pc)
{
	g_trace_kmem.erase(pc);
}


void insert_retaddr(unsigned int pc)
{
	g_ret_stack.push_back(pc);
}

void delete_retaddr(unsigned int pc)
{
	g_ret_stack.pop_back();
}


int is_retaddr(unsigned int pc)
{
	if(g_ret_stack.empty())
		return 0;
	if(g_ret_stack.back() == pc){
		//g_ret_stack.pop();
		return 1;
	}else {
		return 0;
	}
}

void dump_rets1(FILE *file, unsigned int pc)
{
//#ifdef DEBUG
	fprintf(file, "rets:");
	for(std::vector<unsigned int>::iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); ++it){
		fprintf(file, "%x->", *it);
	}
	fprintf(file, "%x\n", pc);
//#endif
}


void clear_calldata(void)
{
	while(!g_ret_stack.empty())
    	g_ret_stack.clear();
}


FILE *PEMU_open(int syscall, int index)
{
	char path[100];
	FILE *file;
	
	sprintf(path, "%sT_%d_%s_%x", "linux-obj/", syscall, g_syscall[syscall].c_str(), index);
	file = fopen(path, "w");
	if(file == 0) {
		assert(0);
	}
	fprintf(stderr, "PEMU_open:%s file:%p\n", path, file);
	fprintf(file, "cr3=%x esp=%x\n", PEMU_get_cr3(), get_kernel_esp() & 0xffffe000);
	return file;
}

FILE *PEMU_close(FILE *f)
{
	fprintf(stderr, "PEMU_close file:%p\n", f);
	fclose(f);
}


void load_syscalls(void)
{
	FILE *file = fopen("syscalls", "r");
	char name[50];
	int num;
	if(file == NULL){
		fprintf(stderr, "error in open syscalls\n");
		exit(0);
	}

	while(fscanf(file, "%d\t%s\n", 
				&num, name) != EOF) {
		g_syscall[num] = name;
		//cout<<num<<" "<<name<<endl;
	}
}

const char *get_syscallName(int num)
{
	return g_syscall[num].c_str();
}

//#define DEBUG

struct info {
	int in_syscall;	//at syscall contex?
	int int_level;	//intr level
	int in_sched;	//schedule?
	int trace;		//trace?
	uint32_t esp;	//kernel stack address
	int sysnum;		//syscall number
	vector<unsigned int> *p_retstack;	//retstack
	vector<unsigned int> *p_callstack;	//callstack

	uint32_t proc_addr;//for task_struct;
};

extern uint32_t g_pc;
FILE *output_database;
static FILE *output_file1;
static FILE *output_file2;
static FILE *output_file3;


void cur_dump_rets(FILE *file, unsigned int pc)
{
//#ifdef DEBUG
	fprintf(file, "%p rets:", p_cur_retstack);
	for(std::vector<unsigned int>::iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); ++it){
		fprintf(file, "%x->", *it);
	}
	fprintf(file, "%x\n", pc);
//#endif
}

void cur_insert_retaddr(unsigned int pc)
{
	(*p_cur_retstack).push_back(pc);
}

int cur_delete_retaddr(unsigned int pc)
{
	(*p_cur_retstack).pop_back();
	if((*p_cur_retstack).size() == 0) {
		return 1;	//callstack empty;	
	}
	return 0;
}

void cur_dump_callstack(FILE *file, unsigned int pc)
{
#ifdef TRACE_SYSCALL_OBJ
	fprintf(file, "%p rets:", p_cur_retstack);
	for(std::vector<unsigned int>::iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); ++it){
		fprintf(file, "%x->", *it);
	}
	fprintf(file, "%x\n", pc);
#else
	string s = get_callstack_ebp(pc);
	fprintf(file, "%s\n", s.c_str());
#endif
}


string get_cur_callstack_str(unsigned int pc)
{
#ifdef TRACE_SYSCALL_OBJ
	stringstream ss;
	string r = "rets:";
	for(std::vector<unsigned int>::iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); ++it){
		ss<<hex<<*it<<"->";
	}
	ss<<hex<<pc;
	r += ss.str();
	return r;
#else
	return get_callstack_ebp(pc);
#endif
}


void cur_insert_callstack(unsigned int pc)
{
	(*p_cur_callstack).push_back(pc);
}

int cur_delete_callstack(unsigned int pc)
{
	(*p_cur_callstack).pop_back();
	if((*p_cur_callstack).size() == 0) {
		return 1;	//callstack empty;	
	}
	return 0;
}

unsigned int cur_get_ret(void)
{
	return (*p_cur_retstack)[(*p_cur_retstack).size()-1];
}


unsigned int get_parent_callstack(void)
{
	if((*p_cur_callstack).size() >= 2) {
		vector<unsigned int>::reverse_iterator rit = (*p_cur_callstack).rbegin();
		return *(++rit);
	} else {
		return 0xffffffff;
	}
}

unsigned int get_cur_callstack(void)
{
	return (*p_cur_callstack).back();
}

void new_proc_start(uint32_t pc_start) 
{
	uint32_t new_esp = 0, old_esp = 0;
	if(disas_one_inst_ex(pc_start, &pemu_inst) == XED_ERROR_NONE) {
		xed_reg_enum_t reg_id_0;
		xed_reg_enum_t reg_id_1;
		
		const xed_operand_t *op_0 = xed_inst_operand(
				xed_decoded_inst_inst(&pemu_inst.PEMU_xedd_g), 0);
		const xed_operand_t *op_1 = xed_inst_operand(
				xed_decoded_inst_inst(&pemu_inst.PEMU_xedd_g), 1);
		
		xed_operand_enum_t op_name_0 = xed_operand_name(op_0);
		xed_operand_enum_t op_name_1 = xed_operand_name(op_1);
		
		if(operand_is_reg(op_name_0, &reg_id_0)) {
			old_esp = PEMU_get_reg(XED_REG_ESP) & 0xffffe000;
		}

		if(operand_is_reg(op_name_1, &reg_id_1)) {
			new_esp = PEMU_get_reg(reg_id_1);
		} else if(operand_is_mem(op_name_1, &new_esp, 1)) {
			PEMU_read_mem(new_esp, 4, &new_esp);
		} else {
			return;
		}
		new_esp &= 0xffffe000;
	}

#ifdef DEBUG
	fprintf(stderr, "old:%x int:%d sched:%x new:%x\n", 
			old_esp, s_int_level, s_in_sched, new_esp);
#endif
	
	//save old proc info
	if(g_all_info.count(old_esp) > 0) {
		g_all_info[old_esp].int_level = s_int_level;
		g_all_info[old_esp].in_sched = s_in_sched;
		g_all_info[old_esp].in_syscall = s_in_syscall;
		g_all_info[old_esp].sysnum = s_sysnum;
		g_all_info[old_esp].trace = s_trace;
		g_all_info[old_esp].esp = s_esp;
		g_all_info[old_esp].p_retstack = p_cur_retstack;
		g_all_info[old_esp].p_callstack = p_cur_callstack;
		g_all_info[old_esp].proc_addr = s_proc_addr;
	} else {
	}

	//set info from new proc
	if(g_all_info.count(new_esp) > 0) {
		s_int_level = g_all_info[new_esp].int_level;
		s_in_sched = g_all_info[new_esp].in_sched;
		s_in_syscall = g_all_info[new_esp].in_syscall;
		s_trace = g_all_info[new_esp].trace;
		s_esp = g_all_info[new_esp].esp;
		s_sysnum = g_all_info[new_esp].sysnum;
		p_cur_retstack = g_all_info[new_esp].p_retstack;
		p_cur_callstack = g_all_info[new_esp].p_callstack;
		s_proc_addr = g_all_info[new_esp].proc_addr;
#ifdef DEBUG
		fprintf(stderr, "new:%x int:%d sched:%x in_syscall:%x esp:%x\n",
				new_esp, s_int_level, s_in_sched, s_in_syscall, s_esp);
#endif
	} else {//if we are not at syscall context of new proc or we didn't trace it.
		s_int_level = 0;
		s_in_sched = 0;
		s_in_syscall = 0;
		s_trace = 0;
		s_esp = 0;
		s_sysnum = -1;
		p_cur_retstack = NULL;
		p_cur_callstack = NULL;
		s_proc_addr = 0;
#ifdef DEBUG
		fprintf(stderr, "esp not found in new_proc_start: %x\n", new_esp);
#endif
	}
}

/*the interrupt ocurs at syscall context??*/
int syscall_intr(uint32_t esp)
{
	return g_all_info.count(esp);
}

/*the iret ocurs at syscall context??*/
int syscall_iret(uint32_t esp)
{
	return g_all_info.count(esp);
}

/*enter syscall*/
void syscall_enter(uint32_t esp, int label)
{
//	if(PEMU_get_reg(XED_REG_EAX) == 0xfc
//			|| PEMU_get_reg(XED_REG_EAX) == 0x1) //ignore sys_exit
//		return;
	
#ifdef DEBUG
	if(g_all_retstacks.count(esp) != 0) {
		p_cur_retstack = &g_all_retstack[esp];
		cur_dump_rets(stdout, 0);
		printf("fuch shit here esp:%x %x %x %x %x %x %p\n",
				g_all_info[esp].in_syscall,
				g_all_info[esp].int_level,
				g_all_info[esp].in_sched,
				g_all_info[esp].trace,
				g_all_info[esp].esp,
				g_all_info[esp].sysnum,
				g_all_info[esp].p_retstack);

		printf("syscall_enter: %d %x %d %x label:%d\n", 
				s_int_level, 
				s_in_sched,
				s_in_syscall,
				s_esp,
				label);
		assert(g_all_retstacks.count(esp) == 0);
	}

	printf("syscall_enter:%x label:%x sysnum:%x\n", 
			esp, label, PEMU_get_reg(XED_REG_EAX));
	
	assert(g_all_retstacks.count(esp) == 0);
	assert(g_all_info.count(esp) == 0);
#endif

	struct info info;
	info.int_level = 0;
	info.in_sched = 0;
	info.in_syscall = 1;
	info.trace = 0;
	info.esp = esp;
	info.proc_addr = 0;
	info.sysnum = PEMU_get_reg(XED_REG_EAX);
	g_all_info[esp] = info;
	g_all_retstacks[esp] = vector<unsigned int>(0, 0);
	g_all_callstacks[esp] = vector<unsigned int>(0,0);

//	if(PEMU_get_reg(XED_REG_EAX) == 0xa2) {
//		info.trace = 1;
//	}

	s_int_level = 0;
	s_in_sched = 0;
	s_trace = info.trace;
	s_sysnum = info.sysnum;
	s_in_syscall = 1;
	s_esp = esp;
	p_cur_retstack = &g_all_retstacks[esp];
	p_cur_callstack = &g_all_callstacks[esp];
	s_proc_addr = 0;
	(*p_cur_retstack).clear();
	(*p_cur_callstack).clear();

	//fprintf(stderr, "new esp:%x\n", );
}

/*leave syscall*/
void syscall_exit(uint32_t esp, int label)
{
#ifdef DEBUG
	printf("syscall_exit: %x %d %x %d %x label:%d\n", 
			esp, 
			s_int_level, 
			s_in_sched,
			s_in_syscall,
			s_esp,
			label);
	
	if(p_cur_retstack != NULL) {
		if((*p_cur_retstack).size() != 0 
				&& (*p_cur_retstack).back() != 0xc1031232) {
			for(std::vector<unsigned int>::iterator it = (*p_cur_retstack).begin(); 
					it != (*p_cur_retstack).end(); ++it) {
				fprintf(stderr, "%x->", *it);
			}
			assert((*p_cur_retstack).size() == 0);
		}
		goto OUT;
	}
	assert(s_int_level == 0);
	assert(s_in_sched == 0);
	//assert(s_esp == esp);
OUT:
#endif

	g_all_retstacks.erase(esp);
	g_all_callstacks.erase(esp);
	g_all_info.erase(esp);
	
	p_cur_retstack = NULL;
	p_cur_callstack = NULL;
	s_in_sched = 0;
	s_int_level = 0;
	s_trace = 0;
	s_in_syscall = 0;
	s_sysnum = -1;
	s_esp = 0;
	s_proc_addr = 0;
}


void decr_intr(void)
{
	s_int_level--;
#ifdef DEBUG
	printf("decr esp:%x int:%d\n", get_kernel_esp() & 0xffffe000, s_int_level);
#endif
}

void inc_intr(void)
{
	s_int_level++;
#ifdef DEBUG
	printf("inc esp:%x int:%d\n", get_kernel_esp() & 0xffffe000, s_int_level);
#endif
}

int is_cur_retaddr(unsigned int pc)
{
	if((*p_cur_retstack).empty())
		return 0;
	if((*p_cur_retstack).back() == pc){
		//g_ret_stack.pop();
		return 1;
	}else {
		return 0;
	}
}

///////////////////output info////////////////////////////////////
struct DataIntem
{
	unsigned int pc;
	int size;
	char name[50];
	void *callstack;
	void *types;
	unordered_set<string> *pset;
};

map<int, unordered_set<size_t> > sys_read;
map<int, unordered_set<size_t> > sys_write;
map<int, unordered_set<size_t> > sys_create;
map<int, unordered_set<size_t> > sys_delete;
map<size_t, unordered_set<string> > delete_loc_off;
map<size_t, unordered_set<string> > read_loc_off;
map<size_t, unordered_set<string> > write_loc_off;
map<size_t, struct DataIntem> database;

map<size_t, map<int, unordered_set<string> > > read_off_type;
map<size_t, map<int, unordered_set<string> > > write_off_type;
map<size_t, map<int, string> > first_write_off_type;

map<size_t, int> vmalloc_database;

static unordered_set<unsigned int> g_dest;
static unordered_map<unsigned int, unordered_set<string> > g_point_to;

void save_vmalloc(size_t type)
{
	if(vmalloc_database.count(type) == 0) {
		cur_dump_callstack(output_database, g_pc);
		vmalloc_database[type] = 1;
	}
}

void set_readSys(int sysnum, unsigned int addr, NodeType *tmp)
{
	stringstream ss;
	sys_read[sysnum].insert(tmp->type);
	//ss << hex << hash_read_callstack(g_pc) << ":" << addr - tmp->key;
	//ss << hex << g_pc << ":" << addr - tmp->key;
	//read_loc_off[tmp->type].insert(ss.str());
	
	ss << hex << hash_read_callstack(g_pc) << ":" << g_pc;
	read_off_type[tmp->type][addr-tmp->key].insert(ss.str());
}

void set_writeSys(int sysnum, unsigned int addr, NodeType *tmp)
{
	stringstream ss;
	sys_write[sysnum].insert(tmp->type);
	//ss << hex << hash_write_callstack(g_pc) << ":" << addr - tmp->key;
	//ss << hex << g_pc << ":" << addr - tmp->key;
	//write_loc_off[tmp->type].insert(ss.str());

	ss << hex << hash_write_callstack(g_pc) << ":" << g_pc;
	if(first_write_off_type.count(tmp->type) == 0
			|| first_write_off_type[tmp->type].count(addr-tmp->key) == 0) {
		first_write_off_type[tmp->type][addr-tmp->key] = ss.str();
	}
	write_off_type[tmp->type][addr-tmp->key].insert(ss.str());

}

void update_traced_size(size_t obj, int size)
{
	if(database.count(obj) != 0 && database[obj].size < size) {
		database[obj].size = size;
		//cout<<"update "<<hex<<obj<<" "<<size<<endl;
	}
}

void set_createSys(int sysnum, size_t obj, struct DataIntem data)
{
	sys_create[sysnum].insert(obj);

	if(database.count(obj) == 0) {
		cur_dump_callstack(output_database, g_pc);
		fprintf(output_database, "%lx %x %s %d\n", 
				obj, data.pc, data.name, data.size);
		
		data.types = new map<unsigned int, unordered_set<string> >();
		data.callstack = new string(get_cur_callstack_str(g_pc));
		database[obj] = data;
		database[obj].pset = new unordered_set<string>();
	}
}

void set_deleteSys(int sysnum, NodeType *tmp)
{
	if(in_syscall_context()) {
		if(s_in_sched == 0 && s_int_level == 0) {
			stringstream ss;
			sys_delete[sysnum].insert(tmp->type);
			ss << hex << hash_delete_callstack(g_pc);
			delete_loc_off[tmp->type].insert(ss.str());
		}
	}
}


void dump_loc_off(char *fname, map<size_t, unordered_set<string> > &loc_off)
{
	FILE *file = fopen(fname, "w");
	
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}
	for(map<size_t, unordered_set<string> >::iterator it = loc_off.begin();
			it != loc_off.end(); it++) {
			fprintf(file, "%lx", it->first);
		for(auto it2 = it->second.begin();
				it2 != it->second.end(); it2++) {
			fprintf(file, " %s", (*it2).c_str());
		}
		fprintf(file, "\n");
	}
	fclose(file);
}

void dump_hash_callstack(char *fname, map<size_t, string > &hash_callstack)
{
	FILE *file = fopen(fname, "w");
	
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}
	for(map<size_t, string >::iterator it = hash_callstack.begin();
			it != hash_callstack.end(); it++) {
		fprintf(file, "%lx:%s\n", it->first, it->second.c_str());
	}
	fclose(file);

}

void dump_type_off(char *fname, map<size_t, map<int, unordered_set<string> > > &hash)
{
	FILE *file = fopen(fname, "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}
	for(map<size_t, map<int, unordered_set<string> > >::iterator it = hash.begin(); it != hash.end();
			it++) {
		fprintf(file, "%lx", (*it).first);
		for(map<int, unordered_set<string> >::iterator it2 = (*it).second.begin(); it2 != (*it).second.end();
				it2++) {
			for(auto it3 = (*it2).second.begin(); it3 != (*it2).second.end(); it3++) {
				fprintf(file, " %x:%s", (*it2).first, (*it3).c_str());
			}
		}
		fprintf(file, "\n");
	}
	fclose(file);
}

void dump_first_type_off(char *fname, map<size_t, map<int, string> > &hash)
{
	FILE *file = fopen(fname, "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}
	for(map<size_t, map<int, string> >::iterator it = hash.begin(); it != hash.end();
			it++) {
		fprintf(file, "%lx", (*it).first);
		for(map<int, string>::iterator it2 = (*it).second.begin(); it2 != (*it).second.end();
				it2++) {
			fprintf(file, " %x:%s", (*it2).first, (*it2).second.c_str());
		}
		fprintf(file, "\n");
	}
	fclose(file);
}

void dump_database(void)
{
	FILE *file = fopen("database_size", "w");
	for(map<size_t, struct DataIntem>::iterator it = database.begin();
			it != database.end(); it++) {
		fprintf(file, "%llx %s %d\n", it->first, it->second.name, it->second.size-1);
	}
	fclose(file);
}

void open_database(void)
{
	output_database = fopen("database", "w");
	output_file1 = fopen("/home/junyuan/Desktop/dump_func.s", "w");
	output_file2 = fopen("/home/junyuan/Desktop/dump_callstack_r.s", "w");
	output_file3 = fopen("/home/junyuan/Desktop/dump_callstack_w.s", "w");
	if(!output_database && !output_file1 && !output_file2) {
		fprintf(stderr, "error in open database\n");
		exit(0);
	}
}


void dump_rewards_types(char *fname)
{
	FILE *file = fopen(fname, "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}
	for(map<size_t, struct DataIntem >::iterator it = database.begin(); it != database.end();
			it++) {
		struct DataIntem data = (*it).second;
		map<unsigned int, unordered_set<string> > *mp = (map<unsigned int, unordered_set<string> >*) data.types;
		
		fprintf(file, "%lx\t%s", (*it).first, data.name);
		for(map<unsigned int, unordered_set<string> >::iterator it2 = (*mp).begin();
				it2 != (*mp).end(); it2++) {
			for(auto it3 = (*it2).second.begin(); it3 != (*it2).second.end(); it3++) {
				fprintf(file, "\t%x:%s", (*it2).first, (*it3).c_str());
			}
		}
		fprintf(file, "\n");
	}
	fclose(file);
}

void dump_objs_types(char *fname)
{
	FILE *file = fopen(fname, "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}
	for(map<size_t, struct DataIntem >::iterator it = database.begin(); it != database.end();
			it++) {
		struct DataIntem data = (*it).second;
		fprintf(file, "%s\n", ((string*)data.callstack)->c_str());
		fprintf(file, "%lx %s", (*it).first, data.name);
		fprintf(file, "\n");
	}
	fclose(file);
}

void dump_type_rw(char *fname)
{
	FILE *file = fopen(fname, "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}
	for(map<size_t, struct DataIntem >::iterator it = database.begin(); it != database.end();
			it++) {
		fprintf(file, "%llx", it->first);
		for(unordered_set<string>::iterator it2 = it->second.pset->begin();it2 != it->second.pset->end();it2++) {
			fprintf(file, " %s", (*it2).c_str());
		}
		fprintf(file, "\n");
	}
	fclose(file);
}

void dump_called_func(char *fname)
{
	FILE *file = fopen(fname, "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}

	for(unordered_set<unsigned int>::iterator it = g_dest.begin();
			it != g_dest.end();it++) {
		fprintf(file, "%x\n", *it);
	}
	fclose(file);
}

void dump_point_to(char *fname)
{
	FILE *file = fopen(fname, "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", fname);
		exit(0);
	}

	for(unordered_map<unsigned int, unordered_set<string> >::iterator it = g_point_to.begin();
			it != g_point_to.end();it++) {
		for(unordered_set<string>::iterator it2 = it->second.begin();
				it2 != it->second.end(); it2++) {
			fprintf(file, "%s\n", (*it2).c_str());
		}
	}
	fclose(file);
}

void set_dump(void)
{
	dump_rewards_types("rewards_types");
	dump_objs_types("all_objs_type");
	dump_type_rw("/home/junyuan/Desktop/dump_func1.s");
	dump_called_func("/home/junyuan/Desktop/dump_called_func.s");
	dump_point_to("/home/junyuan/Desktop/dump_point_to.s");
}

//begin (for code diff)
map<unsigned int, unordered_set<unsigned int> > call_relation;
void set_syscall_call_funcs(unsigned int pc)
{
	syscall_call_funcs[pc]++;
}

void set_call_relation(unsigned int caller, unsigned int callee)
{
	//printf("%x:%x\n", get_parent_callstack(), callee);
	call_relation[caller].insert(callee);
}

void print_call_relation(void)
{
	FILE *file = fopen("call_relation", "w");
	if(!file) {
		fprintf(stderr, "error in open %s\n", "call_relation");
		exit(0);
	}
	for(map<unsigned int, unordered_set<unsigned int> >::iterator it = call_relation.begin(); it != call_relation.end(); it++) {
		for(auto it3 = (*it).second.begin(); it3 != (*it).second.end(); it3++) {
				fprintf(file, "%x:%x\n", (*it).first, (*it3));
		}
	}

}
//end (for code diff)

//begin (for handle type sinks)
struct Args {
	int size;
	string type;
	Args(int size, string type) {
		this->size = size;
		this->type = type;
	}
};
struct Func {
	int num;
	string name;
	vector<Args> args;
};
map<unsigned int, struct Func*> g_func_interface;
unordered_set<string> type_64;
void load_function_interface(void)
{
	char line[500], fname[50], tmp[50];
	char *pline;
	int addr;
	
	FILE* file = fopen(TYPE_64, "r");
	if(!file) {
		perror(TYPE_64);
	}

	while (fgets(line, 500, file)) {
		pline = strtok(line, "\n");//strip newline
		type_64.insert(pline);
	}
	
	file = fopen(FUNCTION_INTERFACE, "r");
	if(!file) {
		perror(FUNCTION_INTERFACE);
	}
	
	while (fgets(line, 500, file)) {
		pline = strtok(line, "\n");//strip newline
		char *token = strtok(pline, "\t");
		sscanf(token, "%x", &addr);

		token = strtok(NULL, "\t");
		strcpy(fname, token);

		Func *func = new Func();
		func->name = fname;
		int num = 0;
		while ((token = strtok(NULL, "\t")) != NULL) {
			int start = 0;
			while(token[start] == ' ') {
				start++;
			}
			if(type_64.count(token+start)) {
				func->args.push_back(Args(2, token+start));
			} else {
				func->args.push_back(Args(1, token+start));
			}
			num++;
		}
		func->num = num;
		g_func_interface[addr] = func;
	}
	fclose(file);
}

int get_parameter(unsigned int *paras, struct Func* func)
{
	unsigned int val;
	int index = -1, n = func->args.size();
	paras[++index] = PEMU_get_reg(XED_REG_EAX);
	paras[++index] = PEMU_get_reg(XED_REG_EDX);
	if(n > 2) {
		paras[++index] = PEMU_get_reg(XED_REG_ECX);
	}
	for(int i = 0;i < 2*n;i++) {
		if(PEMU_read_mem(PEMU_get_reg(XED_REG_ESP)+4*i, 4, &val) != 0) {
			assert(0);
		}
		paras[++index] = val;
	}
}

//#define DEBUG
int recover_sem_types(unsigned int target_pc)
{
	if(!g_func_interface.count(target_pc)) {
		return -1;
	}
	struct Func* func = g_func_interface[target_pc];
	int index = 0;
	unsigned int paras[50];
	NodeType *p;

#if 0	
	fprintf(stdout, "%s", func->name.c_str());
	for(int i = 0;i < func->args.size();i++) {
		fprintf(stdout, " %s", func->args[i].type.c_str());
	}
	fprintf(stdout, "\n");
	fflush(stdout);
#endif
	get_parameter(paras, func);
	for(int i = 0;i < func->args.size();i++) {
		//cout<<func->args[i].type<<" "<<hex<<paras[index]<<" "<<endl;
		if(p = ds_code_rbtFind2(paras[index])) {
			if(database.count(p->type)) {
				map<unsigned int, unordered_set<string> > *mp = (map<unsigned int, unordered_set<string> >*) database[p->type].types;
				//if(paras[index] == p->key) {
#ifdef DEBUG
			cout<<func->name<<endl;
			cout<<hex<<p->type<<" "<<func->args[i].type<<" "<<hex<<paras[index]<<" "<<hex<<p->key<<" "<<paras[index]-p->key<<endl;
			cout<<"ARG:"<<i<<" "<<"INDEX:"<<index<<endl;
			cout<<flush;
#endif
					(*mp)[paras[index]-p->key].insert(func->args[i].type);
				//}
			}
		}
		index += func->args[i].size;
	}
	//cout<<endl;
	return 0;
}






struct Instance {
	size_t type;
	unordered_set<string> *pset;
};

struct ESP {
	unsigned int max_esp;
	unsigned int int_num;
	ESP(unsigned int esp, unsigned int num) {
		max_esp = esp;
		int_num = num;
	}
};

unordered_map<unsigned int, struct Instance > g_instances;
unordered_map<size_t, string> g_access_callstack;
unordered_map<unsigned int, stack<struct ESP> > g_interrupt_esp;


void cur_dump_callstack_pc(FILE *file, unsigned int pc)
{
	for(std::vector<unsigned int>::iterator it = (*p_cur_retstack).begin(); 
			it != (*p_cur_retstack).end(); ++it){
		fprintf(file, "%x->", *it);
	}
	fprintf(file, "%x\n", pc);
}

void set_info_proc(uint32_t addr)
{
	uint32_t esp = get_kernel_esp() & 0xffffe000;
	if(g_all_info[esp].proc_addr) {
		fprintf(stderr, "%x %x\n", esp, g_all_info[esp].proc_addr);
		assert(0);
	}
	g_all_info[esp].proc_addr = addr;
}

void add_instance(size_t type, unsigned int addr)
{
	if(g_instances.count(addr)) {
		fprintf(stderr, "addr:%x\told:%llx\tnew:%llx\n", 
				addr, g_instances[addr].type, type);
		//assert(0);
	}

	g_instances[addr].type = type;
	g_instances[addr].pset = new unordered_set<string>();
	//set_info_proc(addr);
}

//delete flag: -1:exception 0:normal
void delete_instance(unsigned int addr)//, int flag)
{
	//if(!g_instances.count(addr)) {
	//	assert(0);
	//}
#ifdef RECORD_INSTANCE_ACCESS 
	fprintf(output_file1, "%llx\t", g_instances[addr].type);
	for(unordered_set<string>::iterator it2 = g_instances[addr].pset->begin();
			it2 != g_instances[addr].pset->end();
			it2++) {
		fprintf(output_file1, "%s\t", it2->c_str());
	}
	fprintf(output_file1, "\n");
#endif
	if(g_instances.count(addr)) {
		delete(g_instances[addr].pset);
		g_instances.erase(addr);
	}
}

static inline string int_to_string(unsigned int addr)
{
   	stringstream ss;
	ss << hex <<addr;
	return ss.str();
}


struct ESP get_interrupt_new(unsigned int esp_key);
string get_callstack_ebp(uint32_t pc)
{
#if 0
	uint32_t ret_addr;
	uint32_t kernel_esp = get_kernel_esp() & 0xffffe000;
	string str;
	
	uint32_t ebp = PEMU_get_reg(XED_REG_EBP);

	str = int_to_string(pc);	
	while(1) {
		if(ebp < kernel_esp || ebp > kernel_esp+STACK_SIZE) {
			break;
		}
		PEMU_read_mem(ebp+4, 4, &ret_addr);
		if(NO_TEXT) { //make sure text range
			break;
		}
		str = int_to_string(ret_addr)+"->"+str;
		if(TIME_INTERRUPT || COMMON_INTERRUPT || COMMON_EXCEPTION) { //interrupt context
			break;
		}
		PEMU_read_mem(ebp, 4, &ebp);
	}
//	cout<<str<<endl;
	return str;
#endif
	uint32_t ret_addr;
	uint32_t kernel_esp = PEMU_get_reg(XED_REG_ESP) & 0xffffe000;
	string str;

	uint32_t esp_max;
	uint32_t int_num;
	struct ESP info = get_interrupt_new(kernel_esp);
	if(info.max_esp != 0) {
		esp_max = info.max_esp;
		int_num = info.int_num;
	} else {
		esp_max = kernel_esp + STACK_SIZE;
		int_num = -1;
	}
	
	
	uint32_t ebp;
	if(HARD_CODE_SCHED1) {
		PEMU_read_mem(PEMU_get_reg(XED_REG_ESP), 4, &ebp);
	} else {
		ebp = PEMU_get_reg(XED_REG_EBP);
	}

	str = int_to_string(pc);
	while(1) {
		if(ebp < kernel_esp || ebp+4 >= esp_max) {
			break;
		}
		PEMU_read_mem(ebp+4, 4, &ret_addr);

		str = int_to_string(ret_addr)+"->"+str;
//		str += " " + int_to_string(ebp);
		if(HARD_CODE_SCHED2) {
			PEMU_read_mem(ebp+8, 4, &ebp);
		} else if(PEMU_read_mem(ebp, 4, &ebp) < 0 
				|| TIME_INTERRUPT
				|| COMMON_INTERRUPT
				|| COMMON_EXCEPTION) {
			break;
		} 	
	}
//	cout<<str<<endl;
	if(int_num != -1) {
		str = "I-" + to_string(int_num) + ":" + str;
	}
//	str += " " + int_to_string(ebp)+" "+ int_to_string(esp_max);
	return str;
}

void add_read_action(unsigned int addr, unsigned int size, unsigned int off, unsigned int func, unsigned int action)
{
#if 0
	if(!g_instances.count(addr)) {
		assert(0);
	}
#endif

	string cs = get_callstack_ebp(g_pc);
	hash<std::string> hash_fn;
	size_t hash = hash_fn(cs);
	if(!g_access_callstack.count(hash)) {
		g_access_callstack[hash] = cs;
		fprintf(output_file2, "%llx:%s\n", hash, cs.c_str());
	}
#if 0
	stringstream ss;
	//ss <<hex<<func<<" "<<hex<<off<<" "<<action;
	ss <<hex<<hash<<":"<<get_mem_taint2(addr+off)<<":"<<hex<<func<<":"<<hex<<off;
	//set_mem_taint_bysize2(addr, 1, 1);
	string s = ss.str();
#ifdef RECORD_INSTANCE_ACCESS
	g_instances[addr].pset->insert(s);
#endif
#ifdef RECORD_TYPE_ACCESS
	database[g_instances[addr].type].pset->insert(s);
#endif
#endif
}


void add_write_action(unsigned int addr, unsigned int size, unsigned int off, unsigned int func, unsigned int action)
{
#if 0
	if(!g_instances.count(addr)) {
		assert(0);
	}
#endif

	string cs = get_callstack_ebp(g_pc);
	hash<std::string> hash_fn;
	size_t hash = hash_fn(cs);
	if(!g_access_callstack.count(hash)) {
		g_access_callstack[hash] = cs;
		fprintf(output_file3, "%llx:%s\n", hash, cs.c_str());
	}
#if 0
	stringstream ss;
	//ss <<hex<<func<<" "<<hex<<off<<" "<<action;
	ss <<hex<<hash<<":"<<get_mem_taint2(addr+off)<<":"<<hex<<func<<":"<<hex<<off;
	set_mem_taint_bysize2(addr+off, 1, size);
	string s = ss.str();
#ifdef RECORD_INSTANCE_ACCESS
	g_instances[addr].pset->insert(s);
#endif
#ifdef RECORD_TYPE_ACCESS
	database[g_instances[addr].type].pset->insert(s);
#endif
#endif
}


void add_interrupt_esp(unsigned int esp_key, unsigned int max_esp, int int_num)
{
	struct ESP e(max_esp, int_num);
	g_interrupt_esp[esp_key].push(e);
}

struct ESP get_interrupt_new(unsigned int esp_key)
{
	ESP r(0, 0);
	if(g_interrupt_esp.count(esp_key)) {
		r = g_interrupt_esp[esp_key].top();
	}
	return r;
}


void delete_interrupt_esp(unsigned int esp_key)
{
	if(g_interrupt_esp.count(esp_key)) {
		if(g_interrupt_esp[esp_key].size() != 0) {
			//assert(0);
			g_interrupt_esp[esp_key].pop();
			if(g_interrupt_esp[esp_key].size() == 0) {
				g_interrupt_esp.erase(esp_key);
			}
		}
	} else {
		//assert(0);
	}
}	



void add_pointTo(unsigned int pc, size_t src, size_t dst)
{
	stringstream ss;
	ss <<hex<<pc<<":"<<hex<<src<<"->"<<hex<<dst;
	string s = ss.str();
	g_point_to[pc].insert(s);
}


void add_call_dst(unsigned int pc)
{
	g_dest.insert(pc);
}

void check_if_success(void)
{
#if 0
	uint32_t esp = get_kernel_esp() & 0xffffe000;
	if((int)PEMU_get_reg(XED_REG_EAX) < 0
			&&
			(g_all_info[esp].sysnum == 120 || g_all_info[esp].sysnum == 2 
				|| g_all_info[esp].sysnum == 190)) {
		fprintf(stderr, "proc create failed\n", g_all_info[esp].proc_addr);
		g_instances[g_all_info[esp].proc_addr];
	}
#endif
}


//end




}
