#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "usb.h"
#include "block.h"
#include "shell.h"

static uint64_t rand_state;

static void cmd_ticks(int argc, char **argv) { out("Ticks: %ld\n", (long)ticks); }

static void cmd_uptime(int argc, char **argv)
{
	uint64_t s = ticks / HZ;
	out("Uptime: %ld:%02ld:%02ld\n", (long)(s / 3600), (long)(s / 60 % 60), (long)(s % 60));
}

static void cmd_cpuinfo(int argc, char **argv)
{
	uint32_t pvr = platform_pvr();
	out("CPU: %s, pvr %08x\n", pvr >> 16 == 0x7100 ? "Xenon, 3 cores 6 threads" : "PowerPC 64", pvr);
	out("Timebase: %ld Hz\n", (long)platform_tb_freq());
	out("Machine: %s\n", platform_name());
}

static void cmd_cpuid(int argc, char **argv)
{
	uint32_t pvr = platform_pvr();
	out("PVR: %08x  version %04x  revision %04x\n", pvr, pvr >> 16, pvr & 0xffff);
}

// TODO no rtc yet, the 360 keeps it in the smc. the date starts at 2026 and counts uptime
static void cmd_date(int argc, char **argv)
{
	char d[32];
	fs_date(fs_now(), d, sizeof d);
	out("%s\n", d);
}

static void cmd_irqinfo(int argc, char **argv)
{
	out("Decrementer: %d Hz, %ld ticks\n", HZ, (long)ticks);
	out("External interrupts: masked, usb is polled every 10 ms\n");
}

static void cmd_palette(int argc, char **argv)
{
	for (int i = 0; i < 16; i++) {
		out_color(i);
		out("%02d ", i);
	}
	out_color(WHITE);
	out("\n");
}

// xorshift from the timebase
static void cmd_rand(int argc, char **argv)
{
	if (!rand_state)
		rand_state = mftb() | 1;
	rand_state ^= rand_state << 13;
	rand_state ^= rand_state >> 7;
	rand_state ^= rand_state << 17;
	out("%ld\n", (long)(rand_state % 1000000));
}

static void cmd_hex(int argc, char **argv)
{
	long v;
	if (argc != 2 || sh_number(argv[1], &v)) {
		sh_error("Usage: hex <number>");
		return;
	}
	out("0x%lx\n", v);
}

static void cmd_dec(int argc, char **argv)
{
	long v;
	char tmp[40];
	snprintf(tmp, sizeof tmp, "0x%s", argc == 2 ? argv[1] : "");
	if (argc != 2 || sh_number(argv[1][0] == '0' && argv[1][1] == 'x' ? argv[1] : tmp, &v)) {
		sh_error("Usage: dec <hex>");
		return;
	}
	out("%ld\n", v);
}

static void cmd_beep(int argc, char **argv) { sh_error("No speaker on this box"); }
static void cmd_ps(int argc, char **argv) { threads_list(); }
static void cmd_lsusb(int argc, char **argv) { usb_list(); }
static void cmd_lsblk(int argc, char **argv) { block_list(); }

static void cmd_mem(int argc, char **argv)
{
	out("Free: %ld KB in 4 KB pages\n", (long)pages_free() * 4);
}

static void cmd_mouse(int argc, char **argv)
{
	out("Mouse: x %d y %d buttons %d\n", mouse_x, mouse_y, mouse_buttons);
}

static void cmd_sleep(int argc, char **argv)
{
	long s;
	if (argc != 2 || sh_number(argv[1], &s) || s < 0) {
		sh_error("Usage: sleep <seconds>");
		return;
	}
	sleep_ms(s * 1000);
}

static void cmd_true(int argc, char **argv) {}
static void cmd_false(int argc, char **argv) {}

static void cmd_seq(int argc, char **argv)
{
	long first = 1, step = 1, last;
	if (argc == 2 && !sh_number(argv[1], &last)) {
	} else if (argc == 3 && !sh_number(argv[1], &first) && !sh_number(argv[2], &last)) {
	} else if (argc == 4 && !sh_number(argv[1], &first) && !sh_number(argv[2], &step) &&
		   !sh_number(argv[3], &last) && step) {
	} else {
		sh_error("Usage: seq [first [step]] last");
		return;
	}
	for (long v = first; step > 0 ? v <= last : v >= last; v += step)
		out("%ld\n", v);
}

static void cmd_halt(int argc, char **argv)
{
	out("System halted.\n");
	platform_halt();
}

static void cmd_reboot(int argc, char **argv) { platform_reboot(); }
static void cmd_shutdown(int argc, char **argv) { platform_poweroff(); }

const struct command cmd_sys[] = {
	{"ticks", cmd_ticks}, {"uptime", cmd_uptime}, {"cpuinfo", cmd_cpuinfo}, {"cpuid", cmd_cpuid},
	{"date", cmd_date}, {"time", cmd_date}, {"irqinfo", cmd_irqinfo}, {"palette", cmd_palette},
	{"rand", cmd_rand}, {"hex", cmd_hex}, {"dec", cmd_dec}, {"beep", cmd_beep}, {"ps", cmd_ps},
	{"mem", cmd_mem}, {"lsusb", cmd_lsusb}, {"lsblk", cmd_lsblk}, {"mouse", cmd_mouse},
	{"mousepos", cmd_mouse}, {"sleep", cmd_sleep}, {"true", cmd_true}, {"false", cmd_false},
	{"seq", cmd_seq}, {"halt", cmd_halt}, {"reboot", cmd_reboot},
	{"shutdown", cmd_shutdown},
	{NULL, NULL}
};
