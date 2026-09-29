/* MT6877 built-in connectivity bring-up. No partition writes or network setup. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int node(const char *name,const char *path) {
    FILE *f=fopen("/proc/devices","r");
    if(!f) return -1;
    char line[256],found[128]; unsigned int major_num;
    while(fgets(line,sizeof(line),f)) {
        if(sscanf(line,"%u %127s",&major_num,found)==2 && !strcmp(found,name)) {
            fclose(f);
            if(mknod(path,S_IFCHR|0600,makedev(major_num,0))<0 && errno!=EEXIST) return -1;
            return open(path,O_RDWR|O_CLOEXEC);
        }
    }
    fclose(f);
    errno=ENOENT;
    return -1;
}
int main(void) {
    setbuf(stdout,NULL);
    alarm(45);
    int fd=node("conninfra_drv","/dev/conninfra_dev");
    if(fd<0) { perror("conninfra node"); return 1; }
    errno=0;
    int rc=ioctl(fd,_IOR(0xc2,2,int),0);
    printf("CONNINFRA_INIT rc=%d errno=%d\n",rc,errno);
    if(rc<0) { close(fd); return 2; }
    errno=0;
    rc=ioctl(fd,_IOR(0xc2,0,int),0);
    printf("CONNINFRA_CHIP rc=%d errno=%d\n",rc,errno);
    close(fd);
    fd=node("mtk_wmt_wifi_chrdev","/dev/wmtWifi");
    if(fd<0) { perror("wmtWifi node"); return 3; }
    /* Forward the device's original calibration bytes opaquely to the vendor
     * driver's documented buffer endpoint. No parsing or persistent writes. */
    unsigned char buffer[12+8192];
    memcpy(buffer,"WR-BUF:NVRAM",12);
    FILE *cal=fopen("/etc/hardware/wifi-nvram.bin","rb");
    if(!cal) { perror("calibration file"); close(fd); return 5; }
    size_t size=fread(buffer+12,1,8192,cal);
    int invalid=ferror(cal) || size==0 || fgetc(cal)!=EOF;
    fclose(cal);
    if(invalid) { close(fd); return 6; }
    ssize_t sent=write(fd,buffer,size+12);
    printf("NVRAM_OPAQUE_TRANSFER bytes=%zd expected=%zu\n",sent,size+12);
    if(sent!=(ssize_t)(size+12)) { close(fd); return 7; }
    /* The built-in BT/Wi-Fi pre-calibration runs asynchronously and takes the
     * same write mutex as station activation. Allow it to finish first. */
    puts("NVRAM_READY_CALIBRATION_WAIT");
    sleep(12);
    errno=0;
    ssize_t n=write(fd,"S",1);
    printf("WIFI_STATION_ENABLE bytes=%zd errno=%d\n",n,errno);
    close(fd);
    return n==1 ? 0 : 4;
}
