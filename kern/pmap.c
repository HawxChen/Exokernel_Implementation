/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/kclock.h>

//#define __ALL_COUNT__

#ifdef __ALL_COUNT__
#define __PT_REF__
#define __PD_REF__
#endif
// These variables are set by i386_detect_memory()
size_t npages;                  // Amount of physical memory (in pages)
static size_t nAvailPages;      //Hawx: Amount of available physical memory (in pages) 
static size_t npages_basemem;   // Amount of base memory (in pages)
static size_t BaseMemBound;     // Amount of base memory (in pages)
static size_t UpperMemBound;    // Amount of base memory (in pages)

// These variables are set in mem_init()
pde_t *kern_pgdir;              // Kernel's initial page directory
struct Page *pages;             // Physical page state array
static struct Page *page_free_list; // Free list of physical pages
static struct Page *tail_free_page; // Free list of physical pages


// --------------------------------------------------------------
// Detect machine's physical memory setup.
// --------------------------------------------------------------

static int
nvram_read (int r)
{
    return mc146818_read (r) | (mc146818_read (r + 1) << 8);
}

/*
 *Hawx: descide the value of npages.
 *                           npages_basemem
 *                           npages_extmem
 */
static void
i386_detect_memory (void)
{
    size_t npages_extmem;

    // Use CMOS calls to measure available base & extended memory.
    // (CMOS calls return results in kilobytes.)
    npages_basemem = (nvram_read (NVRAM_BASELO) * 1024) / PGSIZE;
    npages_extmem = (nvram_read (NVRAM_EXTLO) * 1024) / PGSIZE;


    // Calculate the number of physical pages available in both base
    // and extended memory.
    if (npages_extmem)
        npages = (EXTPHYSMEM / PGSIZE) + npages_extmem;
    else
        npages = npages_basemem;
    cprintf ("Physical memory: %uK available, base = %uK, extended = %uK\n",
             npages * PGSIZE / 1024,
             npages_basemem * PGSIZE / 1024, npages_extmem * PGSIZE / 1024);
}


// --------------------------------------------------------------
// Set up memory mappings above UTOP.
// --------------------------------------------------------------

static void check_page_free_list (bool only_low_memory);
static void check_page_alloc (void);
static void check_kern_pgdir (void);
static physaddr_t check_va2pa (pde_t * pgdir, uintptr_t va);
static void check_page (void);
static void check_page_installed_pgdir (void);
static void boot_map_region (pde_t * pgdir, uintptr_t va, size_t size,
                      physaddr_t pa, int perm);

// This simple physical memory allocator is used only while JOS is setting
// up its virtual memory system.  page_alloc() is the real allocator.
//
// If n>0, allocates enough pages of contiguous physical memory to hold 'n'
// bytes.  Doesn't initialize the memory.  Returns a kernel virtual address.
//
// If n==0, returns the address of the next free page without allocating
// anything.
//
// If we're out of memory, boot_alloc should panic.
// This function may ONLY be used during initialization,
// before the page_free_list list has been set up.
static void *
boot_alloc (uint32_t n)
{
    static char *nextfree;      // virtual address of next byte of free memory
    char *result;
    size_t amount_pages = 0;


    // Initialize nextfree if this is the first time.
    // 'end' is a magic symbol automatically generated by the linker,
    // which points to the end of the kernel's bss segment:
    // the first virtual address that the linker did *not* assign
    // to any kernel code or global variables.

    /*
     *Hawx: .bss is the section to store the uninitialized global data.
     *Use the 
     */
    if (!nextfree)
    {
        nAvailPages = npages;
        extern char end[];
        nextfree = ROUNDUP ((char *) end, PGSIZE);
    }

    // Allocate a chunk large enough to hold 'n' bytes, then update
    // nextfree.  Make sure nextfree is kept aligned
    // to a multiple of PGSIZE.
    // LAB 2: Your code here.

    /*Hawx: Calculate how many pages needed.
     *      Check wheather system has enougn free pages.
     */
    amount_pages = n / PGSIZE;
    if (n % PGSIZE)
    {
        amount_pages += 1;
    }
    if (nAvailPages < amount_pages)
    {
        panic ("We don't have any available pages");
    }

    /*
     *Hawx: Set the return pointer.
     *      Sutract the ready used page.
     *      Set the nextfree pointer to the next free page.       
     */
    /*
       if (0 == n)
       {
       result = (char *) ROUNDDOWN ((uint32_t) nextfree + PGSIZE, PGSIZE);
       }
     */
    result = nextfree;
    if (0 != n)
    {
        nAvailPages -= amount_pages;
        nextfree =
            ROUNDUP ((char *) ((uint32_t) nextfree + amount_pages * PGSIZE),
                     PGSIZE);
    }

    cprintf
        ("Require %d bytes, allocate:%d pages, Remainder:%d pages, Total:%d pages, nextfree:0x%x, result:0x%x\n",
         n, amount_pages, nAvailPages, npages, (uint32_t) nextfree,
         (uint32_t) result);
    return (void *) result;
}

