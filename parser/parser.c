/*
 * (C) Copyright 2015-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>
#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "util.h"
#include "swupdate.h"
#include "parselib.h"
#include "parsers.h"
#include "swupdate_dict.h"
#include "lua_util.h"

#define MODULE_NAME	"PARSER"

#define NODEROOT (!strlen(CONFIG_PARSERROOT) ? \
			"software" : CONFIG_PARSERROOT)

#if defined(CONFIG_LIBCONFIG) || defined(CONFIG_JSON)

typedef int (*parse_element)(parsertype p,
		void *cfg, void *start, const char **nodes,
		struct swupdate_cfg *swcfg, lua_State *L);

static bool path_append(const char **nodes, const char *field)
{
	unsigned int count = 0;

	count = count_string_array(nodes);

	if (count >= MAX_PARSED_NODES)
		return false;

	nodes[count++] = field;
	nodes[count] = NULL;

	return true;
}

static void *find_node_and_path(parsertype p, void *root, const char *field,
			struct swupdate_cfg *swcfg, const char **nodes)
{
	struct hw_type *hardware;
	void *node = NULL;
	int i;

	if (!field)
		return NULL;

	hardware = &swcfg->hw;

	for (i = 0; i < 4; i++) {
		nodes[0] = NULL;
		switch(i) {
		case 0:
	        	if (strlen(swcfg->running_mode) && strlen(swcfg->software_set) &&
		        		strlen(hardware->boardname)) {
				nodes[0] = NODEROOT;
				nodes[1] = hardware->boardname;
				nodes[2] = swcfg->software_set;
				nodes[3] = swcfg->running_mode;
				nodes[4] = NULL;
			}
			break;
		case 1:
			/* try with software set and mode */
			if (strlen(swcfg->running_mode) && strlen(swcfg->software_set)) {
				nodes[0] = NODEROOT;
				nodes[1] = swcfg->software_set;
				nodes[2] = swcfg->running_mode;
				nodes[3] = NULL;
			}
			break;
		case 2:
			/* Try with board name */
			if (strlen(hardware->boardname)) {
				nodes[0] = NODEROOT;
				nodes[1] = hardware->boardname;
				nodes[2] = NULL;
			}
			break;
		case 3:
			/* Fall back without board entry */
			nodes[0] = NODEROOT;
			nodes[1] = NULL;
			break;
		}

		/*
		 * If conditions are not set,
		 * skip to the next option
		 */
		if (!nodes[0])
			continue;

		/*
		 * The first find_root() search for
		 * the root element from board, selection
		 * The second one starts from root and follow the tree
		 * to search for element
		 */
		if (find_root(p, root, nodes)) {
			if (!path_append(nodes, field))
				return NULL;
			node = find_root(p, root, nodes);

			if (node) {
				return node;
			}
		}
	}

	return NULL;
}

static int parser_follow_link(parsertype p, void *cfg, void *elem,
				const char **nodes, struct swupdate_cfg *swcfg,
				parse_element fn, lua_State *L)
{
	const char *ref;
	void *link;
	char **tmp = NULL;	/* to store temporary link path */
	const char **linknodes;
	int result = 0;

	ref = get_field_string(p, elem, "ref");
	linknodes = (const char **)calloc(MAX_PARSED_NODES, sizeof(*nodes));
	if (!linknodes)
		return -ENOMEM;
	TRACE("Link found, following %s", ref);
	for (int j = 0; j < count_string_array(nodes); j++) {
		linknodes[j] = nodes[j];
	}
	if (!set_find_path(linknodes, ref, tmp)) {
		free(linknodes);
		return -1;
	}

	link = find_root(p, cfg, linknodes);
	if (link) {
		result = fn(p, cfg, link, linknodes, swcfg, L);
	}
	free_string_array(tmp);
	free(linknodes);
	return result;
}

static void *find_node(parsertype p, void *root, const char *field,
			struct swupdate_cfg *swcfg)
{
	const char **nodes;
	void *node = NULL;

	if (!field)
		return NULL;

