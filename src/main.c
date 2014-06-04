/*
 * main.c - Main program routines
 *
 * Copyright (c) 2014   A. Dilly
 *
 * AirCat is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * AirCat is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with AirCat.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>

#include "config_file.h"
#include "output.h"
#include "avahi.h"
#include "httpd.h"

#include "modules.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef CONFIG_PATH
	#define CONFIG_PATH "/etc/aircat/"
#endif

#ifndef VERSION
	#define VERSION "1.0.0"
#endif

#ifndef MODULES_PATH
	#define MODULES_PATH "/usr/lib/aircat/"
#endif

/* Common modules */
static struct output_handle *output = NULL;
static struct avahi_handle *avahi = NULL;
static struct httpd_handle *httpd = NULL;
static struct config_handle *config = NULL;
/* Modules */
static struct module_list *modules = NULL;

/* URLs */
struct url_table config_urls[];

/* Program args */
static char *config_file = NULL;	/* Alternative configuration file */
static int verbose = 0;			/* Verbosity */
static int stop_signal = 0;		/* Stop signal */

static void print_usage(const char *name)
{
	printf("Usage: %s [OPTIONS]\n"
		"\n"
		"Options:\n"
		"-c      --config=FILE        Use FILE as configuration file\n"
		"-h      --help               Print this usage and exit\n"
		"-v      --verbose            Active verbose output\n"
		"        --version            Print version and exit\n",
		 name);
}

static void print_version(void)
{
	printf("AirCat " VERSION "\n");
}

static void parse_opt(int argc, char * const argv[])
{
	int c;

	/* Get options */
	while(1)
	{
		int option_index = 0;
		static const char *short_options = "c:hv";
		static struct option long_options[] =
		{
			{"version",      no_argument,        0, 0},
			{"config",       required_argument,  0, 'c'},
			{"help",         no_argument,        0, 'h'},
			{"verbose",      no_argument,        0, 'v'},
			{0, 0, 0, 0}
		};

		/* Get next option */
		c = getopt_long (argc, argv, short_options, long_options, &option_index);
		if(c == EOF)
			break;

		/* Parse option */
		switch(c)
		{
			case 0:
				switch(option_index)
				{
					case 0:
						/* Version */
						print_version();
						exit(EXIT_SUCCESS);
						break;
				}
				break;
			case 'c':
				/* Config file */
				config_file = strdup(optarg);
				break;
			case 'v':
				/* Verbose */
				verbose = 1;
				break;
			case 'h':
				/* Help */
				print_usage(argv[0]);
				exit(EXIT_SUCCESS);
				break;
			default:
				print_usage(argv[0]);
				exit(EXIT_FAILURE);
		}
	}
}

void signal_handler(int signum)
{
	if(signum == SIGINT || signum == SIGTERM)
	{
		printf("Received Stop signal...\n");
		stop_signal = 1;
	}
}

int main(int argc, char* argv[])
{
	struct module_list *list;
	struct module_attr attr;
	struct timeval timeout;
	struct json *cfg;
	fd_set fds;

	/* Parse options */
	parse_opt(argc, argv);

	/* Set configuration filename */
	if(config_file == NULL)
		config_file = strdup(CONFIG_PATH "/aircat.conf");

	/* Open configuration */
	config_open(&config, config_file);

	/* Setup signal handler */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Load module list */
	modules = modules_load(MODULES_PATH);

	/* Open Avahi Client */
	avahi_open(&avahi);

	/* Open Output Module */
	output_open(&output, OUTPUT_ALSA, 44100, 2);

	/* Get HTTP configuration from file */
	cfg = config_get_json(config, "httpd");

	/* Open HTTP Server */
	httpd_open(&httpd, cfg);

	/* Free HTTP configuration */
	json_free(cfg);

	/* Open all modules */
	attr.avahi = avahi;
	attr.output = output;
	for(list = modules; list != NULL; list = list->next)
	{
		/* Open module */
		if(list->mod->open == NULL)
			continue;

		/* Get module configuration */
		attr.config = config_get_json(config, list->mod->name);

		/* Open module */
		if(list->mod->open(&list->mod->handle, &attr) != 0)
		{
			fprintf(stderr, "Failed to open %s module!\n",
				list->mod->name);
			if(list->mod->close != NULL)
				list->mod->close(&list->mod->handle);
			list->mod->handle = NULL;
		}

		/* Free module configuration */
		json_free((struct json *) attr.config);

		/* Add URLs to HTTP server */
		if(list->mod->urls != NULL)
			httpd_add_urls(httpd, list->mod->name, list->mod->urls,
				       list->mod->handle);
	}

	/* Add basic URLs */
	httpd_add_urls(httpd, "config", config_urls, NULL);

	/* Start HTTP Server */
	httpd_start(httpd);

	/* Wait an input on stdin (only for test purpose) */
	while(!stop_signal)
	{
		FD_ZERO(&fds);
		FD_SET(0, &fds); 
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		if(select(1, &fds, NULL, NULL, &timeout) < 0)
			break;

		if(FD_ISSET(0, &fds))
			break;

		/* Iterate Avahi client */
		avahi_loop(avahi, 10);
	}

	/* Stop HTTP Server */
	httpd_stop(httpd);

	/* Close HTTP Server */
	httpd_close(httpd);

	/* Close all modules */
	for(list = modules; list != NULL; list = list->next)
	{
		/* Save module configuration */
		if(list->mod->get_config != NULL)
		{
			cfg = list->mod->get_config(list->mod->handle);
			config_set_json(config, list->mod->name, cfg);
			json_free(cfg);
		}

		/* Close module */
		if(list->mod->close != NULL)
			list->mod->close(list->mod->handle);
	}

	/* Close Output Module */
	output_close(output);

	/* Close Avahi Client */
	avahi_close(avahi);

	/* Save configuration */
	config_save(config);

	/* Close Configuration */
	if(config_file != NULL)
		free(config_file);
	config_close(config);

	/* Free modules */
	modules_free(modules);

	return EXIT_SUCCESS;
}