// Set up a two-level page table:
//    kern_pgdir is its linear (virtual) address of the root
//
// This function only sets up the kernel part of the address space
// (ie. addresses >= UTOP).  The user part of the address space
// will be setup later.
//
// From UTOP to ULIM, the user is allowed to read but not write.
// Above ULIM the user cannot read or write.
void
mem_init (void)
{
    uint32_t cr0;
    size_t n;

    // Find out how much memory the machine has (npages & npages_basemem).
    i386_detect_memory ();

    // Remove this line when you're ready to test this function.

    //////////////////////////////////////////////////////////////////////
    // create initial page directory.
    kern_pgdir = (pde_t *) boot_alloc (PGSIZE);
    memset (kern_pgdir, 0, PGSIZE);

    //////////////////////////////////////////////////////////////////////
    // Recursively insert PD in itself as a page table, to form
    // a virtual page table at virtual address UVPT.
    // (For now, you don't have understand the greater purpose of the
    // following two lines.)

    // Permissions: kernel R, user R

    /*Hawx:
     * Set PDE's PDX(UVPT) index' entry's value to kern_pgdir's 'physical addr.
     * This PDE manages 4MB memory space
     */
    kern_pgdir[PDX (UVPT)] = PADDR (kern_pgdir) | PTE_U | PTE_P;

    //////////////////////////////////////////////////////////////////////
    // Allocate an array of npages 'struct Page's and store it in 'pages'.
    // The kernel uses this array to keep track of physical pages: for
    // each physical page, there is a corresponding struct Page in this
    // array.  'npages' is the number of physical pages in memory.

    // Your code goes here:
    pages = (struct Page *) boot_alloc (sizeof (struct Page) * npages);


    //////////////////////////////////////////////////////////////////////
    // Now that we've allocated the initial kernel data structures, we set
    // up the list of free physical pages. Once we've done so, all further
    // memory management will go through the page_* functions. In
    // particular, we can now map memory using boot_map_region
    // or page_insert
    page_init ();

    check_page_free_list (1);
    check_page_alloc ();
    check_page ();

    //////////////////////////////////////////////////////////////////////
    // Now we set up virtual memory

    //////////////////////////////////////////////////////////////////////
    // Map 'pages' read-only by the user at linear address UPAGES
    // Permissions:
    //    - the new image at UPAGES -- kernel R, user R
    //      (ie. perm = PTE_U | PTE_P)
    //    - pages itself -- kernel RW, user NONE

    // Your code goes here:
    boot_map_region (kern_pgdir, (uintptr_t) UPAGES,
                     npages * sizeof (struct Page),
                     PADDR (pages), PTE_U | PTE_P);

    //////////////////////////////////////////////////////////////////////
    // Use the physical memory that 'bootstack' refers to as the kernel
    // stack.  The kernel stack grows down from virtual address KSTACKTOP.
    // We consider the entire range from [KSTACKTOP-PTSIZE, KSTACKTOP)
    // to be the kernel stack, but break this into two pieces:
    //     * [KSTACKTOP-KSTKSIZE, KSTACKTOP) -- backed by physical memory
    //     * [KSTACKTOP-PTSIZE, KSTACKTOP-KSTKSIZE) -- not backed; so if
    //       the kernel overflows its stack, it will fault rather than
    //       overwrite memory.  Known as a "guard page".
    //     Permissions: kernel RW, user NONE

    // Your code goes here:
    /* 
       boot_map_region(kern_pgdir
       , (uintptr_t)(KSTACKTOP-PTSIZE)
       , (uint32_t)(PTSIZE - KSTKSIZE)
       , (physaddr_t)bootstacktop
       , PTE_P);
     */

    boot_map_region (kern_pgdir, (uintptr_t) (KSTACKTOP - KSTKSIZE),
                     (uint32_t) (KSTKSIZE), (physaddr_t) PADDR (bootstack),
                     PTE_W);


    //////////////////////////////////////////////////////////////////////
    // Map all of physical memory at KERNBASE.
    // Ie.  the VA range [KERNBASE, 2^32) should map to
    //      the PA range [0, 2^32 - KERNBASE)
    // We might not have 2^32 - KERNBASE bytes of physical memory, but
    // we just set up the mapping anyway.
    // Permissions: kernel RW, user NONE

    // Your code goes here:
    boot_map_region (kern_pgdir, (uintptr_t) KERNBASE,
                     (uint32_t) (0xffffffff - KERNBASE), (physaddr_t) (0),
                     PTE_W);

    // Check that the initial page directory has been set up correctly.
    check_kern_pgdir ();

    // Switch from the minimal entry page directory to the full kern_pgdir
    // page table we just created.  Our instruction pointer should be
    // somewhere between KERNBASE and KERNBASE+4MB right now, which is
    // mapped the same way by both page tables.
    //
    // If the machine reboots at this point, you've probably set up your
    // kern_pgdir wrong.
    lcr3 (PADDR (kern_pgdir));

    check_page_free_list (0);

    // entry.S set the really important flags in cr0 (including enabling
    // paging).  Here we configure the rest of the flags that we care about.
    cr0 = rcr0 ();
    cr0 |= CR0_PE | CR0_PG | CR0_AM | CR0_WP | CR0_NE | CR0_MP;
    cr0 &= ~(CR0_TS | CR0_EM);
    lcr0 (cr0);

    // Some more checks, only possible after kern_pgdir is installed.
    check_page_installed_pgdir ();
}