	nodes = (const char **)calloc(MAX_PARSED_NODES, sizeof(*nodes));
	if (!nodes)
		return NULL;

	node = find_node_and_path(p, root, field, swcfg, nodes);
	free(nodes);

	return node;
}

static bool get_common_fields(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{

	void *setting;

	if((setting = find_node(p, cfg, "version", swcfg)) == NULL) {
		ERROR("Missing version in configuration file");
		return false;
	}

	GET_FIELD_STRING(p, setting, NULL, swcfg->version);
	TRACE("Version %s", swcfg->version);

	if((setting = find_node(p, cfg, "description", swcfg)) != NULL) {
		GET_FIELD_STRING(p, setting, NULL, swcfg->description);
		TRACE("Description %s", swcfg->description);
	}

	if(swcfg->globals.no_state_marker) {
		swcfg->bootloader_state_marker = false;
	} else {
		swcfg->bootloader_state_marker = true;
		if((setting = find_node(p, cfg, "bootloader_state_marker", swcfg)) != NULL) {
			get_field(p, setting, NULL, &swcfg->bootloader_state_marker);
			TRACE("Setting bootloader state marker: %s",
			      swcfg->bootloader_state_marker == true ? "true" : "false");
		}
	}

	if(swcfg->globals.no_transaction_marker) {
		swcfg->bootloader_transaction_marker = false;
	} else {
		swcfg->bootloader_transaction_marker = true;
		if((setting = find_node(p, cfg, "bootloader_transaction_marker", swcfg)) != NULL) {
			get_field(p, setting, NULL, &swcfg->bootloader_transaction_marker);
			TRACE("Setting bootloader transaction marker: %s",
			      swcfg->bootloader_transaction_marker == true ? "true" : "false");
		}
	}

	if ((setting = find_node(p, cfg, "output", swcfg)) != NULL) {
		if (!strlen(swcfg->output)) {
			TRACE("Output file set but not enabled with -o, ignored");
		} else {
			GET_FIELD_STRING(p, setting, NULL, swcfg->output);
			get_field(p, setting, NULL, &swcfg->output);
			TRACE("Incoming SWU stored : %s", swcfg->output);
		}
	}

	return true;
}

static void add_properties_cb(const char *name, const char *value, void *data)
{
	struct img_type *image = (struct img_type *)data;

	if (!name || !value)
		return;

	TRACE("\t\tProperty %s: %s", name, value);
	if (dict_insert_value(&image->properties, (char *)name, (char *)value))
		ERROR("Property not stored, skipping...");
}

static void add_properties(parsertype p, void *node, struct img_type *image)
{
	void *properties;

	properties = get_child(p, node, "properties");
	if (properties) {
		TRACE("Found properties for %s:", image->fname);

		iterate_field(p, properties, add_properties_cb, image);
	}
}

#ifdef CONFIG_HW_COMPATIBILITY
/*
 * Check if the software can run on the hardware
 */
static int parse_hw_compatibility(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *setting, *hw;
	int count, i;
	char s[SWUPDATE_GENERAL_STRING_SIZE];
	struct hw_type *hwrev;

	setting = find_node(p, cfg, "hardware-compatibility", swcfg);
	if (setting == NULL) {
		ERROR("HW compatibility not found");
		return -1;
	}

	count = get_array_length(p, setting);

	for(i = 0; i < count; ++i) {
		hw = get_elem_from_idx(p, setting, i);

		if (!hw)
			continue;

		s[0] = '\0';
		GET_FIELD_STRING(p, hw, NULL, s);
		if (!strlen(s))
			continue;

		hwrev = (struct hw_type *)calloc(1, sizeof(struct hw_type));
		if (!hwrev) {
			ERROR("No memory: malloc failed");
			return -1;
		}

		strlcpy(hwrev->revision, s, sizeof(hwrev->revision));
		LIST_INSERT_HEAD(&swcfg->hardware, hwrev, next);
		TRACE("Accepted Hw Revision : %s", hwrev->revision);
	}

	return 0;
}
#else
static int parse_hw_compatibility(parsertype __attribute__ ((__unused__))p,
		void __attribute__ ((__unused__))  *cfg,
		struct swupdate_cfg __attribute__ ((__unused__)) *swcfg)
{
	return 0;
}
#endif

static int is_image_installed(struct swver *sw_ver_list,
                              struct img_type *img)
{
    struct sw_version *swver;

    if (!sw_ver_list)
        return false;

    if (!strlen(img->id.name) || !strlen(img->id.version) ||
        !img->id.install_if_different)
        return false;

    LIST_FOREACH(swver, sw_ver_list, next) {
        /*
         * Check if name and version are identical
         */
        if (!strncmp(img->id.name, swver->name, sizeof(img->id.name)) &&
            !compare_versions(img->id.version, swver->version)) {
            TRACE("%s(%s) already installed, skipping...",
                  img->id.name,
                  img->id.version);

            return true;
        }
    }

    return false;
}

static int is_image_higher(struct swver *sw_ver_list,
                           struct img_type *img)
{
    struct sw_version *swver;

    if (!sw_ver_list)
        return false;

    if (!strlen(img->id.name) || !strlen(img->id.version) ||
        !img->id.install_if_higher)
        return false;

    LIST_FOREACH(swver, sw_ver_list, next) {
        const char* current_version = swver->version;
        const char* proposed_version = img->id.version;

        /*
         * Check if name are identical and the new version is lower
         * or equal.
         */
        if (!strncmp(img->id.name, swver->name, sizeof(img->id.name)) &&
            (compare_versions(proposed_version, current_version) <= 0)) {
            TRACE("%s(%s) has a higher or same version installed, skipping...",
                  img->id.name,
                  img->id.version);

            return true;
        }
    }

    return false;
}

static int run_embscript(parsertype p, void *elem, struct img_type *img,
			 lua_State *L, const char *embscript)
{
	const char *embfcn;
	if (!embscript)
		return 0;
	if (!exist_field_string(p, elem, "hook"))
		return 0;
	embfcn = get_field_string(p, elem, "hook");
	return lua_parser_fn(L, embfcn, img);
}

static int parse_common_attributes(parsertype p, void *elem, struct img_type *image, struct swupdate_cfg *cfg)
{
	char seek_str[MAX_SEEK_STRING_SIZE];
	const char* compressed;

	/*
	 * GET_FIELD_STRING does not touch the passed string if it is not
	 * found, be sure that it is empty
	 */
	seek_str[0] = '\0';

	GET_FIELD_STRING(p, elem, "name", image->id.name);
	GET_FIELD_STRING(p, elem, "version", image->id.version);
	GET_FIELD_STRING(p, elem, "filename", image->fname);
	GET_FIELD_STRING(p, elem, "path", image->path);
	GET_FIELD_STRING(p, elem, "volume", image->volname);
	GET_FIELD_STRING(p, elem, "device", image->device);
	GET_FIELD_STRING(p, elem, "mtdname", image->mtdname);
	GET_FIELD_STRING(p, elem, "filesystem", image->filesystem);
	GET_FIELD_STRING(p, elem, "type", image->type);
	GET_FIELD_STRING(p, elem, "offset", seek_str);
	GET_FIELD_STRING(p, elem, "data", image->type_data);
	get_hash_value(p, elem, image->sha256);

	/* convert the offset handling multiplicative suffixes */
	image->seek = ustrtoull(seek_str, 0);
	if (errno){
		ERROR("offset argument: ustrtoull failed");
		return -1;
	}

	if ((compressed = get_field_string(p, elem, "compressed")) != NULL) {
		if (!strcmp(compressed, "zlib")) {
			image->compressed = COMPRESSED_ZLIB;
		} else if (!strcmp(compressed, "zstd")) {
			image->compressed = COMPRESSED_ZSTD;
		} else {
			ERROR("compressed argument: '%s' unknown", compressed);
			return -1;
		}
	} else {
		get_field(p, elem, "compressed", &image->compressed);
	}
	get_field(p, elem, "installed-directly", &image->install_directly);
	get_field(p, elem, "preserve-attributes", &image->preserve_attributes);
	get_field(p, elem, "install-if-different", &image->id.install_if_different);
	get_field(p, elem, "install-if-higher", &image->id.install_if_higher);
	get_field(p, elem, "encrypted", &image->is_encrypted);
	get_field(p, elem, "execute-when-no-images", &image->execute_when_no_images);
	GET_FIELD_STRING(p, elem, "ivt", image->ivt_ascii);

	if (is_image_installed(&cfg->installed_sw_list, image)) {
		image->skip = SKIP_SAME;
	} else if (is_image_higher(&cfg->installed_sw_list, image)) {
		image->skip = SKIP_HIGHER;
	} else {
		image->skip = SKIP_NONE;
	}

	return 0;
}

static int _parse_partitions(parsertype p, void *cfg, void *setting, const char **nodes, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *elem;
	int count, i, skip, err;
	struct img_type *partition;

	if (setting == NULL)
		return 0;

	count = get_array_length(p, setting);
	/*
	 * Parse in reverse order, so that the partitions are processed
	 * by LIST_HEAD() in the same order as they are found in sw-description
	 */
	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if (exist_field_string(p, elem, "ref")) {
			err = parser_follow_link(p, cfg, elem, nodes, swcfg, _parse_partitions, L);
			if (err)
				return err;
			continue;
		}

		partition = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!partition) {
			ERROR("No memory: malloc failed");
			return -ENOMEM;
		}
		if (parse_common_attributes(p, elem, partition, swcfg) < 0) {
			free_image(partition);
			return -1;
		}
		GET_FIELD_STRING(p, elem, "name", partition->volname);

		if (!strlen(partition->type))
			strlcpy(partition->type, "ubipartition", sizeof(partition->type));
		partition->is_partitioner = 1;

		partition->provided = 1;

		if ((!strlen(partition->volname) && !strcmp(partition->type, "ubipartition")) ||
				!strlen(partition->device)) {
			ERROR("Partition incompleted in description file");
			free_image(partition);
			return -1;
		}

		get_field(p, elem, "size", &partition->partsize);

		add_properties(p, elem, partition);

		skip = run_embscript(p, elem, partition, L, swcfg->embscript);
		if (skip < 0) {
			free_image(partition);
			return -1;
		}
		TRACE("Partition: %s new size %lld bytes",
			!strcmp(partition->type, "ubipartition") ? partition->volname : partition->device,
			partition->partsize);

		LIST_INSERT_HEAD(&swcfg->images, partition, next);
	}

	return 0;
}

