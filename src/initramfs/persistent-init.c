/* Initramfs PID 1: loop-mounts the ext4 rootfs stored inside the "super" partition,
 * starts a USB-serial SSH rescue listener and a 600 s boot watchdog, then hands over
 * to OpenRC. Static aarch64; build with tools/build-helpers.sh. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <linux/reboot.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define TOKEN "PMLINUXROOT"
#define ROOT_OFFSET 4294967296ULL
#define ROOT_LENGTH 4294967296ULL
static int klog = -1;
static int pmsg = -1;
static int gserial = -1;
static const char *boot_stage = "PID1_START";
static void logmsg(const char *text) {
    if (klog >= 0) dprintf(klog,"<3>" TOKEN " %s\n",text);
    if (pmsg >= 0) dprintf(pmsg,TOKEN " %s\n",text);
}
static int open_serial(void) {
    if(gserial>=0) return 0;
    gserial=open("/dev/ttyGS0",O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
    if(gserial<0) return -1;
    struct termios tty;
    if(tcgetattr(gserial,&tty)<0) { close(gserial); gserial=-1; return -1; }
    cfmakeraw(&tty);
    tty.c_cflag|=CLOCAL|CREAD;
    tty.c_cflag&=~HUPCL;
    if(tcsetattr(gserial,TCSANOW,&tty)<0) { close(gserial); gserial=-1; return -1; }
    return 0;
}
static void serial_line(const char *body) {
    char line[256];
    int length=snprintf(line,sizeof(line),TOKEN " %s\n",body);
    if(length<=0 || (size_t)length>=sizeof(line)) return;
    if(open_serial()<0) return;
    (void)write(gserial,line,(size_t)length);
}
static void set_stage(const char *stage) {
    boot_stage=stage;
    logmsg(stage);
    char body[160];
    snprintf(body,sizeof(body),"BOOT_STAGE=%s",stage);
    serial_line(body);
}
static void reboot_bootloader(int unused) {
    (void)unused;
    char message[192];
    snprintf(message,sizeof(message),"RETURN_BOOTLOADER stage=%s",boot_stage);
    logmsg(message);
    sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, "bootloader");
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART, NULL);
    for (;;) pause();
}
static void fail_boot(const char *reason) {
    char message[192],body[224];
    snprintf(message,sizeof(message),"BOOT_FAILURE=%s LAST_STAGE=%s",reason,boot_stage);
    logmsg(message);
    snprintf(body,sizeof(body),"BOOT_FAILURE=%s LAST_STAGE=%s",reason,boot_stage);
    for(int i=0;i<12;i++) {
        serial_line(body);
        sleep(1);
    }
    reboot_bootloader(0);
}
static int put(const char *path, const char *value) {
    int fd=open(path,O_WRONLY|O_CLOEXEC);
    if(fd<0) return -1;
    size_t len=strlen(value);
    ssize_t n=write(fd,value,len);
    close(fd);
    return n==(ssize_t)len ? 0 : -1;
}
static int temperature(void) {
    FILE *f=fopen("/sys/class/power_supply/battery/temp","r");
    int t=-1;
    if(f) { if(fscanf(f,"%d",&t)!=1) t=-1; fclose(f); }
    return t;
}
static int usb_identity(void) {
    logmsg("USB_CONFIGFS_BEGIN");
    /* MT6877 driver exposes this mode for explicitly enabling peripheral USB.
     * It does not enable OTG or request high-current charging. */
    if(mount("configfs","/sys/kernel/config","configfs",0,NULL)<0 && errno!=EBUSY) return -1;
    const char *root="/sys/kernel/config/usb_gadget/redmi_probe";
    if(mkdir(root,0700)<0 && errno!=EEXIST) return -1;
    if(chdir(root)<0) return -1;
    logmsg("USB_CONFIGFS_CREATED");
    if(put("idVendor","0x1d6b") || put("idProduct","0x0104")) return -1;
    put("bDeviceClass","0x02");
    put("bcdUSB","0x0200");
    put("bcdDevice","0x0100");
    mkdir("strings/0x409",0700);
    put("strings/0x409/serialnumber",TOKEN);
    put("strings/0x409/manufacturer","Local Linux probe");
    put("strings/0x409/product","Redmi native Linux diagnostic");
    mkdir("configs/c.1",0700);
    mkdir("configs/c.1/strings/0x409",0700);
    put("configs/c.1/strings/0x409/configuration","Native Linux proof");
    put("configs/c.1/MaxPower","250");
    if(mkdir("functions/acm.GS0",0700)<0) return -1;
    logmsg("USB_FUNCTION_CREATED");
    if(symlink("/sys/kernel/config/usb_gadget/redmi_probe/functions/acm.GS0","configs/c.1/acm.GS0")<0) return -1;
    DIR *d=opendir("/sys/class/udc");
    if(!d) return -1;
    struct dirent *entry;
    int rc=-1;
    while((entry=readdir(d))) {
        if(entry->d_name[0]=='.') continue;
        logmsg("USB_BIND_BEGIN");
        rc=put("UDC",entry->d_name);
        break;
    }
    closedir(d);
    if(rc==0) put("/sys/devices/platform/11201000.mtu3_0/cmode","3");
    chdir("/");
    return rc;
}
static pid_t start_ssh(void) {
    pid_t pid=fork();
    if(pid!=0) return pid;
    alarm(0);
    signal(SIGUSR1,SIG_DFL);
    if(gserial>=0) { close(gserial); gserial=-1; }
    for(;;) {
        int fd=open("/dev/ttyGS0",O_RDWR|O_NOCTTY);
        if(fd<0) { sleep(2); continue; }
        struct termios tty;
        if(tcgetattr(fd,&tty)<0) { close(fd); sleep(2); continue; }
        cfmakeraw(&tty);
        tty.c_cflag|=CLOCAL|CREAD;
        tty.c_cflag&=~HUPCL;
        cfsetispeed(&tty,B115200);
        cfsetospeed(&tty,B115200);
        if(tcsetattr(fd,TCSANOW,&tty)<0) { close(fd); sleep(2); continue; }

        /* Keep a rescue listener independent of OpenRC startup ordering. */
        char line[64];
        size_t used=0;
        int matched=0;
        int received=0;
        while(!matched) {
            struct pollfd input={.fd=fd,.events=POLLIN};
            int ready=poll(&input,1,1000);
            if(ready<0 && errno==EINTR) continue;
            if(ready<0 || (ready>0 && (input.revents&(POLLERR|POLLHUP|POLLNVAL)))) break;
            if(ready==0) {
                serial_line("BOOT_STAGE=USB_RESCUE_WAIT");
                continue;
            }
            char c;
            ssize_t n=read(fd,&c,1);
            if(n<0 && errno==EINTR) continue;
            if(n!=1) break;
            if(!received) {
                serial_line("BOOT_STAGE=USB_RX_BYTE");
                received=1;
            }
            if(c=='\r') continue;
            if(c=='\n') {
                line[used]=0;
                if(strcmp(line,"STARTSSH")==0) {
                    serial_line("BOOT_STAGE=USB_STARTSSH_MATCHED");
                    matched=1;
                }
                used=0;
            } else if(used<sizeof(line)-1) line[used++]=c;
            else used=0;
        }
        if(!matched) { close(fd); sleep(2); continue; }

        /* Windows purges serial input when opening COM. Send the banner only
         * after its explicit request, then serve one SSH session and repeat. */
        if(write(fd,"BOOTSTRAP_READY\n",16)!=16) { close(fd); sleep(1); continue; }
        pid_t session=fork();
        if(session==0) {
            dup2(fd,STDIN_FILENO); dup2(fd,STDOUT_FILENO);
            int err=open("/dev/null",O_WRONLY|O_CLOEXEC);
            if(err>=0) { dup2(err,STDERR_FILENO); if(err>2) close(err); }
            if(fd>2) close(fd);
            execl("/usr/sbin/sshd","sshd","-i","-e","-f",
                  "/etc/ssh/sshd_config",(char *)NULL);
            _exit(14);
        }
        close(fd);
        if(session<0) { sleep(2); continue; }
        while(waitpid(session,NULL,0)<0 && errno==EINTR) {}
        sleep(1);
    }
}
static int create_node(const char *path, mode_t mode, dev_t device) {
    if(mknod(path,mode,device)==0 || errno==EEXIST) return 0;
    return -1;
}