// --------------------------------------------------------------
// Tracking of physical pages.
// The 'pages' array has one 'struct Page' entry per physical page.
// Pages are reference counted, and free pages are kept on a linked list.
// --------------------------------------------------------------

//
// Initialize page structure and memory free list.
// After this is done, NEVER use boot_alloc again.  ONLY use the page
// allocator functions below to allocate and deallocate physical
// memory via the page_free_list.
//
void
page_init (void)
{
    extern char end[];
    physaddr_t next_boot_free;
    /*
     *Hawx; map it from [0 npages_basemem]
     */
    // The example code here marks all physical pages as free.
    // However this is not truly the case.  What memory is free?
    //  1) Mark physical page 0 as in use.
    //     This way we preserve the real-mode IDT and BIOS structures
    //     in case we ever need them.  (Currently we don't, but...)
    //Hawx: The following labs we will need them.  

    //  2) The rest of base memory, [PGSIZE, npages_basemem * PGSIZE)
    //     is free.
    //  3) Then comes the IO hole [IOPHYSMEM, EXTPHYSMEM), which must
    //     never be allocated.
    //  4) Then extended memory [EXTPHYSMEM, ...).
    //     Some of it is in use, some is free. Where is the kernel
    //     in physical memory?  Which pages are already in use for
    //     page tables and other data structures?
    //Hawx: Notice the allocation of part for IO memory and used memory

    //My Code is at here:
    // Change the code to reflect this.
    // NB: DO NOT actually touch the physical memory corresponding to
    // free pages!

    /*
     *Hawx: Init all pages as to be free.
     */
    page_free_list = &pages[0]; //Page.0 can't be used.
    size_t i;
    for (i = 1; i < npages; i++)
    {
        pages[i - 1].pp_ref = 0;
        pages[i - 1].pp_link = &pages[i];
        //Hawx: paddr is the memory mapped & managed by this page.
        pages[i - 1].paddr = page2pa (&pages[i - 1]);
    }
    pages[npages - 1].pp_ref = 0;
    pages[npages - 1].pp_link = NIL;
    pages[npages - 1].paddr = page2pa (&pages[npages - 1]);

    tail_free_page = &pages[npages - 1];

    //Set specific used page
    // Set Page.0; -for kernel usage.
    pages[0].pp_ref = 1;
    pages[0].pp_link = NIL;     //head used
    page_free_list = &pages[1];

    next_boot_free = PADDR (boot_alloc (0));
    //Set IO hole: [IOPHYSMEM, EXTPHYSMEM) - for IDT usage HW? Kernel?
    //set other used page for kernel
    //JOS kernel used memory: [EXTPHYSMEM, next_boot_free)
    //data structure/image...etc.
    for (i = PGNUM (EXTPHYSMEM); i < PGNUM (next_boot_free); i++)
    {
        pages[i].pp_ref = 1;
    }
    pages[PGNUM (IOPHYSMEM) - 1].pp_link = &pages[PGNUM (next_boot_free)];
    pages[PGNUM (next_boot_free) - 1].pp_link = NIL;
    pages[0].pp_link = &pages[PGNUM (IOPHYSMEM)];   //link to head-page.0


    //Set nAvailPages
    nAvailPages = nAvailPages - ((1 /*Page.0 */ ) +
                                 (PGNUM (EXTPHYSMEM) -
                                  PGNUM (IOPHYSMEM) /*IO Hole */ ));
}

//
// Allocates a physical page.  If (alloc_flags & ALLOC_ZERO), fills the entire
// returned physical page with '\0' bytes.  Does NOT increment the reference
// count of the page - the caller must do these if necessary (either explicitly
// or via page_insert).
//
// Returns NULL if out of free memory.
//
// Hint: use page2kva and memset
struct Page *
page_alloc (int alloc_flags)
{
    struct Page *ret_page = NIL;

    if (NIL == page_free_list)
    {
        return NIL;
    }
    ret_page = page_free_list;
    page_free_list = ret_page->pp_link;
    ret_page->pp_link = NIL;
    /*
     *Hawx: Increment of Referce Count is not page_alloc's job 
     *ret_page->pp_ref = 1;
     */

    if (ALLOC_ZERO & alloc_flags)
    {
        //The page is already mapped by MMU.
        //It has the same result if here doesn't use KADDR(). But it needs to set following lines.
        // Map VA's [0, 4MB) to PA's [0, 4MB)
        //[0] = ((uintptr_t) entry_pgtable - KERNBASE) + PTE_P + PTE_W,
        memset ((void *) KADDR (ret_page->paddr), '\0', PGSIZE);
        //memset ((void *)  (ret_page->paddr), '\0', PGSIZE);
    }
    ret_page->pp_ref = 0;
    return ret_page;
}

//
// Return a page to the free list.
// (This function should only be called when pp->pp_ref reaches 0.)
//
void
page_free (struct Page *pp)
{
    if (NIL == pp)
        return;

    if (PGNUM (pp->paddr) >= npages)
    {
        return;
    }
    pp->pp_link = page_free_list;
    page_free_list = pp;
}

