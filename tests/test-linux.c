/*
 */

#include "qemu-common.h"
#include "nbd.h"
#include "qemu_socket.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glib.h>
#include <linux/iso_fs.h>

static int systemf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static int systemf(const char *fmt, ...)
{
    char buffer[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    g_test_message("Running command %s\n", buffer);

    return system(buffer);
}

static int qemu_nbd_open(const char *filename)
{
    int fd, ret;
    ssize_t len;
    struct sockaddr_un addr;
    char buf[8 + 8 + 8 + 128];

    if (fork() == 0) {
        systemf("./qemu-nbd -v -R -k /tmp/nbd.sock \"%s\"", filename);
        exit(0);
    }

    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    g_assert(fd != -1);

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "/tmp/nbd.sock");

    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    g_assert(ret != -1);

    len = read(fd, buf, sizeof(buf));
    if (len == -1) {
        printf("read: %m\n");
    }
    g_assert_cmpint(len, ==, sizeof(buf));

    return fd;
}

static ssize_t qemu_nbd_read(int fd, void *data, size_t size, off_t offset)
{
    struct nbd_request req;
    struct nbd_reply reply;
    size_t len;

    req.magic = cpu_to_be32(NBD_REQUEST_MAGIC);
    req.type = cpu_to_be32(NBD_CMD_READ);
    req.handle = cpu_to_be64(0x01);
    req.from = cpu_to_be64(offset);
    req.len = cpu_to_be32(size);

    len = write(fd, &req, sizeof(req));
    g_assert(len == sizeof(req));

    len = read(fd, &reply, sizeof(reply));
    g_assert(len == sizeof(reply));

    reply.magic = be32_to_cpu(reply.magic);
    reply.error = be32_to_cpu(reply.error);
    reply.handle = be64_to_cpu(reply.handle);

    g_assert(reply.magic == NBD_REPLY_MAGIC);
    g_assert(reply.error == 0);
    g_assert(reply.handle == 0x01);

    len = read(fd, data, size);
    g_assert(len == size);

    return len;
}

static ssize_t qemu_nbd_pread(int fd, void *data, size_t size, off_t offset)
{
    void *buffer;
    off_t sub_offset = 0;
    size_t real_size = size;

    if (offset & 511) {
        sub_offset = offset & 511;
        size += sub_offset;
    }

    if (size & 511) {
        size += 511;
        size &= ~511UL;
    }

    buffer = g_malloc(size);
    qemu_nbd_read(fd, buffer, size, offset);
    memcpy(data, buffer + sub_offset, real_size);

    return real_size;
}

static uint8_t iso_u8(void *ptr)
{
    uint8_t *buf = (uint8_t *)ptr;
    return buf[0];
}

static inline uint16_t iso_u16(void *ptr)
{
    uint8_t *buf = (uint8_t *)ptr;
    return buf[0] | (buf[1] << 8);
}