static int locate_super(char *path,size_t path_size) {
    DIR *directory=opendir("/sys/class/block");
    if(!directory) return -1;
    struct dirent *entry;
    while((entry=readdir(directory))) {
        if(entry->d_name[0]=='.') continue;
        char uevent[PATH_MAX];
        snprintf(uevent,sizeof(uevent),"/sys/class/block/%s/uevent",entry->d_name);
        FILE *f=fopen(uevent,"r");
        if(!f) continue;
        char line[256],name[128]="";
        int match=0;
        while(fgets(line,sizeof(line),f)) {
            line[strcspn(line,"\r\n")]=0;
            if(!strcmp(line,"PARTNAME=super")) match=1;
            else if(!strncmp(line,"DEVNAME=",8) && strlen(line+8)<sizeof(name))
                strcpy(name,line+8);
        }
        fclose(f);
        if(match && name[0]) {
            int n=snprintf(path,path_size,"/dev/%s",name);
            closedir(directory);
            return n>0 && (size_t)n<path_size ? 0 : -1;
        }
    }
    closedir(directory);
    errno=ENOENT;
    return -1;
}

static int mount_persistent_root(void) {
    char block_path[PATH_MAX],device_path[PATH_MAX],device_file[PATH_MAX];
    set_stage("SUPER_LOOKUP");
    if(locate_super(block_path,sizeof(block_path))<0) return -1;
    const char *name=strrchr(block_path,'/');
    if(!name || snprintf(device_file,sizeof(device_file),"/sys/class/block/%s/dev",name+1)>=(int)sizeof(device_file)) return -1;
    FILE *f=fopen(device_file,"r");
    unsigned int major_num=0,minor_num=0;
    if(!f || fscanf(f,"%u:%u",&major_num,&minor_num)!=2) { if(f) fclose(f); return -1; }
    fclose(f);
    if(create_node(block_path,S_IFBLK|0600,makedev(major_num,minor_num))<0) return -1;
    set_stage("SUPER_OPEN");
    int super=open(block_path,O_RDWR|O_CLOEXEC);
    if(super<0) return -1;
    int read_only=1;
    unsigned long long capacity=0;
    set_stage("SUPER_BLOCK_CHECK");
    if(ioctl(super,BLKROGET,&read_only)<0 || read_only ||
       ioctl(super,BLKGETSIZE64,&capacity)<0 || capacity<ROOT_OFFSET+ROOT_LENGTH) {
        close(super); errno=EROFS; return -1;
    }
    set_stage("SUPER_BLOCK_OK");
    unsigned short magic=0;
    set_stage("EXT4_MAGIC_CHECK");
    if(pread(super,&magic,sizeof(magic),(off_t)(ROOT_OFFSET+1024+0x38))!=sizeof(magic) || magic!=0xEF53) {
        close(super); errno=EINVAL; return -1;
    }
    set_stage("EXT4_MAGIC_OK");
    set_stage("LOOP_SETUP");
    if(create_node("/dev/loop-control",S_IFCHR|0600,makedev(10,237))<0) { close(super); return -1; }
    int control=open("/dev/loop-control",O_RDWR|O_CLOEXEC);
    if(control<0) { close(super); return -1; }
    int number=ioctl(control,LOOP_CTL_GET_FREE);
    close(control);
    if(number<0) { close(super); return -1; }
    snprintf(device_path,sizeof(device_path),"/dev/loop%d",number);
    if(create_node(device_path,S_IFBLK|0600,makedev(7,(unsigned int)number))<0) { close(super); return -1; }
    int loop=open(device_path,O_RDWR|O_CLOEXEC);
    if(loop<0) { close(super); return -1; }
    if(ioctl(loop,LOOP_SET_FD,super)<0) { close(loop); close(super); return -1; }
    struct loop_info64 info;
    memset(&info,0,sizeof(info));
    info.lo_offset=ROOT_OFFSET;
    info.lo_sizelimit=ROOT_LENGTH;
    info.lo_flags=LO_FLAGS_AUTOCLEAR;
    if(ioctl(loop,LOOP_SET_STATUS64,&info)<0) { ioctl(loop,LOOP_CLR_FD,0); close(loop); close(super); return -1; }
    set_stage("LOOP_ATTACHED");
    close(super);
    if(mount(device_path,"/newroot","ext4",MS_NOATIME,NULL)<0) {
        ioctl(loop,LOOP_CLR_FD,0); close(loop); return -1;
    }
    set_stage("EXT4_MOUNTED");
    logmsg("PERSISTENT_EXT4_MOUNTED");
    if(mkdir("/newroot/run",0755)<0 && errno!=EEXIST) { close(loop); return -1; }
    set_stage("RUN_TMPFS");
    if(mount("tmpfs","/newroot/run","tmpfs",MS_NOSUID|MS_NODEV,"mode=0755,size=64m")<0) { close(loop); return -1; }
    if(mkdir("/newroot/tmp",01777)<0 && errno!=EEXIST) { close(loop); return -1; }
    set_stage("TMP_TMPFS");
    if(mount("tmpfs","/newroot/tmp","tmpfs",MS_NOSUID,"mode=1777,size=128m")<0) { close(loop); return -1; }
    set_stage("ROOT_PREPARED");
    close(loop);
    return 0;
}

