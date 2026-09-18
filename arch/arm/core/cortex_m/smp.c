/*
 * Copyright (c) 2026 Paulo Santos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Cortex-M SMP support
 *
 * Cortex-M differs from Zephyr's other SMP targets in ways that shape this
 * file. There is no distributed interrupt controller: each core has its own
 * NVIC, SysTick and MPU, and any inter-processor interrupt comes from
 * SoC-specific hardware. There is also no architectural per-CPU register, so
 * arch_curr_cpu() goes through z_soc_cpu_id(), which the SoC provides.
 *
 * Starting a core is likewise SoC business, so this file owns the generic
 * part, the per-core initialisation a secondary must perform before it can
 * run kernel code, and delegates the physical start to
 * soc_start_secondary_cpu().
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/kernel/thread_stack.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/barrier.h>
#include <cmsis_core.h>

#include <kernel_internal.h>
#include <cortex_m/exception.h>
#include <ipi.h>

K_KERNEL_STACK_ARRAY_DECLARE(z_interrupt_stacks, CONFIG_MP_MAX_NUM_CPUS, CONFIG_ISR_STACK_SIZE);

/**
 * @brief Physically start a secondary core. Implemented by the SoC.
 *
 * @param cpu_num   Zephyr CPU number being started.
 * @param entry     Where the core begins executing.
 * @param stack_top Initial stack pointer for it.
 * @param vtor      Vector table it should use.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int soc_start_secondary_cpu(int cpu_num, void (*entry)(void), void *stack_top, uint32_t vtor);

#if defined(CONFIG_SCHED_IPI_SUPPORTED)
/**
 * @brief Interrupt the given CPUs. Implemented by the SoC.
 *
 * Cortex-M has no architectural inter-processor interrupt, so the mechanism is
 * SoC-specific; on RP2350 it is a doorbell.
 *
 * @param cpu_bitmap CPUs to interrupt, by Zephyr CPU number.
 */
void soc_sched_ipi(uint32_t cpu_bitmap);

/**
 * @brief Connect the SoC's IPI interrupt to its own handler.
 *
 * Left to the SoC so that it can use IRQ_CONNECT() with its own interrupt
 * number, rather than forcing CONFIG_DYNAMIC_INTERRUPTS on every user of this
 * port just to install one handler. That handler acknowledges the interrupt
 * and calls z_arm_cortex_m_sched_ipi().
 */
void soc_sched_ipi_connect(void);

/** @brief IRQ number the SoC raises for an IPI. */
unsigned int soc_sched_ipi_irq(void);

/**
 * @brief Tell the scheduler an IPI arrived. Called by the SoC's IPI handler.
 */
/** @brief Secondary CPU entry in reset.S; moves this core onto PSP. */
void z_arm_secondary_reset(void);

void z_arm_cortex_m_sched_ipi(void)
{
	z_sched_ipi();
}
#endif /* CONFIG_SCHED_IPI_SUPPORTED */

/*
 * Handed to the secondary by arch_cpu_start(). Only one CPU is started at a
 * time, and the kernel waits for it to come up before starting another, so a
 * single slot is enough.
 */
static struct {
	arch_cpustart_t fn;
	void *arg;
	int cpu_num;
} cpu_start;

/*
 * The per-core half of arch_kernel_init(). That function cannot simply be
 * reused here: z_arm_interrupt_stack_setup() takes z_interrupt_stacks[0]
 * unconditionally, so it would point this core's MSP at the primary's
 * interrupt stack. The stack is instead set by the SoC when it starts the
 * core, from the one the kernel supplied.
 *
 * Everything else below is genuinely per core, because the SCB, NVIC, MPU and
 * fault registers are all banked.
 */