static inline uint32_t iso_u32(void *ptr)
{
    uint8_t *buf = (uint8_t *)ptr;
    return (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | buf[0];
}

typedef struct IsoFile
{
    int directory;
    off_t offset;
    off_t size;
} IsoFile;

static IsoFile *iso_find_path(int fd, IsoFile *dir, const char *pathname)
{
    struct iso_directory_record record;
    ssize_t len;
    char name[257];
    uint8_t name_len;
    off_t offset = dir->offset;

    if (!dir->directory) {
        return NULL;
    }

    len = qemu_nbd_pread(fd, &record, sizeof(record), offset);
    assert(len == sizeof(record));

    while ((offset - dir->offset) < dir->size) {
        size_t size;
        int directory;

        name_len = iso_u8(record.name_len);
        directory = !!(iso_u8(record.flags) & 0x02);

        size = len;
        len = qemu_nbd_pread(fd, name, name_len, offset + len);
        assert(len == name_len);
        name[name_len] = 0;

        size += len;

        if (size < iso_u8(record.length)) {
            size_t record_length = iso_u8(record.length) - size;
            char buffer[256];
            int i = 0;

            len = qemu_nbd_pread(fd, buffer, record_length, offset + size);
            assert(len == record_length);
            if (buffer[i] == 0) {
                i++;
            }

            if (record_length > 5 && buffer[i] == 'R' && buffer[i + 1] == 'R') {
                i += 5;
                while (i < record_length) {
                    if (buffer[i] == 'N' && buffer[i + 1] == 'M') {
                        name_len = buffer[i + 2] - 5;
                        memcpy(name, &buffer[i + 5], name_len);
                        name[name_len] = 0;
                        i += buffer[i + 2];
                    } else {
                        break;
                    }
                }
            }
        }

        if (strcmp(name, pathname) == 0) {
            IsoFile *ret = malloc(sizeof(*ret));
            ret->directory = directory;
            ret->offset = iso_u32(record.extent) * 2048;
            ret->size = iso_u32(record.size);
            return ret;
        }

        offset += iso_u8(record.length);

        do {
            len = qemu_nbd_pread(fd, &record, sizeof(record), offset);
            assert(len == sizeof(record));

            if (iso_u8(record.length) == 0) {
                offset += 2047;
                offset &= ~2047ULL;
            }
        } while (iso_u8(record.length) == 0 && (offset - dir->offset) < dir->size);
    }
    
    return NULL;
}

static IsoFile *iso_find_root(int fd)
{
    off_t offset = 0;
    ssize_t len;
    struct iso_volume_descriptor vd;

    offset = 32768;
    do {
        len = qemu_nbd_pread(fd, &vd, sizeof(vd), offset);
        assert(len == sizeof(vd));

        if (iso_u8(vd.type) == 1) {
            struct iso_primary_descriptor *pd;
            struct iso_directory_record *root;
            char name[256];
            IsoFile *ret;

            pd = (struct iso_primary_descriptor *)&vd;
            root = (struct iso_directory_record *)pd->root_directory_record;

            memcpy(name, root->name, iso_u8(root->name_len));
            name[iso_u8(root->name_len)] = 0;

            ret = malloc(sizeof(*ret));
            ret->directory = 1;
            ret->offset = iso_u32(root->extent) * 2048;
            ret->size = iso_u32(root->size);
            return ret;
        }

        offset += len;
    } while (iso_u8(vd.type) != 255);

    return NULL;
}

static IsoFile *iso_find_file(int fd, const char *filename)
{
    const char *end;
    IsoFile *cur = iso_find_root(fd);

    while (cur && filename) {
        IsoFile *dir;
        char pathname[257];

        end = strchr(filename, '/');
        if (end) {
            memcpy(pathname, filename, end - filename);
            pathname[end - filename] = 0;
            filename = end + 1;
        } else {
            snprintf(pathname, sizeof(pathname), "%s", filename);
            filename = end;
        }

        dir = iso_find_path(fd, cur, pathname);
        free(cur);
        cur = dir;
    }

    return cur;
}

static const char preseed[] = 
    "d-i	debian-installer/locale	string en_US.UTF-8\n"
    "d-i	debian-installer/splash boolean false\n"
    "d-i	console-setup/ask_detect	boolean false\n"
    "d-i	console-setup/layoutcode	string us\n"
    "d-i	console-setup/variantcode	string \n"
    "d-i	netcfg/get_nameservers	string \n"
    "d-i	netcfg/get_ipaddress	string \n"
    "d-i	netcfg/get_netmask	string 255.255.255.0\n"
    "d-i	netcfg/get_gateway	string \n"
    "d-i	netcfg/confirm_static	boolean true\n"
    "d-i	clock-setup/utc	boolean true\n"
    "d-i 	partman-auto/method string regular\n"
    "d-i 	partman-lvm/device_remove_lvm boolean true\n"
    "d-i 	partman-lvm/confirm boolean true\n"
    "d-i 	partman/confirm_write_new_label boolean true\n"
    "d-i 	partman/choose_partition        select Finish partitioning and write changes to disk\n"
    "d-i 	partman/confirm boolean true\n"
    "d-i 	partman/confirm_nooverwrite boolean true\n"
    "d-i 	partman/default_filesystem string ext3\n"
    "d-i 	clock-setup/utc boolean true\n"
    "d-i	clock-setup/ntp	boolean true\n"
    "d-i	clock-setup/ntp-server	string ntp.ubuntu.com\n"
    "d-i	base-installer/kernel/image	string linux-server\n"
    "d-i	passwd/root-login	boolean false\n"
    "d-i	passwd/make-user	boolean true\n"
    "d-i	passwd/user-fullname	string ubuntu\n"
    "d-i	passwd/username	string ubuntu\n"
    "d-i	passwd/user-password-crypted	password $6$.1eHH0iY$ArGzKX2YeQ3G6U.mlOO3A.NaL22Ewgz8Fi4qqz.Ns7EMKjEJRIW2Pm/TikDptZpuu7I92frytmk5YeL.9fRY4.\n"
    "d-i	passwd/user-uid	string \n"
    "d-i	user-setup/allow-password-weak	boolean false\n"
    "d-i	user-setup/encrypt-home	boolean false\n"
    "d-i	passwd/user-default-groups	string adm cdrom dialout lpadmin plugdev sambashare\n"
    "d-i	apt-setup/services-select	multiselect security\n"
    "d-i	apt-setup/security_host	string security.ubuntu.com\n"
    "d-i	apt-setup/security_path	string /ubuntu\n"
    "d-i	debian-installer/allow_unauthenticated	string false\n"
    "d-i	pkgsel/upgrade	select safe-upgrade\n"
    "d-i	pkgsel/language-packs	multiselect \n"
    "d-i	pkgsel/update-policy	select none\n"
    "d-i	pkgsel/updatedb	boolean true\n"
    "d-i	grub-installer/skip	boolean false\n"
    "d-i	lilo-installer/skip	boolean false\n"
    "d-i	grub-installer/only_debian	boolean true\n"
    "d-i	grub-installer/with_other_os	boolean true\n"
    "d-i	finish-install/keep-consoles	boolean false\n"
    "d-i	finish-install/reboot_in_progress	note \n"
    "d-i	cdrom-detect/eject	boolean true\n"
    "d-i	debian-installer/exit/halt	boolean false\n"
    "d-i	debian-installer/exit/poweroff	boolean false\n"
    "d-i	preseed/late_command		string echo -ne '\x1' | dd bs=1 count=1 seek=1281 of=/dev/port\n"
    ;

static const char ks[] =
    "install\n"
    "text\n"
    "reboot\n"
    "lang en_US.UTF-8\n"
    "keyboard us\n"
    "network --bootproto dhcp\n"
    "rootpw 123456\n"
    "firewall --enabled --ssh\n"
    "selinux --enforcing\n"
    "timezone --utc America/New_York\n"
    "firstboot --disable\n"
    "bootloader --location=mbr --append=\"console=tty0 console=ttyS0,115200\"\n"
    "zerombr\n"
    "clearpart --all --initlabel\n"
    "autopart\n"
    "reboot\n"
    "\n"
    "%packages\n"
    "@base\n"
    "@core\n"
    "%end\n"
    "%post\n"
    "echo -ne '\x1' | dd bs=1 count=1 seek=1281 of=/dev/port\n"
    "%end\n"
    ;

typedef struct WeightedChoice
{
    const char *string;
    int percentage;
} WeightedChoice;

static const char *choose(WeightedChoice *choices)
{
    int i, value;
    int cur_percentage = 0;

    value = g_test_rand_int_range(0, 100);
    for (i = 0; choices[i].string; i++) {
        cur_percentage += choices[i].percentage;
        if (value < cur_percentage) {
            return choices[i].string;
        }
    }

    g_assert_not_reached();
    return NULL;
}

static void copy_file_from_iso(const char *iso, const char *src, const char *dst)
{
    int iso_fd, dst_fd;
    IsoFile *filp;
    off_t offset;

    iso_fd = qemu_nbd_open(iso);
    dst_fd = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);

    g_assert(iso_fd != -1);
    g_assert(dst_fd != -1);

    filp = iso_find_file(iso_fd, src);

    for (offset = 0; offset < filp->size; offset += 2048) {
        char buffer[2048];
        size_t size = MIN(filp->size - offset, 2048);
        ssize_t len;

        len = qemu_nbd_pread(iso_fd, buffer, size, filp->offset + offset);
        g_assert(len == size);

        len = pwrite(dst_fd, buffer, size, offset);
        g_assert(len == size);
    }

    close(dst_fd);
    close(iso_fd);
}