static void boot_watchdog(void) {
    sleep(600);
    logmsg("ROOTFS_BOOT_WATCHDOG_EXPIRED");
    reboot_bootloader(0);
}

int main(void) {
    if(getpid()!=1) { fputs("This binary only runs as init (PID 1).\n",stderr); return 2; }
    signal(SIGUSR1,reboot_bootloader);
    mkdir("/proc",0755); mkdir("/sys",0755); mkdir("/dev",0755); mkdir("/newroot",0755);
    if(mount("proc","/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)<0) fail_boot("PROC_MOUNT");
    if(mount("sysfs","/sys","sysfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)<0) fail_boot("SYSFS_MOUNT");
    put("/sys/class/firmware/timeout","3");
    if(mount("devtmpfs","/dev","devtmpfs",MS_NOSUID,"mode=0755,size=10M")<0) fail_boot("DEVTMPFS_MOUNT");
    create_node("/dev/kmsg",S_IFCHR|0600,makedev(1,11));
    create_node("/dev/null",S_IFCHR|0666,makedev(1,3));
    create_node("/dev/zero",S_IFCHR|0666,makedev(1,5));
    create_node("/dev/random",S_IFCHR|0666,makedev(1,8));
    create_node("/dev/urandom",S_IFCHR|0666,makedev(1,9));
    create_node("/dev/tty",S_IFCHR|0666,makedev(5,0));
    create_node("/dev/console",S_IFCHR|0600,makedev(5,1));
    symlink("/proc/self/fd","/dev/fd");
    symlink("/proc/self/fd/0","/dev/stdin");
    symlink("/proc/self/fd/1","/dev/stdout");
    symlink("/proc/self/fd/2","/dev/stderr");
    klog=open("/dev/kmsg",O_WRONLY|O_CLOEXEC);
    FILE *pf=fopen("/sys/class/pmsg/pmsg0/dev","r");
    unsigned int pmajor=0,pminor=0;
    if(pf) {
        if(fscanf(pf,"%u:%u",&pmajor,&pminor)==2) {
            create_node("/dev/pmsg0",S_IFCHR|0600,makedev(pmajor,pminor));
            pmsg=open("/dev/pmsg0",O_WRONLY|O_CLOEXEC);
        }
        fclose(pf);
    }
    put("/proc/sys/kernel/printk","8 4 1 7");
    struct utsname u;
    if(!uname(&u)) {
        char text[256];
    snprintf(text,sizeof(text),"PID1_PERSISTENT_ROOTFS kernel=%.120s",u.release);
    logmsg(text);
    }
    set_stage("BASE_FILESYSTEMS_MOUNTED");
    if(usb_identity()!=0) { logmsg("USB_BIND_FAILED"); fail_boot("USB_BIND"); }
    FILE *serialdev=fopen("/sys/class/tty/ttyGS0/dev","r");
    unsigned int major_num=0,minor_num=0;
    if(!serialdev || fscanf(serialdev,"%u:%u",&major_num,&minor_num)!=2) fail_boot("TTYGS0_MISSING");
    fclose(serialdev);
    if(create_node("/dev/ttyGS0",S_IFCHR|0600,makedev(major_num,minor_num))<0) fail_boot("TTYGS0_NODE");
    set_stage("USB_BOUND");
    set_stage("TEMPERATURE_WAIT");
    int t=-1;
    for(int i=0;i<15 && t<0;i++) {
        t=temperature();
        if(t<0) sleep(1);
    }
    if(t<0 || t>=420) { logmsg("TEMPERATURE_GATE_ABORT"); fail_boot("TEMPERATURE_GATE"); }
    set_stage("TEMPERATURE_OK");
    set_stage("ROOT_MOUNT_BEGIN");
    if(mount_persistent_root()<0) { logmsg("PERSISTENT_ROOT_MOUNT_FAILED"); fail_boot("ROOT_MOUNT"); }
    pid_t watchdog=fork();
    if(watchdog<0) fail_boot("WATCHDOG_FORK");
    if(watchdog==0) boot_watchdog();
    int watchdog_file=open("/newroot/run/redmi-watchdog.pid",O_CREAT|O_WRONLY|O_CLOEXEC,0644);
    if(watchdog_file<0) fail_boot("WATCHDOG_PIDFILE");
    dprintf(watchdog_file,"%ld\n",(long)watchdog);
    close(watchdog_file);
    set_stage("MOVE_DEV");
    if(mount("/dev","/newroot/dev",NULL,MS_MOVE,NULL)<0) fail_boot("MOVE_DEV");
    set_stage("MOVE_PROC");
    if(mount("/proc","/newroot/proc",NULL,MS_MOVE,NULL)<0) fail_boot("MOVE_PROC");
    set_stage("MOVE_SYS");
    if(mount("/sys","/newroot/sys",NULL,MS_MOVE,NULL)<0) fail_boot("MOVE_SYS");
    set_stage("MAKE_ROOT_PRIVATE");
    if(mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)<0) fail_boot("ROOT_PRIVATE");
    set_stage("SWITCH_ROOT");
    if(chdir("/newroot")<0) fail_boot("SWITCH_ROOT_CHDIR");
    if(chroot(".")<0) fail_boot("SWITCH_ROOT_CHROOT");
    if(chdir("/")<0) fail_boot("SWITCH_ROOT_CHDIR_ROOT");
    set_stage("SWITCH_ROOT_DONE");
    if(mkdir("/run/sshd",0755)<0 && errno!=EEXIST) fail_boot("SSHD_RUN_DIR");
    if(start_ssh()<0) fail_boot("USB_RESCUE_FORK");
    set_stage("USB_RESCUE_STARTED");
    int console=open("/dev/console",O_RDWR|O_CLOEXEC);
    if(console>=0) { dup2(console,0); dup2(console,1); dup2(console,2); if(console>2) close(console); }
    setenv("PATH","/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",1);
    logmsg("PERSISTENT_ROOTFS_HANDOFF");
    char *args[]={"/sbin/init",NULL};
    execv(args[0],args);
    logmsg("OPENRC_EXEC_FAILED");
    fail_boot("OPENRC_EXEC");
    return 1;
}
