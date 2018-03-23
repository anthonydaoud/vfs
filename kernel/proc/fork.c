#include "types.h"
#include "globals.h"
#include "errno.h"

#include "util/debug.h"
#include "util/string.h"

#include "proc/proc.h"
#include "proc/kthread.h"

#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/page.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "mm/pagetable.h"
#include "mm/tlb.h"

#include "fs/file.h"
#include "fs/vnode.h"

#include "vm/shadow.h"
#include "vm/vmmap.h"

#include "api/exec.h"

#include "main/interrupt.h"

/* Pushes the appropriate things onto the kernel stack of a newly forked thread
 * so that it can begin execution in userland_entry.
 * regs: registers the new thread should have on execution
 * kstack: location of the new thread's kernel stack
 * Returns the new stack pointer on success. */
static uint32_t
fork_setup_stack(const regs_t *regs, void *kstack)
{
        /* Pointer argument and dummy return address, and userland dummy return
         * address */
        uint32_t esp = ((uint32_t) kstack) + DEFAULT_STACK_SIZE - (sizeof(regs_t) + 12);
        *(void **)(esp + 4) = (void *)(esp + 8); /* Set the argument to point to location of struct on stack */
        memcpy((void *)(esp + 8), regs, sizeof(regs_t)); /* Copy over struct */
        return esp;
}

/* VM BLANK {{{ */

/* Helper; Allocates two shadow objects for each private-mapped area of the
 * current process and chains them together in a list with their
 * mmo_shadowed pointers. Returns the list on success, null on failure. */
static mmobj_t *
newobjs_alloc(void)
{
        vmarea_t *vma;
        mmobj_t *newobjs = NULL;

        list_iterate_begin(&curproc->p_vmmap->vmm_list, vma, vmarea_t, vma_plink) {
                if (vma->vma_flags & MAP_PRIVATE) {
                        dbg(DBG_FORK, "newobjs_alloc: area from pn 0x%05x to 0x%05x "
                            "is private mapped, initializing two shadow objects\n",
                            vma->vma_start, vma->vma_end);

                        mmobj_t *o;
                        if (NULL == (o = shadow_create())) {
                                goto fail;
                        }
                        o->mmo_shadowed = newobjs;
                        newobjs = o;

                        if (NULL == (o = shadow_create())) {
                                goto fail;
                        }
                        o->mmo_shadowed = newobjs;
                        newobjs = o;
                }
        } list_iterate_end();
        return newobjs;

fail:
        /* This cleans up all the objects because of how put works */
        newobjs->mmo_ops->put(newobjs);
        return NULL;
}