static void test_image(const char *command,
                       const char *image, const char *iso,
                       const char *kernel, const char *initrd,
                       const char *cmdline, const char *config_file,
                       bool pseries)
{
    char buffer[1024];
    int fd, rfd;
    pid_t pid;
    int status, ret;
    long max_cpus, max_mem;
    int num_cpus, mem_mb;
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    const char *tmp_kernel = "/tmp/test-vmlinuz";
    const char *tmp_initrd = "/tmp/test-initrd";
    const char *tmp_socket = "/tmp/test-socket";
    const char *nic_type, *blk_type, *cache_type, *disk_format, *aio_method;
    const char *vga_type;
    WeightedChoice nic_types[] = {
        { "e1000", 25 },
        { "virtio", 50 },
        { "rtl8139", 25 },
        { }
    };
    WeightedChoice blk_types[] = {
        { "virtio", 50 },
        { "ide", 50 },
        { }
    };
    WeightedChoice cache_types[] = {
        { "none", 50 },
        { "writethrough", 50 },
        { }
    };
    WeightedChoice disk_formats[] = {
        { "raw", 50 },
        { "qcow2", 25 },
        { "qed", 25 },
        { }
    };
    WeightedChoice aio_methods[] = {
        { "threads", 75 },
        { "native", 25 },
        { }
    };
    WeightedChoice vga_types[] = {
        { "cirrus", 80 },
        { "std", 20 },
        { }
    };

    max_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    num_cpus = MIN(g_test_rand_int_range(1, max_cpus + 1), 8);

    /* Don't use more than 1/2 of physical memory */
    max_mem = (sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE)) >> 20;
    max_mem /= 2;

    mem_mb = (g_test_rand_int_range(128, max_mem) + 7) & ~0x03;

    nic_type = choose(nic_types);
    blk_type = choose(blk_types);
    cache_type = choose(cache_types);
    disk_format = choose(disk_formats);
    aio_method = choose(aio_methods);
    vga_type = choose(vga_types);

    if (pseries) {
        nic_type = "ibmveth";
        blk_type = "scsi";
        vga_type = "none";
    }

    if (strcmp(cache_type, "none") != 0) {
        aio_method = "threads";
    }

    nic_type = "e1000";
    blk_type = "ide";

    g_test_message("Using %d VCPUS", num_cpus);
    g_test_message("Using %d MB of RAM", mem_mb);
    g_test_message("Using `%s' network card", nic_type);
    g_test_message("Using `%s' block device, cache=`%s', format=`%s', aio=`%s'",
                   blk_type, cache_type, disk_format, aio_method);
    g_test_message("Using `%s' graphics card", vga_type);

    copy_file_from_iso(iso, kernel, tmp_kernel);
    copy_file_from_iso(iso, initrd, tmp_initrd);

    fd = socket(PF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", tmp_socket);

    unlink(tmp_socket);

    ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    g_assert(ret != -1);

    ret = listen(fd, 1);
    g_assert(ret != -1);

    pid = fork();
    if (pid == 0) {
        int status;

        status = systemf("./qemu-img create -f %s %s 10G", disk_format, image);
        if (status != 0) {
            exit(WEXITSTATUS(status));
        }

        status = systemf("%s "
                         "-drive file=%s,if=%s,cache=%s,aio=%s,index=0 "
                         "-drive file=%s,media=cdrom,copy-on-read=on,index=2 "
                         "-chardev socket,path=%s,wait=off,id=httpd "
                         "-net user,guestfwd=tcp:10.0.2.1:80-chardev:httpd "
                         "-net nic,model=%s "
                         "-kernel %s -initrd %s "
                         "-append '%s' -vga %s "
                         "-serial stdio -vnc none -smp %d -m %d ",
                         command, image, blk_type, cache_type, aio_method,
                         iso, tmp_socket, nic_type, tmp_kernel, tmp_initrd,
                         cmdline, vga_type, num_cpus, mem_mb);
        unlink(image);

        if (!WIFEXITED(status)) {
            exit(1);
        }

        exit(WEXITSTATUS(status));
    }

    rfd = accept(fd, &addr, &addrlen);
    g_assert(rfd != -1);

    printf("got client connection\n");

    GString *req = g_string_new("");

    do {
        ret = read(rfd, buffer, sizeof(buffer));
        g_assert(ret > 0);
        g_string_append_len(req, buffer, ret);
    } while (!strstr(req->str, "\r\n\r\n"));

    printf("got request:\n%s", req->str);

    g_string_free(req, TRUE);

    snprintf(buffer, sizeof(buffer),
             "HTTP/1.0 200 OK\r\n"
             "Server: BaseHTTP/0.3 Python/2.6.5\r\n"
             "Date: Wed, 30 Mar 2011 19:46:35 GMT\r\n"
             "Content-type: text/plain\r\n"
             "Content-length: %ld\r\n"
             "\r\n", strlen(config_file));
    ret = write(rfd, buffer, strlen(buffer));
    g_assert_cmpint(ret, ==, strlen(buffer));

    printf("sending response\n");

    ret = write(rfd, config_file, strlen(config_file));
    g_assert_cmpint(ret, ==, strlen(config_file));

    printf("sent response\n");
    close(rfd);

    ret = waitpid(pid, &status, 0);
    g_assert(ret == pid);
    g_assert(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 3);

    close(rfd);
    close(fd);
}