static int parse_partitions(parsertype p, void *cfg, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *setting;
	const char *nodes[MAX_PARSED_NODES];

	setting = find_node_and_path(p, cfg, "partitions", swcfg, nodes);

	if (!setting)
		return 0;

	return _parse_partitions(p, cfg, setting, nodes, swcfg, L);
}

static int _parse_scripts(parsertype p, void *cfg, void *setting, const char **nodes, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *elem;
	int count, i, skip, err;
	struct img_type *script;

	if (setting == NULL)
		return 0;

	count = get_array_length(p, setting);
	/*
	 * Scan the scripts in reserve order to maintain
	 * the same order using LIST_INSERT_HEAD
	 */
	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if (exist_field_string(p, elem, "ref")) {
			err = parser_follow_link(p, cfg, elem, nodes, swcfg, _parse_scripts, L);
			if (err)
				return err;
			continue;
		}
		/*
		 * Check for filename field
		 */
		if(!(exist_field_string(p, elem, "filename")))
			TRACE("Script entry without filename field.");

		script = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!script) {
			ERROR( "No memory: malloc failed");
			return -ENOMEM;
		}

		script->execute_when_no_images = true;

		if (parse_common_attributes(p, elem, script, swcfg) < 0) {
			free_image(script);
			return -1;
		}

		/* Scripts as default call the Lua interpreter */
		if (!strlen(script->type)) {
			strcpy(script->type, "lua");
		}
		script->is_script = 1;

		add_properties(p, elem, script);

		skip = run_embscript(p, elem, script, L, swcfg->embscript);
		if (skip < 0) {
			free_image(script);
			return -1;
		}

		TRACE("%s Script: %s",
			skip ? "Skip" : "Found",
			script->fname);

		if (skip || script->skip != SKIP_NONE) {
			free_image(script);
			continue;
		}

		LIST_INSERT_HEAD(&swcfg->scripts, script, next);
	}
	return 0;
}

