/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#include <libarmadito/armadito.h>

#include "armadito-config.h"

#include "module_p.h"
#include "core/dir.h"
#include "string_p.h"

#include <assert.h>
#include <gmodule.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct module_manager {
	/* a GArray and not a GPtrArray because GArray can be automatically NULL terminated */
	GArray *modules;

	struct armadito *armadito;
};

/*
  is module copy really needed?
  module management modifies the a6o_module structure, namely the fields
  'status' and 'data'
  but this is safe even if the structure is in a dynamically loaded object
  (this has been checked in a small program).
  problem may arise if there are 2 instances of struct armadito, but this
  should not happen
  so for now, we copy
*/
static struct a6o_module *module_new(struct a6o_module *src, struct armadito *armadito)
{
	struct a6o_module *mod = (struct a6o_module *)calloc(1,sizeof(struct a6o_module));

	mod->init_fun = src->init_fun;
	mod->conf_table = src->conf_table;
	mod->post_init_fun = src->post_init_fun;
	mod->scan_fun = src->scan_fun;
	mod->close_fun = src->close_fun;
	mod->info_fun = src->info_fun;
	mod->supported_mime_types = src->supported_mime_types;
	mod->name = os_strdup(src->name);
	mod->size = src->size;
	mod->status = A6O_MOD_OK;
	mod->data = NULL;
	mod->armadito = armadito;

	if (mod->size > 0)
		mod->data = calloc(1,mod->size);

	return mod;
}

static void module_free(struct a6o_module *mod)
{
	free((void *)mod->name);

	/* shortcoming: module close() function should have been called before, otherwise module's internal data is not free'd correctly */
	if (mod->data != NULL)
		free(mod->data);

	free(mod);
}

static int module_load(const char *filename, struct a6o_module **pmodule, a6o_error **error)
{
	struct a6o_module *mod_loaded;
	GModule *g_mod;

	a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_DEBUG, "trying to load module object file %s", filename);

	g_mod = g_module_open(filename, G_MODULE_BIND_LAZY);

	/* this is not considered as an error: the module load path may contain */
	/* files that are not dynamic libraries, we simply ignore them */
	if (!g_mod) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "loading module object file %s failed", filename);

		*pmodule = NULL;
		return 0;
	}

	/* the module must export a "module" symbol that is a struct a6o_module */
	/* if the symbol is not found, then module cannot be loaded */
	if (!g_module_symbol(g_mod, "module", (gpointer *)&mod_loaded)) {
		a6o_error_set(error, ARMADITO_ERROR_MODULE_SYMBOL_NOT_FOUND, "symbol 'module' not found in file");

		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "symbol %s not found in file %s", "module", filename);

		*pmodule = NULL;

		return ARMADITO_ERROR_MODULE_SYMBOL_NOT_FOUND;
	}

	a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_INFO, "module %s loaded from file %s", mod_loaded->name, filename);

	*pmodule = mod_loaded;

	return 0;
}

struct module_manager *module_manager_new(struct armadito *armadito)
{
	struct module_manager *mm = malloc(sizeof(struct module_manager));

	mm->modules = g_array_new(TRUE, TRUE, sizeof(struct a6o_module *));
	mm->armadito = armadito;

	return mm;
}

void module_manager_free(struct module_manager *mm)
{
        struct a6o_module **modv;

	for (modv = module_manager_get_modules(mm); *modv != NULL; modv++)
	   module_free(*modv);

	g_array_free(mm->modules, TRUE);

	free(mm);
}

void module_manager_add(struct module_manager *mm, struct a6o_module *module)
{
	struct a6o_module *clone = module_new(module, mm->armadito);

	g_array_append_val(mm->modules, clone);
}

static int module_load_dirent_cb(const char *full_path, enum os_file_flag flags, int entry_errno, void *data)
{
	if (flags & FILE_FLAG_IS_PLAIN_FILE) {
		struct a6o_module *mod_loaded;
		a6o_error *error = NULL;

		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_DEBUG, "loading module from path %s flags %d", full_path, flags);

		if (!module_load(full_path, &mod_loaded, &error) && mod_loaded != NULL)
			module_manager_add((struct module_manager *)data, mod_loaded);

		if(error != NULL)
			free(error);
	}

	return 0;
}

int module_manager_load_path(struct module_manager *mm, const char *path, a6o_error **error)
{
	/* FIXME: for now, dirty stuff, do nothing with error */
	int ret = 0;

	a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_INFO, "loading modules from directory %s", path);

	ret = os_dir_map(path, 0, module_load_dirent_cb, mm);

	return ret;
}

/* apply `module_fun` to all modules that have an OK status */
/* continue if a module returns an error and return error */
static int module_manager_all(struct module_manager *mm, int (*module_fun)(struct a6o_module *, a6o_error **error), a6o_error **error)
{
	struct a6o_module **modv;
	int global_ret = 0;

	for (modv = module_manager_get_modules(mm); *modv != NULL; modv++) {
		struct a6o_module *mod = *modv;
		int mod_ret;

		/* module is not ok, do nothing */
		if (mod->status != A6O_MOD_OK)
			continue;

		mod_ret = (*module_fun)(mod, error);

		if (mod_ret)
			global_ret = mod_ret;
	}

	return global_ret;
}