//
// Decrement the reference count on a page,
// freeing it if there are no more refs.
//
void
page_decref (struct Page *pp)
{
    if (--pp->pp_ref == 0)
        page_free (pp);
}

// Given 'pgdir', a pointer to a page directory, pgdir_walk returns
// a pointer to the page table entry (PTE) for linear address 'va'.
// This requires walking the two-level page table structure.
//
// The relevant page table page might not exist yet.
// If this is true, and create == false, then pgdir_walk returns NULL.
// Otherwise, pgdir_walk allocates a new page table page with page_alloc.
//    - If the allocation fails, pgdir_walk returns NULL.
//    - Otherwise, the new page's reference count is incremented,
//  the page is cleared,
//  and pgdir_walk returns a pointer into the new page table page.
//
// Hint 1: you can turn a Page * into the physical address of the
// page it refers to with page2pa() from kern/pmap.h.
//
// Hint 2: the x86 MMU checks permission bits in both the page directory
// and the page table, so it's safe to leave permissions in the page
// more permissive than strictly necessary.
//
// Hint 3: look at inc/mmu.h for useful macros that mainipulate page
// table and page directory entries.
//
pte_t *
pgdir_walk (pde_t * pgdir, const void *va, int create)
{

    physaddr_t pa = 0;
    struct Page *pde_pg;
    do
    {
        if (PTE_P & pgdir[PDX (va)])
        {
            pde_pg = pa2page (pgdir[PDX (va)]);
        }
        else
        {
            //PDE doesn't exist.
            if (NO_CREATE == create)
            {
                cprintf ("PTE doesn't exist and NO_CREATE\n");
                break;
            }
            if (NIL == (pde_pg = page_alloc (ALLOC_ZERO)))
            {
                cprintf ("PTE doesn't exist and page_alloc failed\n");
                break;
            }
#ifdef __PD_REF__
            INC_PGP (pgdir);
#endif

#ifndef __PT_REF__
            pde_pg->pp_ref++;
#endif
            pgdir[PDX (va)] = pde_pg->paddr | PTE_P | PTE_W;
        }

        //PDE exist now.
        /*Why
           (gdb)
           0xf0100e17 <page_insert+36>:    test   %eax,%eax
           546         if(NIL == ptep)
           (gdb) p ptep
           $1 = (pte_t *) 0x1000
           (gdb) p *ptep
           $2 = <error type>
           (gdb) x/w ptep
           0x1000: 0x00000000
         */
//        return ((( (pde_pg->paddr)) + PTX (va)));
        return ((pte_t *) KADDR (pde_pg->paddr)) + PTX (va);
    }
    while (FALSE);


    return NULL;
}

//
// Map [va, va+size) of virtual address space to physical [pa, pa+size)
// in the page table rooted at pgdir.  Size is a multiple of PGSIZE.
// Use permission bits perm|PTE_P for the entries.
//
// This function is only intended to set up the ``static'' mappings
// above UTOP. As such, it should *not* change the pp_ref field on the
// mapped pages.
//
// Hint: the TA solution uses pgdir_walk
static void
boot_map_region (pde_t * pgdir, uintptr_t va, size_t size, physaddr_t pa,
                 int perm)
{
    uint32_t i;
    pte_t *ptep = NIL;

    size = ROUNDUP (size, PGSIZE);
    for (i = 0; i < size; i += PGSIZE)
    {
        ptep = pgdir_walk (pgdir, (void *) (va + i), CREATE);
        assert (NIL != ptep);
        *ptep = PTE_ADDR (pa + i) | perm | PTE_P;
    }

}

//
// Map the physical page 'pp' at virtual address 'va'.
// The permissions (the low 12 bits) of the page table entry
// should be set to 'perm|PTE_P'.
//
// Requirements
//   - If there is already a page mapped at 'va', it should be page_remove()d.
//   - If necessary, on demand, a page table should be allocated and inserted
//     into 'pgdir'.
//   - pp->pp_ref should be incremented if the insertion succeeds.
//   - The TLB must be invalidated if a page was formerly present at 'va'.
//
// Corner-case hint: Make sure to consider what happens when the same
// pp is re-inserted at the same virtual address in the same pgdir.
// Don't be tempted to write special-case code to handle this
// situation, though; there's an elegant way to address it.
//
// RETURNS:
//   0 on success
//   -E_NO_MEM, if page table couldn't be allocated
//
// Hint: The TA solution is implemented using pgdir_walk, page_remove,
// and page2pa.
//
int
page_insert (pde_t * pgdir, struct Page *pp, void *va, int perm)
{
    //Pointer assign imply KADDR();
    pte_t *ptep = NIL;

    /*Page table
       ----------
       |ptep|pte|
       ----------
     */
    /*
     * *ptep is the memory with permission defined by its permission field.
     * The pointer address return by pgdir_walk is the VA now.
     */
    ptep = pgdir_walk (pgdir, va, CREATE);

    if (NIL == ptep)
    {
        cprintf ("ptep is NULL\n");
        return -E_NO_MEM;
    }

    if (NIL == pp)
    {
        return -E_UNSPECIFIED;
    }

    pgdir[PDX (va)] |= perm;
    //map it!
    if ((pp->paddr != PTE_ADDR (*ptep)))
    {
        //Increase page table's self page count.
#ifdef __PT_REF__
        pa2page (PDE_ADDR (pgdir[PDX (va)]))->pp_ref++;
#endif

        //Increase page's self page count.
        pp->pp_ref++;
        if (*ptep & PTE_P)
        {
            page_remove (pgdir, va);
        }
    }

    *ptep = (page2pa (pp)) | perm | PTE_P;


    return 0;

}