/* Helper: sets up the new areas and shadow mmobjs appropriately */
static void
setup_mmobjs(proc_t *newproc, mmobj_t *newobjs)
{
        /* Parent and child areas and links */
        vmarea_t *oldvma, *newvma;
        list_link_t *oldlink, *newlink;
        /* For every pair of areas in the processes */
        for (oldlink = curproc->p_vmmap->vmm_list.l_next, newlink = newproc->p_vmmap->vmm_list.l_next;
             oldlink != &(curproc->p_vmmap->vmm_list) && newlink != &(newproc->p_vmmap->vmm_list);
             oldlink = oldlink->l_next, newlink = newlink->l_next) {
                oldvma = list_item(oldlink, vmarea_t, vma_plink);
                newvma = list_item(newlink, vmarea_t, vma_plink);
                /* Make sure we copied the stuff correctly */
                KASSERT(oldvma->vma_start == newvma->vma_start
                        && oldvma->vma_end == newvma->vma_end
                        && oldvma->vma_prot == newvma->vma_prot
                        && oldvma->vma_flags == newvma->vma_flags
                        && oldvma->vma_off == newvma->vma_off);

                if (oldvma->vma_flags & MAP_PRIVATE) {
                        /* the base object, and the two new shadow objects we will set up */
                        mmobj_t *base, *oldshadow, *newshadow;
                        base = oldvma->vma_obj;

                        /* Pop the two new shadow objects off the list */
                        KASSERT(newobjs != NULL);
                        oldshadow = newobjs;
                        newobjs = newobjs->mmo_shadowed;

                        KASSERT(newobjs != NULL);
                        newshadow = newobjs;
                        newobjs = newobjs->mmo_shadowed;

                        /* set up shadow pointers */
                        oldshadow->mmo_shadowed = base;
                        newshadow->mmo_shadowed = base;
                        if (base->mmo_shadowed != NULL) {
                                oldshadow->mmo_un.mmo_bottom_obj = base->mmo_un.mmo_bottom_obj;
                                newshadow->mmo_un.mmo_bottom_obj = base->mmo_un.mmo_bottom_obj;
                        } else {
                                oldshadow->mmo_un.mmo_bottom_obj = base;
                                newshadow->mmo_un.mmo_bottom_obj = base;
                        }

                        /* Set up area pointers to shadow objects */
                        oldvma->vma_obj = oldshadow;
                        newvma->vma_obj = newshadow;

                        /* Add the new area to the bottom object areas list */
                        list_insert_tail(&(oldshadow->mmo_un.mmo_bottom_obj->mmo_un.mmo_vmas), &(newvma->vma_olink));

                        /* Note that the only refcount that changes is the parent's, as
                         * the children already have refcount 1 (correct) - the parent
                         * loses an area reference but gains 2 shadows, so up by one */
                        base->mmo_ops->ref(base);
                } else {
                        /* Don't need to allocate objects, just point both areas to the same
                         * one */
                        newvma->vma_obj = oldvma->vma_obj;
                        oldvma->vma_obj->mmo_ops->ref(oldvma->vma_obj);
                }

        }
        KASSERT(newobjs == NULL); /* We shouldn't be leaking memory in our obj list */
}

/* Helper: Copies the file descriptor table */
static void
copy_fds(proc_t *newproc)
{
        int i;
        memcpy(newproc->p_files, curproc->p_files, NFILES * sizeof(file_t *));
        for (i = 0; i < NFILES; i++) {
                if (curproc->p_files[i] != NULL) {
                        fref(curproc->p_files[i]);
                }
        }
}
/* VM BLANK }}} */

/*
 * The implementation of fork(2). Once this works,
 * you're practically home free. This is what the
 * entirety of Weenix has been leading up to.
 * Go forth and conquer.
 */