static int module_init(struct a6o_module *mod, a6o_error **error)
{
	a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_DEBUG, "initializing module %s", mod->name);

	/* module has no init_fun, nothing else to do */
	if (mod->init_fun == NULL)
		return 0;

	/* call the init function */
	mod->status = (*mod->init_fun)(mod);

	/* everything's ok */
	if (mod->status != A6O_MOD_OK) {
		/* module init failed, set error and return NULL */
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "initialization error for module '%s'", mod->name);

		a6o_error_set(error, ARMADITO_ERROR_MODULE_INIT_FAILED, "initialization error for module");

		return ARMADITO_ERROR_MODULE_INIT_FAILED;
	}

	return 0;
}

int module_manager_init_all(struct module_manager *mm, a6o_error **error)
{
	return module_manager_all(mm, module_init, error);
}

static void module_conf_fun(const char *section, const char *key, struct a6o_conf_value *value, void *user_data)
{
	struct module_manager *mm = (struct module_manager *)user_data;
	struct a6o_module *mod;
	struct a6o_conf_entry *conf_entry;

	mod = module_manager_get_module_by_name(mm, section);
	if (mod == NULL) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "configuration: no module '%s'", section);
		return;
	}

	if (mod->conf_table == NULL)
		return;

	for(conf_entry = mod->conf_table; conf_entry->key != NULL; conf_entry++)
		if (!strcmp(conf_entry->key, key))
			break;

	if (conf_entry->key == NULL || conf_entry->conf_fun == NULL) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "configuration: no key '%s' for module '%s'", key, mod->name);
		return;
	}

	/* does the type in value match the type in the configuration entry? */
	if ((a6o_conf_value_get_type(value) & conf_entry->type) == 0) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "configuration: value type (%d) does not match declared type (%d) for key '%s' module '%s'", a6o_conf_value_get_type(value), conf_entry->type, key, mod->name);
		return;
	}

	if ((*conf_entry->conf_fun)(mod, key, value) != A6O_MOD_OK) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "configuration: cannot assign value to key '%s' for module '%s'", key, mod->name);
		return;
	}
}

int module_manager_configure_all(struct module_manager *mm, struct a6o_conf *conf, a6o_error **error)
{
	a6o_conf_apply(conf, module_conf_fun, mm);

	return 0;
}

static int module_post_init(struct a6o_module *mod, a6o_error **error)
{
	a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_DEBUG, "post-initializing module %s", mod->name);

	if (mod->post_init_fun == NULL)
		return 0;

	mod->status = (*mod->post_init_fun)(mod);

	if (mod->status != A6O_MOD_OK) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "post_init error for module '%s'", mod->name);

		a6o_error_set(error, ARMADITO_ERROR_MODULE_POST_INIT_FAILED, "post_init error for module");

		return ARMADITO_ERROR_MODULE_POST_INIT_FAILED;
	}

	return 0;
}

int module_manager_post_init_all(struct module_manager *mm, a6o_error **error)
{
	return module_manager_all(mm, module_post_init, error);
}

static int module_close(struct a6o_module *mod, a6o_error **error)
{
	/* module has no close_fun, do nothing */
	if (mod->close_fun == NULL)
		return 0;

	/* call the close function */
	mod->status = (*mod->close_fun)(mod);

	/* if close failed, return an error */
	if (mod->status != A6O_MOD_OK) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "close error for module '%s'", mod->name);

		a6o_error_set(error, ARMADITO_ERROR_MODULE_CLOSE_FAILED, "close error for module");

		return ARMADITO_ERROR_MODULE_CLOSE_FAILED;
	}

	return 0;
}

int module_manager_close_all(struct module_manager *mm, a6o_error **error)
{
	return module_manager_all(mm, module_close, error);
}

struct a6o_module **module_manager_get_modules(struct module_manager *mm)
{
	return (struct a6o_module **)mm->modules->data;
}

struct a6o_module *module_manager_get_module_by_name(struct module_manager *mm, const char *name)
{
	struct a6o_module **modv;

	for (modv = module_manager_get_modules(mm); *modv != NULL; modv++)
		if (!strcmp((*modv)->name, name))
			return *modv;

	return NULL;
}


#ifdef DEBUG
const char *module_debug(struct a6o_module *module)
{
	GString *s = g_string_new("");
	const char *ret;

	g_string_append_printf(s, "Module '%s':\n", module->name);
	g_string_append_printf(s, "  status     %d\n", module->status);
	g_string_append_printf(s, "  data       %p\n", module->data);
	g_string_append_printf(s, "  init       %p\n", module->init_fun);
	g_string_append_printf(s, "  post_init  %p\n", module->post_init_fun);
	g_string_append_printf(s, "  scan       %p\n", module->scan_fun);
	g_string_append_printf(s, "  close      %p\n", module->close_fun);

	if (module->conf_table != NULL) {
		struct a6o_conf_entry *p;

		g_string_append_printf(s, "  configuration:\n");
		for(p = module->conf_table; p->key != NULL; p++)
			g_string_append_printf(s, "    key %-20s conf %p\n", p->key, p->conf_fun);
	}

	ret = s->str;
	g_string_free(s, FALSE);

	return ret;
}
#endif