static int parse_scripts(parsertype p, void *cfg, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *setting;
	const char *nodes[MAX_PARSED_NODES];

	setting = find_node_and_path(p, cfg, "scripts", swcfg, nodes);

	if (!setting)
		return 0;

	return _parse_scripts(p, cfg, setting, nodes, swcfg, L);
}

static int _parse_bootloader(parsertype p, void *cfg, void *setting, const char **nodes, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *elem;
	int count, i, skip, err;
	struct img_type *script;
	struct img_type dummy;

	if (setting == NULL) {
		return 0;
	}

	count = get_array_length(p, setting);

	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if (exist_field_string(p, elem, "ref")) {
			err = parser_follow_link(p, cfg, elem, nodes, swcfg, _parse_bootloader, L);
			if (err)
				return err;
			continue;
		}

		memset(&dummy, 0, sizeof(dummy));

		/*
		 * Check for mandatory field
		 */
		if(exist_field_string(p, elem, "name")) {
			/*
			 * Call directly get_field_string with size 0
			 * to let allocate the place for the strings
			 */
			GET_FIELD_STRING(p, elem, "name", dummy.id.name);
			GET_FIELD_STRING(p, elem, "value", dummy.id.version);
			skip = run_embscript(p, elem, &dummy, L, swcfg->embscript);
			if (skip < 0) {
				return -1;
			}
			if (!skip) {
				dict_set_value(&swcfg->bootloader, dummy.id.name, dummy.id.version);
				TRACE("Bootloader var: %s = %s",
					dummy.id.name,
					dict_get_value(&swcfg->bootloader, dummy.id.name));
			}
			continue;
		}

		/*
		 * Check if it is a bootloader script
		 */
		if(!(exist_field_string(p, elem, "filename"))) {
			TRACE("bootloader entry is neither a script nor name/value.");
			continue;
		}
		script = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!script) {
			ERROR( "No memory: malloc failed");
			return -ENOMEM;
		}

		if (parse_common_attributes(p, elem, script, swcfg) < 0) {
			free_image(script);
			return -1;
		}

		script->is_script = 1;

		skip = run_embscript(p, elem, script, L, swcfg->embscript);
		if (skip != 0 || script->skip != SKIP_NONE) {
			free_image(script);
			if (skip < 0)
				return -1;
			continue;
		}

		LIST_INSERT_HEAD(&swcfg->bootscripts, script, next);

		TRACE("Found U-Boot Script: %s",
			script->fname);
	}

	return 0;
}