int
do_fork(struct regs *regs)
{
        vmarea_t *vma, *clone_vma;
        pframe_t *pf;
        mmobj_t *to_delete, *new_shadowed;

        /* VM {{{ */
        KASSERT(regs != NULL);
        KASSERT(curproc != NULL);
        KASSERT(curproc->p_state == PROC_RUNNING);

        /* To clean up on failure */
        kthread_t *newthr = NULL;
        /* When we allocate all the new vm objects, we're going to chain them in a
         * list using their mmo_shadowed pointers -> last entry has NULL, etc. */
        mmobj_t *newobjs = NULL;
        vmmap_t *newmap = NULL;
        proc_t *newproc = NULL;


        if (NULL == (newthr = kthread_clone(curthr))) {
                goto fail;
        }
        dbg(DBG_FORK, "kthread copied (new kstack at 0x%p)\n", newthr->kt_kstack);

        if (NULL == (newmap = vmmap_clone(curproc->p_vmmap))) {
                goto fail;
        }
        dbg(DBG_FORK, "vmmap copied (new vmmap at 0x%p)\n", newmap);

        if (NULL == (newobjs = newobjs_alloc())) {
                goto fail;
        }

        if (NULL == (newproc = proc_create(curproc->p_comm))) {
                goto fail;
        }

        /* Once we've made it this far, we cannot fail */
        KASSERT(newproc->p_state == PROC_RUNNING);
        KASSERT(newproc->p_pagedir != NULL);
        dbg(DBG_FORK, "new process allocated with pid %d, "
            "pagedir at 0x%p\n", newproc->p_pid, newproc->p_pagedir);

        vmmap_destroy(newproc->p_vmmap);
        newproc->p_brk = curproc->p_brk;
        newproc->p_start_brk = curproc->p_start_brk;
        newproc->p_vmmap = newmap;
        newmap->vmm_proc = newproc;
        list_insert_tail(&newproc->p_threads, &newthr->kt_plink);
        newthr->kt_proc = newproc;

        /* Set up new objects and areas correctly */
        setup_mmobjs(newproc, newobjs);

        /* Copy open file descriptors */
        copy_fds(newproc);

        /* Set up context */
        KASSERT(newproc->p_pagedir != NULL);
        KASSERT(newthr->kt_kstack != NULL);

        /* Deal with shadows on fork() */

        list_iterate_begin(&curproc->p_vmmap->vmm_list, vma, vmarea_t, vma_plink) {
                mmobj_t *last = vma->vma_obj, *o = last->mmo_shadowed;
                last->mmo_ops->ref(last);
                while (NULL != o && NULL != o->mmo_shadowed) {
                        mmobj_t *shadow = o->mmo_shadowed;
                        /* iff the object has only one parent, and is not right under vm_area */
                        KASSERT(o != last);
                        if (o->mmo_refcount - o->mmo_nrespages == 1) {
                                /* migrate all its pages to last, and remove it from the shadow tree */
                                pframe_t *pf;
                                list_iterate_begin(&o->mmo_respages, pf, pframe_t, pf_olink) {
                                        /* Because the operations that could be
                                         * performed with an intermediate shadow object
                                         * to make pages busy are non-blocking,
                                         * we always expect to see non-busy pages. */
                                        KASSERT(!pframe_is_busy(pf));
                                        /* o has refcount 1+nrespages, so this won't delete it yet */
                                        pframe_migrate(pf, last);
                                } list_iterate_end();
                                last->mmo_shadowed = o->mmo_shadowed;
                                /* Ref o's shadowed, so we don't accidentally delete it when we
                                 * finally put o */
                                o->mmo_shadowed->mmo_ops->ref(o->mmo_shadowed);
                                KASSERT(o->mmo_refcount == 1 && o->mmo_nrespages == 0);
                                o->mmo_ops->put(o);
                        } else {
                                KASSERT(o->mmo_refcount - o->mmo_nrespages > 1);
                                o->mmo_ops->ref(o);
                                last->mmo_ops->put(last);
                                last = o;
                        }
                        o = shadow;
                }
                KASSERT(NULL != last);
                last->mmo_ops->put(last);
        } list_iterate_end();

        newthr->kt_ctx.c_pdptr = newproc->p_pagedir;
        newthr->kt_ctx.c_eip = (uint32_t) &userland_entry;
        /* Remember to start at correct end of stack! */
        newthr->kt_ctx.c_esp = (uint32_t)newthr->kt_kstack + DEFAULT_STACK_SIZE;
        newthr->kt_ctx.c_ebp = newthr->kt_ctx.c_esp;

        newthr->kt_ctx.c_kstack = (uintptr_t) newthr->kt_kstack;
        newthr->kt_ctx.c_kstacksz = DEFAULT_STACK_SIZE;

        /* Fork returns zero in child */
        regs_t newregs;
        memcpy(&newregs, regs, sizeof(regs_t));
        newregs.r_eax = 0;
        newthr->kt_ctx.c_esp = fork_setup_stack(&newregs, newthr->kt_kstack);
        newthr->kt_ctx.c_ebp = newthr->kt_ctx.c_esp;

        /* Flush all the old pagetable mappings and TLB */
        pt_unmap_range(curproc->p_pagedir, USER_MEM_LOW, USER_MEM_HIGH);
        tlb_flush_all();

        sched_make_runnable(newthr);

        return newproc->p_pid;

fail:
        if (newobjs != NULL) {
                newobjs->mmo_ops->put(newobjs);
        }
        if (newmap != NULL) {
                vmmap_destroy(newmap);
        }
        if (newthr != NULL) {
                kthread_destroy(newthr);
        }
        return -ENOMEM;
        /* VM }}} */
        return 0;
}
