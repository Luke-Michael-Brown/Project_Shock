#ifndef _COREMAP_ENTRY_H_
#define _COREMAP_ENTRY_H_

#include <types.h>

struct coremap_entry {
    int num_of_owners;
    int num_pages_used;
} coremap_entry_default = {0, 0};

#endif /* _COREMAP_ENTRY_H_ */