static int parse_bootloader(parsertype p, void *cfg, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *setting;
	const char *nodes[MAX_PARSED_NODES];

	setting = find_node_and_path(p, cfg, "bootenv", swcfg, nodes);

	if (!setting) {
		setting = find_node_and_path(p, cfg, "uboot", swcfg, nodes);
	}
	if (!setting)
		return 0;

	return _parse_bootloader(p, cfg, setting, nodes, swcfg, L);
}

static int _parse_images(parsertype p, void *cfg, void *setting, const char **nodes, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *elem;
	int count, i, skip, err;
	struct img_type *image;

	if (setting == NULL)
		return 0;

	count = get_array_length(p, setting);

	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if (exist_field_string(p, elem, "ref")) {
			err = parser_follow_link(p, cfg, elem, nodes, swcfg, _parse_images, L);
			if (err)
				return err;
			continue;
		}
		/*
		 * Check for mandatory field
		 */
		if(!(exist_field_string(p, elem, "filename"))) {
			TRACE("Image entry without filename field, skipping..");
			continue;
		}

		image = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!image) {
			ERROR( "No memory: malloc failed");
			return -ENOMEM;
		}

		if (parse_common_attributes(p, elem, image, swcfg) < 0) {
			free_image(image);
			return -1;
		}

		/* if the handler is not explicit set, try to find the right one */
		if (!strlen(image->type)) {
			if (strlen(image->volname))
				strcpy(image->type, "ubivol");
			else if (strlen(image->device))
				strcpy(image->type, "raw");
		}

		add_properties(p, elem, image);

		image->bootloader = &swcfg->bootloader;

		skip = run_embscript(p, elem, image, L, swcfg->embscript);
		if (skip < 0) {
			free_image(image);
			return -1;
		}

		TRACE("%s %sImage%s%s%s%s: %s in %s : %s for handler %s%s%s",
			skip ? "Skip" : "Found",
			image->compressed ? "compressed " : "",
			strlen(image->id.name) ? " " : "", image->id.name,
			strlen(image->id.version) ? " " : "", image->id.version,
			image->fname,
			strlen(image->volname) ? "volume" : "device",
			strlen(image->volname) ? image->volname :
			strlen(image->path) ? image->path : image->device,
			strlen(image->type) ? image->type : "NOT FOUND",
			image->install_directly ? " (installed from stream)" : "",
			(strlen(image->id.name) && (image->id.install_if_different ||
						    image->id.install_if_higher) &&
			 				(skip || image->skip != SKIP_NONE)) ?
							" SKIPPED" : ""
			);
		if (skip || image->skip != SKIP_NONE) {
			free_image(image);
			continue;
		}

		LIST_INSERT_HEAD(&swcfg->images, image, next);
	}

	return 0;
}

