#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>

extern uint32_t vects[];
static struct Taskstate ts;

/* For debugging, so print_trapframe can distinguish between printing
 * a saved trapframe and printing the current trapframe and print some
 * additional information in the latter case.
 */
static struct Trapframe *last_tf;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[IDT_ENTRIES] = { {0} };

struct Pseudodesc idt_pd = {
    sizeof (idt) - 1, (uint32_t) idt
};


static const char *
trapname (int trapno)
{
    static const char *const excnames[] = {
        "Divide error",
        "Debug",
        "Non-Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "BOUND Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack Fault",
        "General Protection",
        "Page Fault",
        "(unknown trap)",
        "x87 FPU Floating-Point Error",
        "Alignment Check",
        "Machine-Check",
        "SIMD Floating-Point Exception"
    };

    if (trapno < sizeof (excnames) / sizeof (excnames[0]))
        return excnames[trapno];
    if (trapno == T_SYSCALL)
        return "System call";
    return "(unknown trap)";
}


void
trap_init (void)
{
    extern struct Segdesc gdt[];
    uint32_t i;
    // LAB 3: Your code here.
    for (i = 0; i < IDT_ENTRIES; i++)
    {
        SETGATE (idt[i], TRAP_N, GD_KT, vects[i], DPL_KERN);
    }

    SETGATE (idt[T_SYSCALL], TRAP_Y, GD_KT, vects[T_SYSCALL], DPL_USER);

    SETGATE (idt[T_BRKPT], TRAP_Y, GD_KT, vects[T_BRKPT], DPL_USER);



    // Per-CPU setup 
    trap_init_percpu ();
}

// Initialize and load the per-CPU TSS and IDT
void
trap_init_percpu (void)
{
    // Setup a TSS so that we get the right stack
    // when we trap to the kernel.
    ts.ts_esp0 = KSTACKTOP;
    ts.ts_ss0 = GD_KD;

    // Initialize the TSS slot of the gdt.
    gdt[GD_TSS0 >> 3] = SEG16 (STS_T32A, (uint32_t) (&ts),
                               sizeof (struct Taskstate), 0);
    gdt[GD_TSS0 >> 3].sd_s = 0;

    // Load the TSS selector (like other segment selectors, the
    // bottom three bits are special; we leave them 0)
    ltr (GD_TSS0);

    // Load the IDT
    lidt (&idt_pd);
}

void
print_trapframe (struct Trapframe *tf)
{
    cprintf ("TRAP frame at %p\n", tf);
    print_regs (&tf->tf_regs);
    cprintf ("  es   0x----%04x\n", tf->tf_es);
    cprintf ("  ds   0x----%04x\n", tf->tf_ds);
    cprintf ("  trap 0x%08x %s\n", tf->tf_trapno, trapname (tf->tf_trapno));
    // If this trap was a page fault that just happened
    // (so %cr2 is meaningful), print the faulting linear address.
    if (tf == last_tf && tf->tf_trapno == T_PGFLT)
        cprintf ("  cr2  0x%08x\n", rcr2 ());
    cprintf ("  err  0x%08x", tf->tf_err);
    // For page faults, print decoded fault error code:
    // U/K=fault occurred in user/kernel mode
    // W/R=a write/read caused the fault
    // PR=a protection violation caused the fault (NP=page not present).
    if (tf->tf_trapno == T_PGFLT)
        cprintf (" [%s, %s, %s]\n",
                 tf->tf_err & 4 ? "user" : "kernel",
                 tf->tf_err & 2 ? "write" : "read",
                 tf->tf_err & 1 ? "protection" : "not-present");
    else
        cprintf ("\n");
    cprintf ("  eip  0x%08x\n", tf->tf_eip);
    cprintf ("  cs   0x----%04x\n", tf->tf_cs);
    cprintf ("  flag 0x%08x\n", tf->tf_eflags);
    if ((tf->tf_cs & 3) != 0)
    {
        cprintf ("  esp  0x%08x\n", tf->tf_esp);
        cprintf ("  ss   0x----%04x\n", tf->tf_ss);
    }
}

void
print_regs (struct PushRegs *regs)
{
    cprintf ("  edi  0x%08x\n", regs->reg_edi);
    cprintf ("  esi  0x%08x\n", regs->reg_esi);
    cprintf ("  ebp  0x%08x\n", regs->reg_ebp);
    cprintf ("  oesp 0x%08x\n", regs->reg_oesp);
    cprintf ("  ebx  0x%08x\n", regs->reg_ebx);
    cprintf ("  edx  0x%08x\n", regs->reg_edx);
    cprintf ("  ecx  0x%08x\n", regs->reg_ecx);
    cprintf ("  eax  0x%08x\n", regs->reg_eax);
}