//
// Return the page mapped at virtual address 'va'.
// If pte_store is not zero, then we store in it the address
// of the pte for this page.  This is used by page_remove and
// can be used to verify page permissions for syscall arguments,
// but should not be used by most callers.
//
// Return NULL if there is no page mapped at va.
//
// Hint: the TA solution uses pgdir_walk and pa2page.
//
struct Page *
page_lookup (pde_t * pgdir, void *va, pte_t ** pte_store)
{
    *pte_store = pgdir_walk (pgdir, va, NO_CREATE);


    // Hawx:
    // Coincidence: It could also use the following 2 lines.
    //              Because when page is used, the PTE_P must be set, It will cause pte not to be 0x0.
    // Correct meaning: Unavailable index.

    if (NIL == *pte_store)
        return NIL;

    if (!((**pte_store) & PTE_P))
    {
        cprintf ("The page looked up is not present\n");
        return NIL;
    }

    return pa2page (PTE_ADDR (**pte_store));
}

//
// Unmaps the physical page at virtual address 'va'.
// If there is no physical page at that address, silently does nothing.
//
// Details:
//   - The ref count on the physical page should decrement.
//   - The physical page should be freed if the refcount reaches 0.
//   - The pg table entry corresponding to 'va' should be set to 0.
//     (if such a PTE exists)
//   - The TLB must be invalidated if you remove an entry from
//     the page table.
//
// Hint: The TA solution is implemented using page_lookup,
//  tlb_invalidate, and page_decref.
//
void
page_remove (pde_t * pgdir, void *va)
{
    struct Page *rm_page = NIL;
    pte_t *rm_pte;
    if (NIL == (rm_page = page_lookup (pgdir, va, &rm_pte)))
    {
        return;
    }

    //Decrease Page's self pg_count;
    page_decref (rm_page);
    if (!rm_page->pp_ref)
    {
        tlb_invalidate (pgdir, va);
    }
    *rm_pte = 0;


#ifdef __PT_REF__
    rm_page = pa2page (PDE_ADDR (pgdir[PDX (va)]));
    page_decref (rm_page);
    if (rm_page->pp_ref == 0)
    {
        tlb_invalidate (pgdir, va);
        pgdir[PDX (va)] = 0;
    }
#endif

#ifdef __PD_REF__
    rm_page = GET_PGD_PG (pgdir);
    page_decref (rm_page);
#endif



}

//
// Invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
//
void
tlb_invalidate (pde_t * pgdir, void *va)
{
    // Flush the entry only if we're modifying the current address space.
    // For now, there is only one address space, so always invalidate.
    invlpg (va);
}


// --------------------------------------------------------------
// Checking functions.
// --------------------------------------------------------------

//
// Check that the pages on the page_free_list are reasonable.
//
static void
check_page_free_list (bool only_low_memory)
{
    struct Page *pp;
    int pdx_limit = only_low_memory ? 1 : NPDENTRIES;
    int nfree_basemem = 0,
        nfree_extmem = 0;
    char *first_free_page;

    if (!page_free_list)
        panic ("'page_free_list' is a null pointer!");

    if (only_low_memory)
    {
        // Move pages with lower addresses first in the free
        // list, since entry_pgdir does not map all pages.
        struct Page *pp1,
        *pp2;
        struct Page **tp[2] = { &pp1, &pp2 };
        for (pp = page_free_list; pp; pp = pp->pp_link)
        {
            int pagetype = PDX (page2pa (pp)) >= pdx_limit;
            *tp[pagetype] = pp;
            tp[pagetype] = &pp->pp_link;
        }
        *tp[1] = 0;
        *tp[0] = pp2;
        page_free_list = pp1;
    }

    // if there's a page that shouldn't be on the free list,
    // try to make sure it eventually causes trouble.
    for (pp = page_free_list; pp; pp = pp->pp_link)
        if (PDX (page2pa (pp)) < pdx_limit)
            memset (page2kva (pp), 0x97, 128);

    first_free_page = (char *) boot_alloc (0);
    for (pp = page_free_list; pp; pp = pp->pp_link)
    {
        // check that we didn't corrupt the free list itself
        assert (pp >= pages);
        assert (pp < pages + npages);
        assert (((char *) pp - (char *) pages) % sizeof (*pp) == 0);

        // check a few pages that shouldn't be on the free list
        assert (page2pa (pp) != 0);
        assert (page2pa (pp) != IOPHYSMEM);
        assert (page2pa (pp) != EXTPHYSMEM - PGSIZE);
        assert (page2pa (pp) != EXTPHYSMEM);
        assert (page2pa (pp) < EXTPHYSMEM
                || (char *) page2kva (pp) >= first_free_page);

        if (page2pa (pp) < EXTPHYSMEM)
            ++nfree_basemem;
        else
            ++nfree_extmem;
    }

    assert (nfree_basemem > 0);
    assert (nfree_extmem > 0);
    cprintf ("check_page_free_list() succeeded!\n");
}