static void test_debian_ppc(gconstpointer data)
{
    const char *distro = data;
    char image[1024];
    char iso[1024];

    snprintf(image, sizeof(image), "/tmp/debian-%s.img", distro);
    snprintf(iso, sizeof(iso), "%s/isos/debian-%s-DVD-1.iso",
             getenv("HOME"), distro);

    test_image("ppc64-softmmu/qemu-system-ppc64 -M pseries ",
               image, iso, "/install/powerpc64/vmlinux",
               "/install/powerpc64/initrd.gz",
               "priority=critical locale=en_US "
               "url=http://wiki.qemu.org/download/test.ks console=ttyS0",
               preseed, true);
}

static void test_ubuntu(gconstpointer data)
{
    const char *distro = data;
    char image[1024];
    char iso[1024];

    snprintf(image, sizeof(image), "/tmp/ubuntu-%s.img", distro);
    snprintf(iso, sizeof(iso), "%s/isos/ubuntu-%s.iso", getenv("HOME"), distro);

    test_image("x86_64-softmmu/qemu-system-x86_64 -enable-kvm",
               image, iso, "/install/vmlinuz", "/install/initrd.gz",
               "priority=critical locale=en_US "
               "url=http://10.0.2.1/server.cfg console=ttyS0",
               preseed, false);
}