#define XARG_SYSCALL_PRAR(_regPara) tf->tf_regs._regPara
static void
trap_dispatch (struct Trapframe *tf)
{
    // Handle processor exceptions.
    // LAB 3: Your code here.
    switch (tf->tf_trapno)
    {
    case T_PGFLT:
        page_fault_handler (tf);
        break;
    case T_BRKPT:
        breakpoint_handler (tf);
        break;
    case T_SYSCALL:
        //Extract the parameters
        XARG_SYSCALL_PRAR (reg_eax) = syscall (XARG_SYSCALL_PRAR (reg_eax),
                                               XARG_SYSCALL_PRAR (reg_edx),
                                               XARG_SYSCALL_PRAR (reg_ecx),
                                               XARG_SYSCALL_PRAR (reg_ebx),
                                               XARG_SYSCALL_PRAR (reg_edi),
                                               XARG_SYSCALL_PRAR (reg_esi));
        break;
    default:
        // Unexpected trap: The user process or the kernel has a bug.
        print_trapframe (tf);
        if (tf->tf_cs == GD_KT)
            panic ("unhandled trap in kernel");
        else
        {
            env_destroy (curenv);
            return;
        }
        break;
    }

}

void
trap (struct Trapframe *tf)
{
    // The environment may have set DF and some versions
    // of GCC rely on DF being clear
    asm volatile ("cld":::"cc");

    // Check that interrupts are disabled.  If this assertion
    // fails, DO NOT be tempted to fix it by inserting a "cli" in
    // the interrupt path.
    assert (!(read_eflags () & FL_IF));

    cprintf ("Incoming TRAP frame at %p\n", tf);

    if ((tf->tf_cs & 3) == 3)
    {
        // Trapped from user mode.
        // Copy trap frame (which is currently on the stack)
        // into 'curenv->env_tf', so that running the environment
        // will restart at the trap point.
        assert (curenv);
        curenv->env_tf = *tf;
        // The trapframe on the stack should be ignored from here on.
        tf = &curenv->env_tf;
    }

    // Record that tf is the last real trapframe so
    // print_trapframe can print some additional information.
    last_tf = tf;

    // Dispatch based on what type of trap occurred
    trap_dispatch (tf);

    // Return to the current environment, which should be running.
    assert (curenv && curenv->env_status == ENV_RUNNING);
    env_run (curenv);
}

void
breakpoint_handler (struct Trapframe *tf)
{
    monitor (tf);
    return;
}

void
page_fault_handler (struct Trapframe *tf)
{
    uint32_t fault_va;
    pte_t *ptep;

    // Read processor's CR2 register to find the faulting address
    fault_va = rcr2 ();
    /*
       cr2 is to find the faulting address
       error code has some useful information
     */

    // Handle kernel-mode page faults.
    if ((SEL_PL (tf->tf_cs)) == 0x00)
    {
        print_trapframe (tf);
        panic ("=== Page fault at kernel ===");
    }
    ptep = pgdir_walk (curenv->env_pgdir, (void *) fault_va, NO_CREATE);
    // LAB 3: Your code here.

    /*
       if(0xeec00048 == fault_va)
       {
       cprintf ("*ptep:0x%x\n",*ptep);
       }
     */
    if (PTE_P & *ptep)
    {
        do
        {
            //user mode can do?
            if (!(PTE_U & *ptep))
            {
                break;
            }

            //Writable?
            if (!(PTE_W & *ptep))
            {
                break;
            }
        }
        while (0);
    }
    else
    {
        //Allocate the new one?
        //if it wants to do allocation,
        //then it must check the slot matched perm or not?
        //return;
    }

    // We've already handled kernel-mode exceptions, so if we get here,
    // the page fault happened in user mode.

    // Destroy the environment that caused the fault.
    cprintf ("[%08x] user fault va %08x ip %08x\n",
             curenv->env_id, fault_va, tf->tf_eip);
    cprintf ("[curenv_addr:%08x] env_id_addr: 0x%x,ip %08x\n",
             curenv, (uint32_t) & curenv->env_id, tf->tf_eip);
    print_trapframe (tf);
    env_destroy (curenv);
}