//
// Check the physical page allocator (page_alloc(), page_free(),
// and page_init()).
//
static void
check_page_alloc (void)
{
    struct Page *pp,
    *pp0,
    *pp1,
    *pp2;
    int nfree;
    struct Page *fl;
    char *c;
    int i;

    if (!pages)
        panic ("'pages' is a null pointer!");

    // check number of free pages
    for (pp = page_free_list, nfree = 0; pp; pp = pp->pp_link)
        ++nfree;

    // should be able to allocate three pages
    pp0 = pp1 = pp2 = 0;
    assert ((pp0 = page_alloc (0)));
    assert ((pp1 = page_alloc (0)));
    assert ((pp2 = page_alloc (0)));

    assert (pp0);
    assert (pp1 && pp1 != pp0);
    assert (pp2 && pp2 != pp1 && pp2 != pp0);
    assert (page2pa (pp0) < npages * PGSIZE);
    assert (page2pa (pp1) < npages * PGSIZE);
    assert (page2pa (pp2) < npages * PGSIZE);

    // temporarily steal the rest of the free pages
    fl = page_free_list;
    page_free_list = 0;

    // should be no free memory
    assert (!page_alloc (0));

    // free and re-allocate?
    page_free (pp0);
    page_free (pp1);
    page_free (pp2);
    pp0 = pp1 = pp2 = 0;
    assert ((pp0 = page_alloc (0)));
    assert ((pp1 = page_alloc (0)));
    assert ((pp2 = page_alloc (0)));
    assert (pp0);
    assert (pp1 && pp1 != pp0);
    assert (pp2 && pp2 != pp1 && pp2 != pp0);
    assert (!page_alloc (0));

    // test flags
    memset (page2kva (pp0), 1, PGSIZE);
    page_free (pp0);
    assert ((pp = page_alloc (ALLOC_ZERO)));
    assert (pp && pp0 == pp);
    c = page2kva (pp);
    for (i = 0; i < PGSIZE; i++)
        assert (c[i] == 0);

    // give free list back
    page_free_list = fl;

    // free the pages we took
    page_free (pp0);
    page_free (pp1);
    page_free (pp2);

    // number of free pages should be the same
    for (pp = page_free_list; pp; pp = pp->pp_link)
        --nfree;
    assert (nfree == 0);

    cprintf ("check_page_alloc() succeeded!\n");
}

//
// Checks that the kernel part of virtual address space
// has been setup roughly correctly (by mem_init()).
//
// This function doesn't test every corner case,
// but it is a pretty good sanity check.
//

static void
check_kern_pgdir (void)
{
    uint32_t i,
     n;
    pde_t *pgdir;

    pgdir = kern_pgdir;

    // check pages array
    n = ROUNDUP (npages * sizeof (struct Page), PGSIZE);
    for (i = 0; i < n; i += PGSIZE)
        assert (check_va2pa (pgdir, UPAGES + i) == PADDR (pages) + i);


    // check phys mem
    for (i = 0; i < npages * PGSIZE; i += PGSIZE)
        assert (check_va2pa (pgdir, KERNBASE + i) == i);

    // check kernel stack
    for (i = 0; i < KSTKSIZE; i += PGSIZE)
    {
        assert (check_va2pa (pgdir, KSTACKTOP - KSTKSIZE + i) ==
                PADDR (bootstack) + i);
    }
    assert (check_va2pa (pgdir, KSTACKTOP - PTSIZE) == ~0);

    // check PDE permissions
    for (i = 0; i < NPDENTRIES; i++)
    {
        switch (i)
        {
        case PDX (UVPT):
        case PDX (KSTACKTOP - 1):
        case PDX (UPAGES):
            assert (pgdir[i] & PTE_P);
            break;
        default:
            if (i >= PDX (KERNBASE))
            {
                assert (pgdir[i] & PTE_P);
                assert (pgdir[i] & PTE_W);
            }
            else
                assert (pgdir[i] == 0);
            break;
        }
    }
    cprintf ("check_kern_pgdir() succeeded!\n");
}

// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pgdir'.  The hardware normally performs
// this functionality for us!

// We define our own version to help check
// the check_kern_pgdir() function; it shouldn't be used elsewhere.

static physaddr_t
check_va2pa (pde_t * pgdir, uintptr_t va)
{
    pte_t *p;

    //Hawx: Get the PDE.
    //      And Confirm it to be present.
    pgdir = &pgdir[PDX (va)];
    if (!(*pgdir & PTE_P))
        return ~0;

    //Hawx: Get the PTE
    //      And Confirm it to be present.
    p = (pte_t *) KADDR (PTE_ADDR (*pgdir));
    if (!(p[PTX (va)] & PTE_P))
        return ~0;

    //Hawx: Return the page's start address corresponding to va.
    return PTE_ADDR (p[PTX (va)]);
}


