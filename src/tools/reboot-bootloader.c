/* Return the running Linux to the Fastboot bootloader for PC-side recovery.
 * Same reboot request as the initramfs watchdog; requires a confirmation
 * argument so it cannot be triggered by an accidental plain invocation. */
#define _GNU_SOURCE
#include <linux/reboot.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--confirm-bootloader")) {
        fputs("usage: redmi-reboot-bootloader --confirm-bootloader\n", stderr);
        return 64;
    }
    sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, "bootloader");
    perror("reboot bootloader");
    return 1;
}
