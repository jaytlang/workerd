/* vm.c
 * virtual machine management
 * though vmctl(8)
 * (c) jay lang, 2023
 */

#include <sys/types.h>
#include <sys/time.h>

#include <event.h>
#include <stdlib.h>

#include "workerd.h"

#define VM_BOOTSTATE	0
#define VM_READYSTATE	1
#define VM_WORKSTATE	2
#define VM_MAXSTATE	3

#define VM_NOKEY	-1

struct vm {
	int			 initialized;
	int			 state;	
	int			 key;

	char			*deriveddisk;

	struct conn		*conn;
	struct vm_interface	 callbacks;
};


static struct vm	 allvms[VM_MAXCOUNT] = { 0 };

static void	 vm_init(struct vm *);
static char	*vm_getname(struct vm *);

static void
vm_boot(struct vm *v)
{
	if 	
}

static void
vm_init(struct vm *v)
{
	v->initialized = 1;
	v->state = VM_BOOTSTATE;
	v->key = -1;

	
}