// check page_insert, page_remove, &c
static void
check_page (void)
{
    struct Page *pp,
    *pp0,
    *pp1,
    *pp2;
    struct Page *fl;
    pte_t *ptep,
    *ptep1;
    void *va;
    int i;
    extern pde_t entry_pgdir[];

    // should be able to allocate three pages
    pp0 = pp1 = pp2 = 0;
    assert ((pp0 = page_alloc (0)));
    assert ((pp1 = page_alloc (0)));
    assert ((pp2 = page_alloc (0)));

    assert (pp0);
    assert (pp1 && pp1 != pp0);
    assert (pp2 && pp2 != pp1 && pp2 != pp0);

    // temporarily steal the rest of the free pages
    fl = page_free_list;
    page_free_list = 0;

    // should be no free memory
    assert (!page_alloc (0));

    // there is no page allocated at address 0
    assert (page_lookup (kern_pgdir, (void *) 0x0, &ptep) == NULL);

    // there is no free memory, so we can't allocate a page table
    assert (page_insert (kern_pgdir, pp1, 0x0, PTE_W) < 0);

    // free pp0 and try again: pp0 should be used for page table
    //Hawx: map pp1 to 0x0, and use pp0 as the PDE, page table.
    page_free (pp0);            //Hawx: page_free has no effect to substract ref-count.
    assert (page_insert (kern_pgdir, pp1, 0x0, PTE_W) == 0);    //use pp0 as the PDE to map pp1 to pp0(PDE)'s PTE
    assert (PTE_ADDR (kern_pgdir[0]) == page2pa (pp0)); //Hawx: kern_pgdir[0] should use the same phy-addr as pp0.
    assert (check_va2pa (kern_pgdir, 0x0) == page2pa (pp1));    //Hawx: pp1's phy-addr is the same as page for va(0x0)

    //Assign page frame to the right PTE through PDE.
    assert (pp1->pp_ref == 1);
    assert (pp0->pp_ref == 1);

    // should be able to map pp2 at PGSIZE because pp0 is already allocated for page table
    //Hawx pp0 is the PDE, which manage 4MB memory space.
    //Hawx: map pp2 to PGSIZE.
    assert (page_insert (kern_pgdir, pp2, (void *) PGSIZE, PTE_W) == 0);
    assert (check_va2pa (kern_pgdir, PGSIZE) == page2pa (pp2)); //Assign page frame to the right PTE through PDE.
    assert (pp2->pp_ref == 1);

    // should be no free memory
    assert (!page_alloc (0));

    // should be able to map pp2 at PGSIZE because it's already there
    // Hawx: Insert the page already inserted. This count shouldn't be increated.
    assert (page_insert (kern_pgdir, pp2, (void *) PGSIZE, PTE_W) == 0);
    assert (check_va2pa (kern_pgdir, PGSIZE) == page2pa (pp2));
    assert (pp2->pp_ref == 1);

    // pp2 should NOT be on the free list
    // could happen in ref counts are handled sloppily in page_insert
    // Hawx: It doesn't support available pages now.
    assert (!page_alloc (0));

    // check that pgdir_walk returns a pointer to the pte
    //Hawx: Check both ways to access the page phy-addr is the same.
    ptep = (pte_t *) KADDR (PTE_ADDR (kern_pgdir[PDX (PGSIZE)]));   //Hawx: Get the PDE's address, page table's address.
    assert (pgdir_walk (kern_pgdir, (void *) PGSIZE, 0) ==
            ptep + PTX (PGSIZE));

    // should be able to change permissions too.
    //Hawx: It could change permissions for the inserted page by inserting it.
    assert (page_insert (kern_pgdir, pp2, (void *) PGSIZE, PTE_W | PTE_U) ==
            0);
    assert (check_va2pa (kern_pgdir, PGSIZE) == page2pa (pp2));
    assert (pp2->pp_ref == 1);
    assert (*pgdir_walk (kern_pgdir, (void *) PGSIZE, 0) & PTE_U);
    //Hawx: work at here.1
    assert (kern_pgdir[0] & PTE_U);

    // should not be able to map at PTSIZE because need free page for page table
    // Hawx: pp0 is the PDE, page table, currently.
    //       How did page_insert prevent to re-allocate pp0...?
    assert (page_insert (kern_pgdir, pp0, (void *) PTSIZE, PTE_W) < 0);

    // insert pp1 at PGSIZE (replacing pp2)
    // Hawx: map pp1 to PGSIZE, and replace pp2.
    //       pp2 is current freed.
    assert (page_insert (kern_pgdir, pp1, (void *) PGSIZE, PTE_W) == 0);
    assert (!(*pgdir_walk (kern_pgdir, (void *) PGSIZE, 0) & PTE_U));

    // should have pp1 at both 0 and PGSIZE, pp2 nowhere, ...
    //Hawx: Check current map:
    //                        pp1 to 0x0
    //                        pp1 to PGSIZE
    assert (check_va2pa (kern_pgdir, 0) == page2pa (pp1));
    assert (check_va2pa (kern_pgdir, PGSIZE) == page2pa (pp1));
    // ... and ref counts should reflect this
    assert (pp1->pp_ref == 2);
    assert (pp2->pp_ref == 0);

    // pp2 should be returned by page_alloc
    // Hawx: pp2 is just freed, so when we use allocation, then we get the pp2-phy-addr. 
    assert ((pp = page_alloc (0)) && pp == pp2);

    // unmapping pp1 at 0 should keep pp1 at PGSIZE
    // Hawx: Free pp1 at 0x0. 
    //       Current map: pp1 at PGSIZE.
    page_remove (kern_pgdir, 0x0);
    //Hawx: work at here.2
    assert (check_va2pa (kern_pgdir, 0x0) == ~0);
    assert (check_va2pa (kern_pgdir, PGSIZE) == page2pa (pp1));
    assert (pp1->pp_ref == 1);
    assert (pp2->pp_ref == 0);

    // unmapping pp1 at PGSIZE should free it
    // Hawx: Free pp1 at PGSIZE. 
    //       Current map: NONE.
    page_remove (kern_pgdir, (void *) PGSIZE);
    assert (check_va2pa (kern_pgdir, 0x0) == ~0);
    assert (check_va2pa (kern_pgdir, PGSIZE) == ~0);
    assert (pp1->pp_ref == 0);
    assert (pp2->pp_ref == 0);

#ifndef __ALL_COUNT__
    //Page Table has no other entry, So I freed page table page already.
    //Current Available Pages : pp1 page
    //                          pp0.page table page(lastly free)
    // so it should be returned by page_alloc
    // Hawx: pp1 is just freed, so when we use allocation, then we get the pp1-phy-addr. 
    assert ((pp = page_alloc (0)) && pp == pp1);    //Here is pp <-- pp0

    // should be no free memory
    assert (!page_alloc (0));
#endif

    // forcibly take pp0 back
#ifdef __ALL_COUNT__
    //PTE_ADDR (kern_pgdir[0]) is already ZERO.
    //assert (PTE_ADDR (kern_pgdir[0]) == page2pa (pp0));
    //kern_pgdir[0] = 0;
    //Page Table has no other entry, So I freed page table page already.
    assert (pp0->pp_ref == 0);
#else
    assert (PTE_ADDR (kern_pgdir[0]) == page2pa (pp0));
    kern_pgdir[0] = 0;
    assert (pp0->pp_ref == 1);
#endif

    pp0->pp_ref = 0;

    // check pointer arithmetic in pgdir_walk
    page_free (pp0);
    //Hawx: va <- 4MB+4KB
    va = (void *) (PGSIZE * NPDENTRIES + PGSIZE);
    ptep = pgdir_walk (kern_pgdir, va, 1);
    ptep1 = (pte_t *) KADDR (PTE_ADDR (kern_pgdir[PDX (va)]));
    assert (ptep == ptep1 + PTX (va));
    kern_pgdir[PDX (va)] = 0;
    pp0->pp_ref = 0;

    // check that new page tables get cleared
    memset (page2kva (pp0), 0xFF, PGSIZE);
    page_free (pp0);
    pgdir_walk (kern_pgdir, 0x0, 1);
    ptep = (pte_t *) page2kva (pp0);
    for (i = 0; i < NPTENTRIES; i++)
        assert ((ptep[i] & PTE_P) == 0);
    kern_pgdir[0] = 0;
    pp0->pp_ref = 0;

    // give free list back
    page_free_list = fl;

    // free the pages we took
    page_free (pp0);
    page_free (pp1);
    page_free (pp2);

    cprintf ("check_page() succeeded!\n");
}

