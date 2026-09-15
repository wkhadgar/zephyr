/*
 * Copyright (c) 2026 Paulo Santos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RP2350 multicore support
 *
 * Supplies the two SoC-specific pieces Cortex-M SMP needs: the CPU
 * identifier, which the architecture does not provide, and a way to
 * physically start the second core.
 *
 * Register names and the launch sequence come from the HAL rather than from
 * the datasheet by hand: sio_hw and its fifo_st, fifo_wr, fifo_rd and cpuid
 * fields from hardware/structs/sio.h, the RDY and VLD bits from
 * hardware/regs/sio.h, and the sequence itself from
 * multicore_launch_core1_raw() in pico-sdk.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <cmsis_core.h>
#include <hardware/regs/intctrl.h>
#include <hardware/structs/sio.h>

/*
 * The handshake completes in microseconds. Bounded so a core that never
 * answers reports a failure instead of hanging the primary during boot.
 */
#define FIFO_SPIN_LIMIT 1000000U
#define RESTART_LIMIT   100U

uint32_t z_soc_cpu_id(void)
{
	return sio_hw->cpuid;
}

#if defined(CONFIG_SCHED_IPI_SUPPORTED)

/*
 * Doorbell bit reserved for the scheduler IPI. doorbell_out_set raises the
 * matching bit in the other core's doorbell_in, which raises SIO_IRQ_BELL
 * there. With two cores there is only ever one target, so the bitmap only has
 * to be tested for emptiness.
 */
#define IPI_DOORBELL BIT(0)

void soc_sched_ipi(uint32_t cpu_bitmap)
{
	if (cpu_bitmap == 0U) {
		return;
	}

	sio_hw->doorbell_out_set = IPI_DOORBELL;
}

unsigned int soc_sched_ipi_irq(void)
{
	return SIO_IRQ_BELL;
}

void z_arm_cortex_m_sched_ipi(void);

static void rp2350_sched_ipi_isr(const void *arg)
{
	ARG_UNUSED(arg);

	/* Acknowledge before telling the scheduler, so a ring that arrives
	 * while we are in here is not lost.
	 */
	sio_hw->doorbell_in_clr = IPI_DOORBELL;

	z_arm_cortex_m_sched_ipi();
}

void soc_sched_ipi_connect(void)
{
	/*
	 * Priority 0 here is the most urgent Zephyr-managed level, which is
	 * what the scheduler IPI wants: it has to be able to preempt threads
	 * promptly, and it does almost no work.
	 */
	IRQ_CONNECT(SIO_IRQ_BELL, 0, rp2350_sched_ipi_isr, NULL, 0);
}

#endif /* CONFIG_SCHED_IPI_SUPPORTED */

#if defined(CONFIG_SMP)

static bool fifo_push(uint32_t value)
{
	uint32_t spins = FIFO_SPIN_LIMIT;

	while ((sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS) == 0U) {
		spins--;
		if (spins == 0U) {
			return false;
		}
	}

	sio_hw->fifo_wr = value;
	__SEV();

	return true;
}

/*
 * Spins rather than using WFE as pico-sdk does: a WFE with no event left to
 * wake it never returns, which would defeat the bound above.
 */
static bool fifo_pop(uint32_t *value)
{
	uint32_t spins = FIFO_SPIN_LIMIT;

	while ((sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) == 0U) {
		spins--;
		if (spins == 0U) {
			return false;
		}
	}

	*value = sio_hw->fifo_rd;

	return true;
}

static void fifo_drain(void)
{
	while ((sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) != 0U) {
		(void)sio_hw->fifo_rd;
	}
}

int soc_start_secondary_cpu(int cpu_num, void (*entry)(void), void *stack_top, uint32_t vtor)
{
	/* The sequence the bootrom expects: two zeroes, a one, then the
	 * vector table, stack pointer and entry point.
	 */
	const uint32_t seq[] = {
		0U, 0U, 1U, vtor, (uint32_t)stack_top, (uint32_t)entry,
	};
	unsigned int i = 0U;
	unsigned int restarts = 0U;

	if (cpu_num != 1) {
		return -EINVAL;
	}

	do {
		uint32_t cmd = seq[i];
		uint32_t response;

		/* Always drain before sending a zero, and kick the other core
		 * in case it is parked in WFE waiting for FIFO space.
		 */
		if (cmd == 0U) {
			fifo_drain();
			__SEV();
		}

		if (!fifo_push(cmd) || !fifo_pop(&response)) {
			return -ETIMEDOUT;
		}

		/* Any mismatch means the other core is out of step. */
		if (response == cmd) {
			i++;
		} else {
			restarts++;
			i = 0U;
		}

		if (restarts > RESTART_LIMIT) {
			return -EIO;
		}
	} while (i < ARRAY_SIZE(seq));

	return 0;
}

#endif /* CONFIG_SMP */