static int parse_images(parsertype p, void *cfg, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *setting;
	const char *nodes[MAX_PARSED_NODES];

	setting = find_node_and_path(p, cfg, "images", swcfg, nodes);

	if (!setting)
		return 0;

	return _parse_images(p, cfg, setting, nodes, swcfg, L);
}

static int _parse_files(parsertype p, void *cfg, void *setting, const char **nodes, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *elem;
	int count, i, skip, err;
	struct img_type *file;

	if (setting == NULL)
		return 0;

	count = get_array_length(p, setting);

	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if (exist_field_string(p, elem, "ref")) {
			err = parser_follow_link(p, cfg, elem, nodes, swcfg, _parse_files, L);
			if (err)
				return err;
			continue;
		}
		/*
		 * Check for mandatory field
		 */
		if(!(exist_field_string(p, elem, "filename"))) {
			TRACE("File entry without filename field, skipping..");
			continue;
		}

		file = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!file) {
			ERROR( "No memory: malloc failed");
			return -ENOMEM;
		}

		if (parse_common_attributes(p, elem, file, swcfg) < 0) {
			free_image(file);
			return -1;
		}

		if (!strlen(file->type)) {
			strcpy(file->type, "rawfile");
		}

		add_properties(p, elem, file);

		file->bootloader = &swcfg->bootloader;

		skip = run_embscript(p, elem, file, L, swcfg->embscript);
		if (skip < 0) {
			free_image(file);
			return -1;
		}

		TRACE("%s %sFile%s%s%s%s: %s --> %s (%s)%s",
			skip ? "Skip" : "Found",
			file->compressed ? "compressed " : "",
			strlen(file->id.name) ? " " : "", file->id.name,
			strlen(file->id.version) ? " " : "", file->id.version,
			file->fname,
			file->path,
			strlen(file->device) ? file->device : "ROOTFS",
			(strlen(file->id.name) && file->id.install_if_different) ?
					"; Version must be checked" : "");

		if (skip || file->skip != SKIP_NONE) {
			free_image(file);
			continue;
		}

		LIST_INSERT_HEAD(&swcfg->images, file, next);
	}

	return 0;
}

static int parse_files(parsertype p, void *cfg, struct swupdate_cfg *swcfg, lua_State *L)
{
	void *setting;
	const char *nodes[MAX_PARSED_NODES];

	setting = find_node_and_path(p, cfg, "files", swcfg, nodes);

	if (!setting)
		return 0;

	return _parse_files(p, cfg, setting, nodes, swcfg, L);
}