static char *create_cor_iso(const char *url)
{
    char *base_path;
    const char *base_name;
    char *image_file;
    int status;

    base_path = g_strdup_printf("%s/.qemu-test-cache", getenv("HOME"));
    if (access(base_path, F_OK) == -1 && mkdir(base_path, 0755) == -1) {
        fprintf(stderr, "Could not create `%s'\n", base_path);
        return NULL;
    }

    base_name = strrchr(url, '/');
    g_assert(base_name != NULL && *base_name);

    base_name++;

    image_file = g_strdup_printf("%s/%s.qed", base_path, base_name);

    if (access(image_file, F_OK) == -1) {
        status = systemf("./qemu-img create -f qed "
                         "-o backing_file=\"%s\",backing_fmt=raw %s",
                         url, image_file);
        g_assert(status == 0);
    }

    return image_file;
}

static void test_fedora_dvd(gconstpointer data)
{
    const char *distro = data;
    char image[1024];
    char *iso;
    const char *url;
    int version = atoi(distro);

    url = g_strdup_printf("http://archives.fedoraproject.org/pub/archive/fedora/linux/releases/%d/Fedora/x86_64/iso/Fedora-%s.iso:size=3520802816:",
                          version, distro);

    iso = create_cor_iso(url);

    snprintf(image, sizeof(image), "/tmp/fedora-%s.img", distro);

    test_image("x86_64-softmmu/qemu-system-x86_64 -enable-kvm",
               image, iso, "/isolinux/vmlinuz", "/isolinux/initrd.img",
               "stage2=hd:LABEL=\"Fedora\" "
               "ks=http://wiki.qemu.org/download/test.ks console=ttyS0",
               ks, false);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_data_func("/fedora/8/x86_64", "8-x86_64-DVD", test_fedora_dvd);
    g_test_add_data_func("/fedora/11/x86_64", "11-x86_64-DVD", test_fedora_dvd);
    g_test_add_data_func("/fedora/14/x86_64", "14-x86_64-DVD", test_fedora_dvd);

    g_test_add_data_func("/ubuntu/9.10/server/amd64", "9.10-server-amd64", test_ubuntu);
    g_test_add_data_func("/ubuntu/10.04.2/server/amd64", "10.04.2-server-amd64", test_ubuntu);
    g_test_add_data_func("/ubuntu/10.10/server/amd64", "10.10-server-amd64", test_ubuntu);
    g_test_add_data_func("/debian/6.0.1a/powerpc", "6.0.1a-powerpc", test_debian_ppc);

    g_test_run();

    return 0;
}
