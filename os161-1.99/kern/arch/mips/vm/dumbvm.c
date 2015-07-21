/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <elf.h>
#include <syscall.h>
#include <coremap_entry.h>
#include "opt-A3.h"

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

#if OPT_A3
paddr_t startaddr;
paddr_t lastaddr;

int first_page_index;
int number_of_pages;

struct spinlock coremap_lock;
struct coremap_entry* coremap;

bool vm_is_bootstrapped = false;
#endif // OPT_A3

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
void
vm_bootstrap(void)
{
    spinlock_init(&coremap_lock);
    ram_getsize(&startaddr, &lastaddr);

    coremap = (struct coremap_entry*) PADDR_TO_KVADDR(startaddr);
    number_of_pages = (lastaddr - startaddr) / PAGE_SIZE;
    first_page_index = (sizeof(struct coremap_entry) * number_of_pages) / PAGE_SIZE + 1;

    for(int i = 0; i < number_of_pages; ++i) {
	coremap[i] = coremap_entry_default;
	if(i < first_page_index) {
	    coremap[i].num_of_owners = 1;
	    coremap[i].num_pages_used = 1;
	}
    }

    vm_is_bootstrapped = true;
}
#else
vm_bootstrap(void)
{

}
#endif // OPT_A3

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
	return addr;
}

paddr_t
page_alloc(unsigned long npages)
{
    paddr_t pa = 0;

    spinlock_acquire(&coremap_lock);
    pa = unprotected_page_alloc(npages);
    spinlock_release(&coremap_lock);

    return pa;
}

// For if you already have the coremap locked and need to alloc a page.
paddr_t
unprotected_page_alloc(unsigned long npages)
{
    paddr_t pa = 0;
    unsigned long n = 0;

    for(int i = first_page_index; i < number_of_pages; ++i) {
	if(coremap[i].num_of_owners < 1) ++n;
	else				 n = 0;

	if(n == npages) {
	    for(int j = i - n + 1; j <= i; ++j) {
		coremap[j].num_of_owners = 1;
	    }
	    coremap[i-n+1].num_pages_used = npages;
	    pa = (i-n+1) * PAGE_SIZE + startaddr;
	    break;
	}
    }

    return pa;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
#if OPT_A3
	paddr_t pa;

	if(vm_is_bootstrapped) pa = page_alloc(npages);
	else                   pa = getppages(npages);

	if(pa == 0) return 0;
	return PADDR_TO_KVADDR(pa);
#else
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
#endif // OPT_A3
}

void 
free_kpages(vaddr_t addr)
{
#if OPT_A3
    addr &= PAGE_FRAME;

    int i = (KVADDR_TO_PADDR(addr) - startaddr) / PAGE_SIZE;
    spinlock_acquire(&coremap_lock);
    if(coremap[i].num_of_owners > 1) {
	--coremap[i].num_of_owners;
    } else {
	paddr_t paddr = startaddr + i * PAGE_SIZE;
	int num_pages_used = coremap[i].num_pages_used;
	spinlock_release(&coremap_lock);
	bzero((void *)PADDR_TO_KVADDR(paddr), num_pages_used * PAGE_SIZE);
	spinlock_acquire(&coremap_lock);
	for(int j = i; j < i + coremap[i].num_pages_used; ++j) {
	    coremap[j].num_of_owners = 0;
	}
	coremap[i].num_pages_used = 0;
    }
    spinlock_release(&coremap_lock);
#else
	(void)addr;
#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
#if OPT_A3
		sys__exit(1);
#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
#if OPT_A3
	KASSERT(as->as_pagedir != 0);
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	bool writeable;
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		writeable = (as->as_permissions1 & PF_W) != 0;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		writeable = (as->as_permissions2 & PF_W) != 0;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		writeable = true;
	}
	else {
		return EFAULT;
	}

	int dir_number = faultaddress >> 22;
	int page_number = (faultaddress << 10) >> 22;

	if(as->as_pagedir[dir_number] == NULL) {
	    as->as_pagedir[dir_number] = kmalloc(PAGE_TABLE_SIZE * sizeof(paddr_t));
	}
	if(as->as_pagedir[dir_number] == NULL) {
	    return ENOMEM;
	}

	if(as->as_pagedir[dir_number][page_number] == 0) {
	    as->as_pagedir[dir_number][page_number] = page_alloc(1);
	    if(as->as_pagedir[dir_number][page_number] == 0) {
		return ENOMEM;
	    }
	}
	paddr = as->as_pagedir[dir_number][page_number];

	int index = (paddr - startaddr) / PAGE_SIZE;
	spinlock_acquire(&coremap_lock);
	if(coremap[index].num_of_owners > 1) {
	    as->as_pagedir[dir_number][page_number] = unprotected_page_alloc(1);
	    if(as->as_pagedir[dir_number][page_number] == 0) {
		return ENOMEM;
	    }
	    memmove((void*) PADDR_TO_KVADDR(as->as_pagedir[dir_number][page_number]),
		    (const void*) PADDR_TO_KVADDR(paddr),
		    PAGE_SIZE);
	    paddr = as->as_pagedir[dir_number][page_number];
	    --coremap[index].num_of_owners;
	}
	spinlock_release(&coremap_lock);