static int parser(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *scriptnode;
	lua_State *L = NULL;
	int ret;

	swcfg->embscript = NULL;
	scriptnode = find_node(p, cfg, "embedded-script", swcfg);
	if (scriptnode) {
		TRACE("Getting script");
		swcfg->embscript = get_field_string(p, scriptnode, NULL);
	}

	if (swcfg->embscript) {
		TRACE("Found Lua Software:\n%s", swcfg->embscript);
		L = lua_parser_init(swcfg->embscript, &swcfg->bootloader);
		if (!L) {
			ERROR("Required embedded script that cannot be loaded");
			return -1;
		}
	}
	get_hw_revision(&swcfg->hw);

	/* Now parse the single elements */
	ret = parse_hw_compatibility(p, cfg, swcfg) ||
		parse_files(p, cfg, swcfg, L) ||
		parse_images(p, cfg, swcfg, L) ||
		parse_scripts(p, cfg, swcfg, L) ||
		parse_bootloader(p, cfg, swcfg, L);

	/*
	 * Move the partitions at the beginning to be processed
	 * before other images
	 */
	parse_partitions(p, cfg, swcfg, L);

	if (L)
		lua_parser_exit(L);

	if (LIST_EMPTY(&swcfg->images) &&
	    LIST_EMPTY(&swcfg->scripts) &&
	    LIST_EMPTY(&swcfg->bootloader)) {
		ERROR("Found nothing to install");
		return -1;
	}

	return ret;
}
#endif

#ifdef CONFIG_LIBCONFIG
int parse_cfg (struct swupdate_cfg *swcfg, const char *filename)
{
	config_t cfg;
	parsertype p = LIBCFG_PARSER;
	int ret;

	memset(&cfg, 0, sizeof(cfg));
	config_init(&cfg);

	/* Read the file. If there is an error, report it and exit. */
	DEBUG("Parsing config file %s", filename);
	if(config_read_file(&cfg, filename) != CONFIG_TRUE) {
		printf("%s ", config_error_file(&cfg));
		printf("%d ", config_error_line(&cfg));
		printf("%s ", config_error_text(&cfg));

		fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg),
			config_error_line(&cfg), config_error_text(&cfg));
		config_destroy(&cfg);
		ERROR(" ..exiting");
		return -1;
	}

	if (!get_common_fields(p, &cfg, swcfg))
		return -1;

	ret = parser(p, &cfg, swcfg);

	config_destroy(&cfg);

	return ret;
}
#else
int parse_cfg (struct swupdate_cfg __attribute__ ((__unused__)) *swcfg,
		const char __attribute__ ((__unused__)) *filename)
{
	return -1;
}
#endif

#ifdef CONFIG_JSON
int parse_json(struct swupdate_cfg *swcfg, const char *filename)
{
	int fd, ret;
	struct stat stbuf;
	unsigned int size;
	char *string;
	json_object *cfg;
	parsertype p = JSON_PARSER;

	DEBUG("Parsing config file %s", filename);
	/* Read the file. If there is an error, report it and exit. */
	ret = stat(filename, &stbuf);

	if (ret)
		return -EBADF;

	size = stbuf.st_size;
	string = (char *)malloc(size);
	if (!string)
		return -ENOMEM;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		free(string);
		return -EBADF;
	}

	ret = read(fd, string, size);
	close(fd);

	cfg = json_tokener_parse(string);
	if (!cfg) {
		ERROR("JSON File corrupted");
		free(string);
		return -1;
	}

	if (!get_common_fields(p, cfg, swcfg)) {
		free(string);
		return -1;
	}

	ret = parser(p, cfg, swcfg);

	json_object_put(cfg);

	free(string);

	return ret;
}
#else
int parse_json(struct swupdate_cfg __attribute__ ((__unused__)) *swcfg,
		const char __attribute__ ((__unused__)) *filename)
{
	return -1;
}
#endif
