#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>

#include <libpop.h>



void usage(void) {
	printf("usage: pop-ioctl\n"
	       "    -b pci     PCI slot\n"
	       "    -c cmd     regigster or unregister\n"
	       "    -s size    size of allocating p2pmem\n");
}

int main(int argc, char **argv) {

	int fd, ch, ret, cmd = 0;
	struct pop_p2pmem_reg reg;

	memset(&reg, 0, sizeof(reg));

	while((ch = getopt(argc, argv, "b:c:s:")) != -1) {
		switch(ch) {
		case 'b':
			ret = sscanf(optarg, "%x:%x:%x.%x",
				     &reg.domain, &reg.bus,
				     &reg.slot, &reg.func);
			if (ret < 4) {
				reg.domain = 0;
				ret = sscanf(optarg, "%x:%x.%x",
					     &reg.bus, &reg.slot, &reg.func);
			}
			if (ret < 3) {
				fprintf(stderr, "invalid pci %s\n", optarg);
				return -1;
			}
			break;

		case 'c':
			if (strncmp(optarg, "register", 8) == 0)
				cmd = POP_P2PMEM_REG;
			else if (strncmp(optarg, "unregister", 10) == 0)
				cmd = POP_P2PMEM_UNREG;
			else {
				fprintf(stderr, "invalid cmd %s\n", optarg);
				return -1;
			}
			break;

		case 's':
			reg.size = atoi(optarg);
			break;

		default:
			usage();
			return -1;
		}
	}

	if (cmd == 0) {
		fprintf(stderr, "-c cmd must be specified\n");
		return -1;
	}

	
	fd = open("/dev/pop/pop", O_RDWR);
	if (fd < 0) {
		perror("failed to open /dev/pop/pop");
		return fd;
	}

	ret = ioctl(fd, cmd, &reg);
	if (ret != 0) {
		perror("ioctl");
		return ret;
	}

	return 0;
}