#else
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}
#endif // OPT_A3

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

#if OPT_A3
    for (i=0; i<NUM_TLB; i++) {
	tlb_read(&ehi, &elo, i);
	if(!(elo & TLBLO_VALID)) break;
    }

    ehi = faultaddress;
    elo = paddr | TLBLO_VALID;
    if(writeable) elo |= TLBLO_DIRTY;

    DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
    if(i==NUM_TLB) tlb_random(ehi, elo); // TLB is full simply overwrite a random entry
    else	   tlb_write(ehi, elo, i);

    splx(spl);
    return 0;
#else
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif
}

#if OPT_A3
void
update_readonly_tlb(struct addrspace* as)
{
    uint32_t ehi, elo;
    vaddr_t vbase1 = as->as_vbase1;
    vaddr_t vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
    vaddr_t vbase2 = as->as_vbase1;
    vaddr_t vtop2 = vbase1 + as->as_npages1 * PAGE_SIZE;

    for (int i = 0; i < NUM_TLB; ++i) {
	tlb_read(&ehi, &elo, i);
	if(elo & TLBLO_VALID) {
	    vaddr_t faultaddress = ehi;
	    if (((faultaddress >= vbase1 && faultaddress < vtop1 &&
		 ((as->as_permissions1 & PF_W) == 0))) ||
		((faultaddress >= vbase2 && faultaddress < vtop2 &&
		 ((as->as_permissions2 & PF_W) == 0))))
	    {
		elo &= !TLBLO_DIRTY; // Remove writeable
		tlb_write(ehi, elo, i); // Add back to TLB
	    }
	}
    }
}
#endif

struct addrspace *
as_create(void)
{
#if OPT_A3
    struct addrspace* as = kmalloc(sizeof(struct addrspace));
    if(as == NULL) return NULL;

    as->as_pagedir = kmalloc(PAGE_DIR_SIZE * sizeof(paddr_t*));
    if(as->as_pagedir == NULL) {
	kfree(as);
	return NULL;
    }

    as->as_vbase1 = 0;
    as->as_npages1 = 0;
    as->as_permissions1 = 0;

    as->as_vbase2 = 0;
    as->as_npages2 = 0;
    as->as_permissions2 = 0;

    return as;
#else
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_permissions1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_permissions2 = 0;
	as->as_stackpbase = 0;

	return as;
#endif //OPT_A3
}

void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	for(int  i = 0; i < PAGE_DIR_SIZE; ++i) {
	    if(as->as_pagedir[i] != NULL) {
		spinlock_acquire(&coremap_lock);
		for(int j = 0; j < PAGE_TABLE_SIZE; ++j) {
		    if(as->as_pagedir[i][j] != 0) {
			int k = (as->as_pagedir[i][j] - startaddr) / PAGE_SIZE;
			if(coremap[k].num_of_owners == 1) {
			    coremap[k].num_pages_used = 0;
			}
			--coremap[k].num_of_owners;
			spinlock_release(&coremap_lock);
			bzero((void *)PADDR_TO_KVADDR(as->as_pagedir[i][j]), PAGE_SIZE);
			spinlock_acquire(&coremap_lock);
		    }
		}
		spinlock_release(&coremap_lock);
		kfree(as->as_pagedir[i]);
	    }
	}

	kfree(as->as_pagedir);
#endif //OPT_A3
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

#if OPT_A3
#else
	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;
#endif

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
#if OPT_A3
		as->as_permissions1 |= readable | writeable | executable;
#endif
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
#if OPT_A3
		as->as_permissions2 |= readable | writeable | executable;
#endif
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

#if OPT_A3
#else
static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
#endif // OPT_A3

int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
    (void) as;
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif // OPT_A3
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
#if OPT_A3
	(void) as;
#else
	KASSERT(as->as_stackpbase != 0);
#endif

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

#if OPT_A3
	for(int i = 0; i < PAGE_DIR_SIZE; ++i) {
	    if(old->as_pagedir[i] != NULL) {
		new->as_pagedir[i] = kmalloc(PAGE_TABLE_SIZE * sizeof(paddr_t));
		if(new->as_pagedir[i] == NULL) {
		    as_destroy(new);
		    return ENOMEM;
		}
		for(int j = 0; j < PAGE_TABLE_SIZE; ++j) {
		    if(old->as_pagedir[i][j] != 0) {
			new->as_pagedir[i][j] = old->as_pagedir[i][j];
			int index = (new->as_pagedir[i][j] - startaddr) / PAGE_SIZE;
			spinlock_acquire(&coremap_lock);
			coremap[index].num_of_owners++;
			spinlock_release(&coremap_lock);
		    }
		}
	    }
	}

	as_activate(); // Clear TLB so on next write Copy-on-Write will take effect
#else
	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if(as_prepare_load(as) == 0) {
	    as_destroy(new);
	    return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);	
#endif // OPT_A3

	*ret = new;
	return 0;
}