/******************************************************************************
 *                           Basic URLs for AirCat                            *
 ******************************************************************************/

static int config_httpd_default(void *h, struct httpd_req *req,
				unsigned char **buffer, size_t *size)
{
	struct module_list *list;

	/* Set HTTP server to default */
	httpd_set_config(httpd, NULL);

	/* Set all modules to default */
	for(list = modules; list != NULL; list = list->next)
	{
		if(list->mod->set_config != NULL)
			list->mod->set_config(list->mod->handle, NULL);
	}

	return 200;
}

static int config_httpd_reload(void *h, struct httpd_req *req,
			       unsigned char **buffer, size_t *size)
{
	struct module_list *list;
	struct json *cfg;

	/* Load config from file */
	config_load(config);

	/* Get HTTP configuration from file */
	cfg = config_get_json(config, "httpd");

	/* Set HTTP server configuration */
	httpd_set_config(httpd, cfg);

	/* Free configuration */
	json_free(cfg);

	/* Set configuration of all modules */
	for(list = modules; list != NULL; list = list->next)
	{
		/* Get module configuration from file */
		cfg = config_get_json(config, list->mod->name);

		/* Set configuration */
		if(list->mod->set_config != NULL)
			list->mod->set_config(list->mod->handle, cfg);

		/* Free module configuration */
		json_free(cfg);
	}

	return 200;
}

static int config_httpd_save(void *h, struct httpd_req *req,
			     unsigned char **buffer, size_t *size)
{
	struct module_list *list;
	struct json *cfg = NULL;

	/* Get HTTP configuration from module */
	cfg = httpd_get_config(httpd);

	/* Set HTTP configuration in file */
	config_set_json(config, "httpd", cfg);

	/* Free configuration */
	json_free(cfg);

	/* Get all modules configuration */
	for(list = modules; list != NULL; list = list->next)
	{
		/* Get configuration from module */
		if(list->mod->get_config != NULL)
			cfg = list->mod->get_config(list->mod->handle);

		/* Set configuration in file */
		config_set_json(config, list->mod->name, cfg);

		/* Free configuration */
		json_free(cfg);
		cfg = NULL;
	}

	/* Save config to file */
	config_save(config);

	return 200;
}

static int config_httpd(void *h, struct httpd_req *req,
			     unsigned char **buffer, size_t *size)
{
	struct module_list *list;
	struct json *json, *tmp;
	struct lh_entry *entry;
	const char *str;

	if(req->method == HTTPD_GET)
	{
		/* Create a JSON object */
		json = json_new();

		/* Get HTTP configuration from module */
		tmp = httpd_get_config(httpd);
		if(tmp != NULL)
		{
			/* Add object to main JSON object */
			if(req->resource == NULL || *req->resource == '\0' ||
			   strcmp(req->resource, "httpd") == 0)
			json_add(json, "httpd", tmp);
		}

		/* Get all modules configuration */
		for(list = modules; list != NULL; list = list->next)
		{
			/* Get configuration from module */
			if(list->mod->get_config != NULL)
				tmp = list->mod->get_config(list->mod->handle);
			if(tmp == NULL)
				continue;

			/* Add object to main JSON object */
			if(req->resource == NULL || *req->resource == '\0' ||
			   strcmp(req->resource, list->mod->name) == 0)
			json_add(json, list->mod->name, tmp);
		}

		/* Get string */
		str = strdup(json_export(json));
		*buffer = (unsigned char*) str;
		*size = strlen(str);

		/* Free configuration */
		json_free(json);
	}
	else
	{
		/* Parse each JSON entry */
		json_foreach(req->json, str, tmp, entry)
		{
			/* Check resource name */
			if(req->resource != NULL && *req->resource != '\0' &&
			   strcmp(req->resource, str) != 0)
				continue;

			/* Get HTTP configuration from module */
			if(strcmp(str, "httpd") == 0)
			{
				/* Set configuration */
				httpd_set_config(httpd, tmp);
				continue;
			}

			/* Check in modules */
			for(list = modules; list != NULL; list = list->next)
			{
				if(strcmp(str, list->mod->name) == 0)
				{
					/* Set configuration */
					if(list->mod->set_config != NULL)
						list->mod->set_config(
							      list->mod->handle,
							      tmp);
					continue;
				}
			}
		}
	}

	return 200;
}

struct url_table config_urls[] = {
	{"default", 0,             HTTPD_PUT,             0,
						 (void*) &config_httpd_default},
	{"reload",  0,             HTTPD_PUT,             0,
						  (void*) &config_httpd_reload},
	{"save",    0,             HTTPD_PUT,             0,
						    (void*) &config_httpd_save},
	{"",        HTTPD_EXT_URL, HTTPD_GET | HTTPD_PUT, HTTPD_JSON,
							 (void*) &config_httpd},
	{0, 0, 0, 0}
};

