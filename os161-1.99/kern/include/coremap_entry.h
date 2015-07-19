#ifndef _COREMAP_ENTRY_H_
#define _COREMAP_ENTRY_H_

#include <types.h>

struct coremap_entry {
    paddr_t pa;
    bool is_used;
    int num_pages_used;
    struct addrspace* as;
} coremap_entry_default = {(paddr_t) NULL, false, 0, NULL};

#endif /* _COREMAP_ENTRY_H_ */

