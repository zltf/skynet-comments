#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

struct modules {
	int count;  // modules的数量
	struct spinlock lock;  // 自旋锁，避免多个线程同时向skynet_module写入数据，保证线程安全
	const char * path;  // 由skynet配置表中的cpath指定，一般包含./cservice/?.so路径
	struct skynet_module m[MAX_MODULE_TYPE];  // 存放服务模块的数组，最多32类
};

static struct modules * M = NULL;

// 加载c服务的so文件
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

    // 因为要用name替换问号，所以这里长度不需要加1（必定至少会删除一个问号）
	int sz = path_size + name_size;
	//search path
	void * dl = NULL;
    // 存放模块地址
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
        // 去除path前的：;
		while (*path == ';') path++;
        // 如果path为空字符串，跳出循环
		if (*path == '\0') break;
        // 寻找第一个;的位置（多个path用;分隔）
		l = strchr(path, ';');
        // 如果没有分号就定位到字符串后一字节的地址
		if (l == NULL) l = path + strlen(path);
        // 计算第一个path长度
		int len = l - path;
		int i;
        // 对问号前的内容进行拷贝
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
        // 用name替换问号，如果没有问号，就是将name拼在path最后（实际没用，因为path没有问号是错误的）
		memcpy(tmp+i,name,name_size);
		if (path[i] == '?') {
            // path有问号，把问号后面的内容拼再name后面
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
            // path没问号，报错
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
        // 加载动态库，可能会失败
        // flag见：https://blog.csdn.net/nicholas_dlut/article/details/101758101
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
        // path指向字符串中下一个path（;后）的地址，循环直到成功加载动态库
		path = l;
	}while(dl == NULL);

    // 加载失败，报错
	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

// 从modules中返回对应服务的skynet_module
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

static void *
get_api(struct skynet_module *mod, const char *api_name) {
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	return dlsym(mod->module, ptr);
}

static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create");
	mod->init = get_api(mod, "_init");
	mod->release = get_api(mod, "_release");
	mod->signal = get_api(mod, "_signal");

	return mod->init == NULL;
}

struct skynet_module * 
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name);
	if (result)
		return result;

    // 没找到对应服务

    // 加自旋锁
	SPIN_LOCK(M)

    // 双重校验锁思想，防止多线程环境下重复加载skynet_module
	result = _query(name); // double check

	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;
		void * dl = _try_open(M,name);
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;

			if (open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

    // 解锁
	SPIN_UNLOCK(M)

	return result;
}

// 先判断没有create，没有的话就是一个二进制全1的地址
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

// 一定得有init
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

// 可以没有release
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

// 可以没有signal
void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

// 初始化modules
void 
skynet_module_init(const char *path) {
    // 为modules分配空间
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
    // 复制path，复制到堆
	m->path = skynet_strdup(path);

    // 初始化自旋锁
	SPIN_INIT(m)

    // 全局的M赋值
	M = m;
}
