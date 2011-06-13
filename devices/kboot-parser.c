#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "parser.h"
#include "params.h"

#define buf_size 1024

static const char *devpath;

static int param_is_ignored(const char *param)
{
	static const char *ignored_options[] =
		{ "message", "timeout", "default", NULL };
	const char **str;

	for (str = ignored_options; *str; str++)
		if (streq(*str, param))
			return 1;
	return 0;
}

/**
 * Splits a name=value pair, with value terminated by @term (or nul). if there
 * is no '=', then only the value is populated, and *name is set to NULL. The
 * string is modified in place.
 *
 * Returns the next byte to process, or null if we've hit the end of the
 * string.
 *
 * */
static char *get_param_pair(char *str, char **name_out, char **value_out,
		char terminator)
{
	char *sep, *tmp, *name, *value;

	/* terminate the value */
	tmp = strchr(str, terminator);
	if (tmp)
		*tmp = 0;
	else
		tmp = NULL;

	sep = strchr(str, '=');
	if (!sep) {
		*name_out = NULL;
		*value_out = str;
		return tmp ? tmp + 1 : NULL;
	}

	/* terminate the name */
	*sep = 0;

	/* remove leading spaces */
	for (name = str; isspace(*name); name++);
	for (value = sep + 1; isspace(*value); value++);

	/* .. and trailing ones.. */
	for (sep--; isspace(*sep); sep--)
		*sep = 0;
	for (sep = value + strlen(value) - 1; isspace(*sep); sep--)
		*sep = 0;

	*name_out = name;
	*value_out = value;

	return tmp ? tmp + 1 : NULL;
}

struct global_option {
	char *name;
	char *value;
};


static struct global_option global_options[] = {
	{ .name = "root" },
	{ .name = "initrd" },
	{ .name = "video" },
	{ .name = NULL }
};

/*
 * Check if an option (name=value) is a global option. If so, store it in
 * the global options table, and return 1. Otherwise, return 0.
 */
static int check_for_global_option(const char *name, const char *value)
{
	int i;

	for (i = 0; global_options[i].name ;i++) {
		if (!strcmp(name, global_options[i].name)) {
			global_options[i].value = strdup(value);
			return 1;
		}
	}
	return 0;
}

static char *get_global_option(const char *name)
{
	int i;

	for (i = 0; global_options[i].name ;i++)
		if (!strcmp(name, global_options[i].name))
			return global_options[i].value;

	return NULL;
}

static int parse_option(struct boot_option *opt, char *config)
{
	char *pos, *name, *value, *root, *initrd, *cmdline, *tmp;

	root = initrd = cmdline = NULL;

	/* remove quotes around the value */
	while (*config == '"' || *config == '\'')
		config++;

	pos = config + strlen(config) - 1;
	while (*pos == '"' || *pos == '\'')
		*(pos--) = 0;

	if (!strlen(pos))
		return 0;

	pos = strchr(config, ' ');

	/* if there's no space, it's only a kernel image with no params */
	if (!pos) {
		opt->boot_image_file = resolve_path(config, devpath);
		opt->description = strdup(config);
		return 1;
	}

	*pos = 0;
	opt->boot_image_file = resolve_path(config, devpath);

	cmdline = malloc(buf_size);
	*cmdline = 0;

	for (pos++; pos;) {
		pos = get_param_pair(pos, &name, &value, ' ');

		if (!name) {
			strcat(cmdline, " ");
			strcat(cmdline, value);

		} else if (streq(name, "initrd")) {
			initrd = value;

		} else if (streq(name, "root")) {
			root = value;

		} else {
			strcat(cmdline, " ");
			*(value - 1) = '=';
			strcat(cmdline, name);
		}
	}

	if (!root)
		root = get_global_option("root");
	if (!initrd)
		initrd = get_global_option("initrd");

	if (initrd) {
		asprintf(&tmp, "initrd=%s %s", initrd, cmdline);
		free(cmdline);
		cmdline = tmp;

		opt->initrd_file = resolve_path(initrd, devpath);
	}

	if (root) {
		asprintf(&tmp, "root=%s %s", root, cmdline);
		free(cmdline);
		cmdline = tmp;

	} else if (initrd) {
		/* if there's an initrd but no root, fake up /dev/ram0 */
		asprintf(&tmp, "root=/dev/ram0 %s", cmdline);
		free(cmdline);
		cmdline = tmp;
	}

	pb_log("kboot cmdline: %s\n", cmdline);
	opt->boot_args = cmdline;

	asprintf(&opt->description, "%s %s",
			config, opt->boot_args);

	return 1;
}

static void parse_buf(struct device *dev, char *buf)
{
	char *pos, *name, *value;
	int sent_device = 0;

	for (pos = buf; pos;) {
		struct boot_option opt;

		pos = get_param_pair(pos, &name, &value, '\n');

		pb_log("kboot param: '%s' = '%s'\n", name, value);

		if (name == NULL || param_is_ignored(name))
			continue;

		if (*name == '#')
			continue;

		if (check_for_global_option(name, value))
			continue;

		memset(&opt, 0, sizeof(opt));
		opt.name = strdup(name);

		if (parse_option(&opt, value))
			if (!sent_device++)
				add_device(dev);
			add_boot_option(&opt);

		free(opt.name);
	}
}

static int parse(const char *device)
{
	char *filepath, *buf;
	int fd, len, rc = 0;
	struct stat stat;
	struct device *dev;

	devpath = device;

	filepath = resolve_path("/etc/kboot.conf", devpath);

	fd = open(filepath, O_RDONLY);
	if (fd < 0)
		goto out_free_path;

	if (fstat(fd, &stat))
		goto out_close;

	buf = malloc(stat.st_size + 1);
	if (!buf)
		goto out_close;;

	len = read(fd, buf, stat.st_size);
	if (len < 0)
		goto out_free_buf;
	buf[len] = 0;

	dev = malloc(sizeof(*dev));
	memset(dev, 0, sizeof(*dev));
	dev->id = strdup(device);
	dev->icon_file = strdup(generic_icon_file(guess_device_type()));

	parse_buf(dev, buf);

	rc = 1;

out_free_buf:
	free(buf);
out_close:
	close(fd);
out_free_path:
	free(filepath);
	return rc;
}

struct parser kboot_parser = {
	.name = "kboot.conf parser",
	.priority = 98,
	.parse	  = parse
};