static void secondary_core_init(void)
{
	/*
	 * This CPU's switch state, including where its entry frame keeps the
	 * saved LR. Without it the CPU would use CPU 0's, and write fixup
	 * addresses into the primary's interrupt stack.
	 */
	arm_m_percpu_init(arch_curr_cpu()->id);

	z_arm_exc_setup();
	z_arm_fault_init();
	z_arm_cpu_idle_init();
	z_arm_clear_faults();


#if defined(CONFIG_ARM_MPU)
	/*
	 * The MPU is per core, so the primary's configuration does not apply
	 * here. Shared kernel data also has to be mapped Shareable for the
	 * exclusives that k_spinlock is built on to work across cores, which
	 * is the SoC's static region table's job.
	 */
	z_arm_mpu_init();
	z_arm_configure_static_mpu_regions();
#endif

	soc_per_core_init_hook();

#if defined(CONFIG_SCHED_IPI_SUPPORTED)
	/*
	 * The NVIC is per core, so each CPU has to enable the IPI itself. The
	 * handler is already in the shared vector and software ISR tables,
	 * connected once on the primary in arch_smp_init().
	 *
	 * The priority register is per core as well, and matters as much as
	 * the enable. Left at its reset value of zero the IPI sits at the
	 * level reserved for faults and zero-latency interrupts, which BASEPRI
	 * does not mask, so it would arrive while this CPU was still coming up
	 * and had no current thread to switch away from.
	 */
	soc_sched_ipi_connect();
	irq_enable(soc_sched_ipi_irq());
#endif
}

/**
 * @brief Where a secondary core begins executing kernel code.
 *
 * Runs on the secondary with the stack the kernel allocated for it, in Thread
 * mode on MSP, before any thread exists on this CPU.
 */
/*
 * Entered from z_arm_secondary_reset in reset.S, already running on PSP with
 * MSP left as this CPU's interrupt stack. Named as the other architectures
 * name theirs; riscv, arc, arm64 and Cortex-A/R all reach C here.
 */
void arch_secondary_cpu_init(void)
{
	arch_cpustart_t fn = cpu_start.fn;
	void *arg = cpu_start.arg;

	/*
	 * The kernel expects a secondary to arrive with interrupts locked and
	 * unlocks them itself, in z_swap_unlocked() at the end of
	 * smp_init_top(). Cortex-A and Cortex-R get that for free, because
	 * CPSR.I is set coming out of reset; on Cortex-M PRIMASK is clear, so
	 * interrupts are enabled and it has to be done explicitly.
	 *
	 * Without this, z_dummy_thread_init() trips the
	 * "!z_smp_cpu_mobile()" assertion in z_current_thread_set(), because
	 * that predicate is exactly "not in an ISR and interrupts enabled".
	 */
	(void)arch_irq_lock();

	secondary_core_init();

	/*
	 * Hands over to smp_init_top(), which marks this CPU up, waits for the
	 * go-ahead, initialises its dummy thread and then schedules. It does
	 * not return.
	 */
	fn(arg);

	CODE_UNREACHABLE;
}

void arch_cpu_start(int cpu_num, k_thread_stack_t *stack, int sz, arch_cpustart_t fn, void *arg)
{
	int ret;

	cpu_start.fn = fn;
	cpu_start.arg = arg;
	cpu_start.cpu_num = cpu_num;

	/* The secondary reads these as soon as it starts. */
	barrier_dsync_fence_full();

	ret = soc_start_secondary_cpu(cpu_num, z_arm_secondary_reset,
				      K_KERNEL_STACK_BUFFER(stack) + sz, SCB->VTOR);
	if (ret != 0) {
		printk("CPU %d failed to start: %d\n", cpu_num, ret);
	}
}

#if defined(CONFIG_SCHED_IPI_SUPPORTED)
void arch_sched_broadcast_ipi(void)
{
	/* Every CPU but this one. */
	soc_sched_ipi(~BIT(_current_cpu->id));
}

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	soc_sched_ipi(cpu_bitmap & ~BIT(_current_cpu->id));
}
#endif /* CONFIG_SCHED_IPI_SUPPORTED */

int arch_smp_init(void)
{
#if defined(CONFIG_SCHED_IPI_SUPPORTED)
	/*
	 * Connected once, into tables both CPUs share. Enabling is per core:
	 * here for the primary, and in secondary_core_init() for the others.
	 */
	soc_sched_ipi_connect();
	irq_enable(soc_sched_ipi_irq());
#endif

	return 0;
}
