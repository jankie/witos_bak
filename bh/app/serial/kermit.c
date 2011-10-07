#include <loader.h>
#include <uart/ymodem.h>
#include <uart/kermit.h>
#include <flash/part.h>
#include <sysconf.h>
#include <getopt.h>

int main(int argc, char *argv[])
{
	int size, ret;
	struct loader_opt ld_opt;
	struct partition *part;
	char *arg;
	int opt;
	int flag = 0;

	size = 0;
	part = NULL;

	printf("%s loading ...", argv[0]);

	memset(&ld_opt, 0x0, sizeof(ld_opt));

	while ((opt = getopt(argc, argv, "m::", &arg)) != -1)
	{
		switch (opt)
		{
		case 'm':
			if (arg != NULL)
			{
				ret = string2value(arg, (u32 *)&ld_opt.load_addr);
				if (ret < 0)
				{
					printf("Input a invalied address!\n");
					return -EINVAL;
				}
			}
			flag = 1;
			break;

		default:
			break;
		}
	}

	if (flag == 0)
	{
		if ((part = part_open(PART_CURR, OP_RDWR)) == NULL)
		{
			return -EINVAL;
		}

		ld_opt.part = part;
	}

	if (strcmp(argv[0], "kermit") == 0)
	{
		size = kermit_load(&ld_opt);
	}

	if (strcmp(argv[0], "ymodem") == 0)
	{
		size = ymodem_load(&ld_opt);
	}

	if (flag == 0)
	{
		part_set_image(part, ld_opt.file_name, ld_opt.load_size);

		part_close(part);

		sysconf_save();
	}

	return size;
}