// check page_insert, page_remove, &c, with an installed kern_pgdir
static void
check_page_installed_pgdir (void)
{
    struct Page *pp, *pp0, *pp1, *pp2;
    struct Page *fl; pte_t *ptep, *ptep1;
    uintptr_t va;
    int i;

    // check that we can read and write installed pages
    pp1 = pp2 = 0;
    assert ((pp0 = page_alloc (0)));
    assert ((pp1 = page_alloc (0)));
    assert ((pp2 = page_alloc (0)));
    page_free (pp0);
    memset (page2kva (pp1), 1, PGSIZE);
    memset (page2kva (pp2), 2, PGSIZE);
    page_insert (kern_pgdir, pp1, (void *) PGSIZE, PTE_W);
    assert (pp1->pp_ref == 1);
    assert (*(uint32_t *) PGSIZE == 0x01010101U);
    page_insert (kern_pgdir, pp2, (void *) PGSIZE, PTE_W);
    assert (*(uint32_t *) PGSIZE == 0x02020202U);
    assert (pp2->pp_ref == 1);
    assert (pp1->pp_ref == 0);
    *(uint32_t *) PGSIZE = 0x03030303U;
    assert (*(uint32_t *) page2kva (pp2) == 0x03030303U);
    page_remove (kern_pgdir, (void *) PGSIZE);
    assert (pp2->pp_ref == 0);

    // forcibly take pp0 back
    assert (PTE_ADDR (kern_pgdir[0]) == page2pa (pp0));
    kern_pgdir[0] = 0;
    assert (pp0->pp_ref == 1);
    pp0->pp_ref = 0;

    // free the pages we took
    page_free (pp0);

    cprintf ("check_page_installed_pgdir() succeeded!\n");
}
