// This file is released to the public domain.
// I doubt that that there is anything copyrightable here anyway...
#ifndef _VTTY_H
#define _VTTY_H

/**
 * TAG_UART_RX marks packets from kernel to master, containing RX data,
 * packet length will be at least 2 (packet tag plus one byte), but
 * can be much longer (upper bound tbd.) containing more RX data.
 */
#define TAG_UART_RX 	   (0)

/**
 * TAG_SET_TERMIOS marks packets from kernel to master, containing
 * a second struct termios2 containing the previous state from the kernel.
 */
#define TAG_SET_TERMIOS	 (1)

/**
 * TAG_SET_MODEM marks packets from kernel to master, containing
 * a unsigned long word containing the kernels idea of the modem state
 * bits.
 */
#define TAG_SET_MODEM 	 (2)

/**
 * TAG_BREAK_CTL marks packets from kernel to master, containing
 * a int word containing the kernels request for a break state change.
 *
 * The possible values are:
 * * negative value: start break condition forever (kernel will end it by 0)
 * * 0: end break condition
 * * positive value: start break condition and end it after the
 * given time, time unit is milliseconds
 *
 * @note Depending on the modules load time configuration of timed_break
 * (see modules load time parameters), this word containes a timed
 * break request from the kernel or just a state change by call to the
 * tty operation break_ctl().
 * ref.: https://docs.kernel.org/driver-api/tty/tty_driver.html#tty-operations-reference
 *
 * @note The difference of the behavior is achived at the time of master device registration
 * during module load, setting or clearing the bit TTY_DRIVER_HARDWARE_BREAK.
 * This bit controlled by the module load parameter timed_break.
 * ref.: https://docs.kernel.org/driver-api/tty/tty_driver.html#tty-driver-flags
 */
#define TAG_BREAK_CTL 	 (3)

/**
 * TAG_FLUSH_BUFFER marks packets from kernel to master, requesting
 * discard of private output buffers, by call to the tty operation flush_buffer().
 *
 * Data is a dummy int.
 *
 * Ref.: https://docs.kernel.org/driver-api/tty/tty_driver.html#tty-operations-reference
 */
#define TAG_FLUSH_BUFFER (4)

/**
 * TAG_BREAK_CTL marks packets from kernel to master, containing
 * the current slave open count, because it changed, and the amount of change.
 *
 * Data contains two words of type int:
 * * first word containing the relative change of the counter: -1 for close, +1 for open
 * * second word contains the current absolute open count
 */
#define TAG_OPEN_COUNT (5)

#include <asm/ioctl.h>

// vtmx-side ioctls
// can't be bothered to invent own ioctl numbers

/**
 * Master side ioctl() to get the number of the slave device, that is
 * associated with the current master connection.
 */
#define VTMX_GET_VTTY_NUM (TIOCGPTN)

/**
 * Master side ioctl() to set the modem lines of the virtual port.
 *
 * @note The modem lines, that are allowed to be set here are in terms
 * of bit defintions for ioctl() (see man page ioctl_tty(2) ):
 * * all known input/modem-state lines: TIOCM_CTS, TIOCM_CAR/TIOCM_CD, TIOCM_RNG/TIOCM_RI, TIOCM_DSR
 * * if configured: all known output/modem-control lines: TIOCM_DTR, TIOCM_RTS
 * * probably more, i.e. see ref.: https://elixir.bootlin.com/linux/latest/source/include/uapi/asm-generic/termios.h
 *
 * @note The report of output lines is allowed, if configured at module load time by full_state_control.
 */
#define VTMX_SET_MODEM_LINES (TIOCMSET)

#endif
