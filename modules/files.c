/*
 * files.h - A File manager module
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
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>
#include <pthread.h>
#include <unistd.h>

#include "module.h"
#include "utils.h"
#include "file.h"

#define PLAYLIST_ALLOC_SIZE 32

struct files_playlist {
	char *filename;
	struct file_format *format;
};

struct files_handle {
	/* Output handle */
	struct output_handle *output;
	/* Current file player */
	struct file_handle *file;
	struct output_stream *stream;
	/* Previous file player */
	struct file_handle *prev_file;
	struct output_stream *prev_stream;
	/* Player status */
	int is_playing;
	/* Playlist */
	struct files_playlist *playlist;
	int playlist_alloc;
	int playlist_len;
	int playlist_cur;
	/* Thread */
	pthread_t thread;
	pthread_mutex_t mutex;
	int stop;
	/* Configuration */
	char *path;
};

static void *files_thread(void *user_data);
static int files_stop(struct files_handle *h);
static int files_set_config(struct files_handle *h, const struct json *c);

static int files_open(struct files_handle **handle, struct module_attr *attr)
{
	struct files_handle *h;

	/* Allocate structure */
	*handle = malloc(sizeof(struct files_handle));
	if(*handle == NULL)
		return -1;
	h = *handle;

	/* Init structure */
	h->output = attr->output;
	h->file = NULL;
	h->prev_file = NULL;
	h->stream = NULL;
	h->prev_stream = NULL;
	h->is_playing = 0;
	h->playlist_cur = -1;
	h->stop = 0;
	h->path = NULL;

	/* Allocate playlist */
	h->playlist = malloc(PLAYLIST_ALLOC_SIZE *
			     sizeof(struct files_playlist));
	if(h->playlist != NULL)
		h->playlist_alloc = PLAYLIST_ALLOC_SIZE;
	h->playlist_len = 0;

	/* Set configuration */
	files_set_config(h, attr->config);

	/* Init thread */
	pthread_mutex_init(&h->mutex, NULL);

	/* Create thread */
	if(pthread_create(&h->thread, NULL, files_thread, h) != 0)
		return -1;

	return 0;
}

static int files_new_player(struct files_handle *h)
{
	unsigned long samplerate;
	unsigned char channels;

	/* Start new player */
	if(file_open(&h->file, h->playlist[h->playlist_cur].filename) != 0)
	{
		file_close(h->file);
		h->file = NULL;
		h->stream = NULL;
		return -1;
	}

	/* Get samplerate and channels */
	samplerate = file_get_samplerate(h->file);
	channels = file_get_channels(h->file);

	/* Open new Audio stream output and play */
	h->stream = output_add_stream(h->output, samplerate, channels, 0, 0,
				      &file_read, h->file);
	output_play_stream(h->output, h->stream);

	return 0;
}

static void files_play_next(struct files_handle *h)
{
	/* Close previous stream */
	if(h->prev_stream != NULL)
		output_remove_stream(h->output, h->prev_stream);

	/* Close previous file */
	file_close(h->prev_file);

	/* Move current stream to previous */
	h->prev_stream = h->stream;
	h->prev_file = h->file;

	/* Open next file in playlist */
	while(h->playlist_cur <= h->playlist_len)
	{
		h->playlist_cur++;
		if(h->playlist_cur >= h->playlist_len)
		{
			h->playlist_cur = -1;
			h->stream = NULL;
			h->file = NULL;
			break;
		}

		if(files_new_player(h) != 0)
			continue;

		break;
	}
}

static void files_play_prev(struct files_handle *h)
{
	/* Close previous stream */
	if(h->prev_stream != NULL)
		output_remove_stream(h->output, h->prev_stream);

	/* Close previous file */
	file_close(h->prev_file);

	/* Move current stream to previous */
	h->prev_stream = h->stream;
	h->prev_file = h->file;

	/* Open next file in playlist */
	while(h->playlist_cur >= 0)
	{
		h->playlist_cur--;
		if(h->playlist_cur < 0)
		{
			h->playlist_cur = -1;
			h->stream = NULL;
			h->file = NULL;
			break;
		}

		if(files_new_player(h) != 0)
			continue;

		break;
	}
}

static void *files_thread(void *user_data)
{
	struct files_handle *h = (struct files_handle *) user_data;

	while(!h->stop)
	{
		/* Lock playlist */
		pthread_mutex_lock(&h->mutex);

		if(h->playlist_cur != -1 &&
		   h->playlist_cur+1 <= h->playlist_len)
		{
			if(h->file != NULL && 
			   (file_get_pos(h->file) >= file_get_length(h->file)-1
			   || file_get_status(h->file) == FILE_EOF))
			{
				files_play_next(h);
			}
		}

		/* Unlock playlist */
		pthread_mutex_unlock(&h->mutex);

		/* Sleep during 100ms */
		usleep(100000);
	}

	return NULL;
}

static inline void files_free_playlist(struct files_playlist *p)
{
	if(p->filename != NULL)
		free(p->filename);
	if(p->format != NULL)
		file_format_free(p->format);
}

static int files_add(struct files_handle *h, const char *filename)
{
	struct files_playlist *p;
	char *real_path;
	int len;

	if(filename == NULL)
		return -1;

	/* Make real path */
	len = strlen(h->path) + strlen(filename) + 2;
	real_path = calloc(sizeof(char), len);
	if(real_path == NULL)
		return -1;
	sprintf(real_path, "%s/%s", h->path, filename);

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	/* Add more space to playlist */
	if(h->playlist_len == h->playlist_alloc)
	{
		/* Reallocate playlist */
		p = realloc(h->playlist,
			    (h->playlist_alloc + PLAYLIST_ALLOC_SIZE) *
			    sizeof(struct files_playlist));
		if(p == NULL)
			return -1;

		h->playlist = p;
		h->playlist_alloc += PLAYLIST_ALLOC_SIZE;
	}

	/* Fill the new playlist entry */
	p = &h->playlist[h->playlist_len];
	p->filename = strdup(real_path);
	p->format = file_format_parse(real_path, TAG_PICTURE);

	/* Increment playlist len */
	h->playlist_len++;

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return h->playlist_len - 1;
}

static int files_remove(struct files_handle *h, int index)
{
	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	/* Check if it is current file */
	if(h->playlist_cur == index)
	{
		/* Unlock playlist */
		pthread_mutex_unlock(&h->mutex);

		files_stop(h);

		/* Lock playlist */
		pthread_mutex_lock(&h->mutex);

		h->playlist_cur = -1;
	}
	else if(h->playlist_cur > index)
	{
		h->playlist_cur--;
	}

	/* Free index playlist structure */
	files_free_playlist(&h->playlist[index]);

	/* Remove the index from playlist */
	memmove(&h->playlist[index], &h->playlist[index+1], 
		(h->playlist_len - index - 1) * sizeof(struct files_playlist));
	h->playlist_len--;

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return 0;
}

static void files_flush(struct files_handle *h)
{
	/* Stop playing before flush */
	files_stop(h);

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	/* Flush all playlist */
	for(; h->playlist_len--;)
	{
		files_free_playlist(&h->playlist[h->playlist_len]);
	}
	h->playlist_len = 0;
	h->playlist_cur = -1;

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);
}

static int files_play(struct files_handle *h, int index)
{
	/* Get last played index */
	if(index == -1 && (index = h->playlist_cur) < 0)
		index = 0;

	/* Check playlist index */
	if(index >= h->playlist_len)
		return -1;

	/* Stop previous playing */
	files_stop(h);

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	/* Start new player */
	h->playlist_cur = index;
	if(files_new_player(h) != 0)
	{
		/* Unlock playlist */
		pthread_mutex_unlock(&h->mutex);

		h->playlist_cur = -1;
		h->is_playing = 0;
		return -1;
	}

	h->is_playing = 1;

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return 0;
}

static int files_pause(struct files_handle *h)
{
	if(h == NULL || h->output == NULL)
		return 0;

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	if(h->stream != NULL)
	{
		if(!h->is_playing)
		{
			h->is_playing = 1;
			output_play_stream(h->output, h->stream);
		}
		else
		{
			h->is_playing = 0;
			output_pause_stream(h->output, h->stream);
		}
	}

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return 0;
}

static int files_stop(struct files_handle *h)
{
	if(h == NULL)
		return 0;

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	/* Stop stream */
	h->is_playing = 0;

	/* Close stream */
	if(h->stream != NULL)
		output_remove_stream(h->output, h->stream);
	if(h->prev_stream != NULL)
		output_remove_stream(h->output, h->prev_stream);
	h->stream = NULL;
	h->prev_stream = NULL;

	/* Close file */
	file_close(h->file);
	file_close(h->prev_file);
	h->file = NULL;
	h->prev_file = NULL;

	/* Rreset playlist position */
	h->playlist_cur = -1;

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return 0;
}

static int files_prev(struct files_handle *h)
{
	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	if(h->playlist_cur != -1 && h->playlist_cur >= 0)
	{
		/* Start next file in playlist */
		files_play_prev(h);

		/* Close previous stream */
		if(h->prev_stream != NULL)
			output_remove_stream(h->output, h->prev_stream);

		/* Close previous file */
		file_close(h->prev_file);

		h->prev_stream = NULL;
		h->prev_file = NULL;
	}

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return 0;
}

static int files_next(struct files_handle *h)
{
	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	if(h->playlist_cur != -1 && h->playlist_cur+1 <= h->playlist_len)
	{
		/* Start next file in playlist */
		files_play_next(h);

		/* Close previous stream */
		if(h->prev_stream != NULL)
			output_remove_stream(h->output, h->prev_stream);

		/* Close previous file */
		file_close(h->prev_file);

		h->prev_stream = NULL;
		h->prev_file = NULL;
	}

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return 0;
}

static int files_seek(struct files_handle *h, unsigned long pos)
{
	int ret;

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	ret = file_set_pos(h->file, pos);

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	return ret;
}

static struct json *files_get_file_json_object(const char *filename,
					       struct file_format *meta,
					       int add_pic)
{
	struct json *tmp = NULL;
	char *pic = NULL;

	if(filename == NULL)
		return NULL;

	/* Create temporary object */
	tmp = json_new();
	if(tmp == NULL)
		return NULL;

	/* Add filename */
	json_set_string(tmp, "file", filename);

	/* Get tag data */
	if(meta != NULL)
	{
		/* Add all tags */
		json_set_string(tmp, "title", meta->title);
		json_set_string(tmp, "artist", meta->artist);
		json_set_string(tmp, "album", meta->album);
		json_set_string(tmp, "comment", meta->comment);
		json_set_string(tmp, "genre", meta->genre);
		json_set_int(tmp, "track", meta->track);
		json_set_int(tmp, "year", meta->year);

		/* Get picture */
		if(add_pic && meta->picture.data != NULL)
			pic = base64_encode((const char *)meta->picture.data,
					    meta->picture.size);

		/* Add picture to object */
		json_set_string(tmp, "picture", pic);
		json_set_string(tmp, "mime", meta->picture.mime);
		if(pic != NULL)
			free(pic);
	}

	return tmp;
}

static char *files_get_json_status(struct files_handle *h, int add_pic)
{
	struct json *tmp;
	char *str = NULL;
	int idx;

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	idx = h->playlist_cur;
	if(idx < 0)
	{
		/* Unlock playlist */
		pthread_mutex_unlock(&h->mutex);

		return strdup("{ \"file\": null }");
	}

	/* Create basic JSON object */
	str = basename(h->playlist[idx].filename);
	tmp = files_get_file_json_object(str, h->playlist[idx].format, add_pic);
	if(tmp != NULL)
	{
		/* Add curent postion and audio file length */
		json_set_int(tmp, "pos", file_get_pos(h->file));
		json_set_int(tmp, "length", file_get_length(h->file));
	}

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	/* Get JSON string */
	str = strdup(json_export(tmp));

	/* Free JSON object */
	json_free(tmp);

	return str;
}

static char *files_get_json_playlist(struct files_handle *h)
{
	struct json *root, *tmp;
	char *str;
	int i;

	/* Create JSON object */
	root = json_new_array();
	if(root == NULL)
		return NULL;

	/* Lock playlist */
	pthread_mutex_lock(&h->mutex);

	/* Fill the JSON array with playlist */
	for(i = 0; i < h->playlist_len; i++)
	{
		/* Create temporary object */
		str = basename(h->playlist[i].filename);
		tmp = files_get_file_json_object(str, h->playlist[i].format, 0);
		if(tmp == NULL)
			continue;

		/* Add to list */
		if(json_array_add(root, tmp) != 0)
			json_free(tmp);
	}

	/* Unlock playlist */
	pthread_mutex_unlock(&h->mutex);

	/* Get JSON string */
	str = strdup(json_export(root));

	/* Free JSON object */
	json_free(root);

	return str;
}

static char *files_get_json_list(struct files_handle *h, const char *path)
{
	char *ext[] = { ".mp3", ".m4a", ".mp4", ".aac", ".ogg", ".wav", NULL };
	struct json *root = NULL, *dir_list, *file_list, *tmp;
	struct dirent *entry;
	struct stat s;
	DIR *dir;
	struct file_format *format;
	char *real_path;
	char *str = NULL;
	int len, i;

	/* Make real path */
	if(path == NULL)
	{
		real_path = strdup(h->path);
	}
	else
	{
		len = strlen(h->path) + strlen(path) + 2;
		real_path = calloc(sizeof(char), len);
		if(real_path == NULL)
			return NULL;
		sprintf(real_path, "%s/%s", h->path, path);
	}

	/* Open directory */
	dir = opendir(real_path);
	if(dir == NULL)
		goto end;

	/* Create JSON object */
	root = json_new();
	dir_list = json_new_array();
	if(root == NULL || dir_list == NULL)
		goto end;
	file_list = json_new_array();
	if(file_list == NULL)
	{
		json_free(dir_list);
		goto end;
	}

	/* List files  */
	while((entry = readdir(dir)) != NULL)
	{
		if(entry->d_name[0] == '.')
			continue;

		/* Make complete filanme path */
		len = strlen(real_path) + strlen(entry->d_name) + 2;
		str = calloc(sizeof(char), len);
		if(str == NULL)
			continue;
		sprintf(str, "%s/%s", real_path, entry->d_name);

		/* Stat file */
		stat(str, &s);

		/* Add to array */
		if(s.st_mode & S_IFREG)
		{
			/* Verify extension */
			len = strlen(entry->d_name);
			for(i = 0; ext[i] != NULL; i++)
			{
				if(strcmp(&entry->d_name[len-4], ext[i]) == 0)
				{
					/* Read meta data from file */
					format = file_format_parse(str,
								   TAG_PICTURE);

					/* Create temporary object */
					tmp = files_get_file_json_object(
								  entry->d_name,
								  format, 1);
					if(format != NULL)
						file_format_free(format);
					if(tmp == NULL)
						continue;

					if(json_array_add(file_list, tmp)
					   != 0)
						json_free(tmp);
					break;
				}
			}
		}
		else if(s.st_mode & S_IFDIR)
		{
			/* Create temporary object */
			tmp = json_new_string(entry->d_name);
			if(tmp == NULL)
				continue;

			if(json_array_add(dir_list, tmp) != 0)
				json_free(tmp);
		}

		/* Free complete filenmae */
		free(str);
	}

	/* Add both arrays to JSON object */
	json_add(root, "directory", dir_list);
	json_add(root, "file", file_list);

	/* Get JSON string */
	str = strdup(json_export(root));

end:
	if(root != NULL)
		json_free(root);

	if(real_path != NULL)
		free(real_path);

	if(dir != NULL)
		closedir(dir);

	return str;
}

static int files_set_config(struct files_handle *h, const struct json *c)
{
	const char *path;

	if(h == NULL)
		return -1;

	/* Free previous values */
	if(h->path != NULL)
		free(h->path);
	h->path = NULL;

	/* Parse configuration */
	if(c != NULL)
	{
		/* Get files path */
		path = json_get_string(c, "path");
		if(path != NULL)
			h->path = strdup(path);
	}

	/* Set default values */
	if(h->path == NULL)
		h->path = strdup("/var/aircat/files");

	return 0;
}

static struct json *files_get_config(struct files_handle *h)
{
	struct json *c;

	/* Create a new config */
	c = json_new();
	if(c == NULL)
		return NULL;

	/* Set current files path */
	json_set_string(c, "path", h->path);

	return c;
}

static int files_close(struct files_handle *h)
{
	if(h == NULL)
		return 0;

	/* Stop playing */
	files_stop(h);

	/* Stop thread */
	h->stop = 1;
	if(pthread_join(h->thread, NULL) < 0)
		return -1;

	/* Free playlist */
	if(h->playlist != NULL)
	{
		files_flush(h);
		free(h->playlist);
	}

	/* Free files path */
	if(h->path != NULL)
		free(h->path);

	free(h);

	return 0;
}

#define HTTPD_RESPONSE(s) *buffer = (unsigned char*)s; \
			  *size = strlen(s);

static int files_httpd_playlist_add(struct files_handle *h,
				    struct httpd_req *req,
				    unsigned char **buffer, size_t *size)
{
	int idx;

	/* Add file to playlist */
	idx = files_add(h, req->resource);
	if(idx < 0)
	{
		HTTPD_RESPONSE(strdup("File is not supported"));
		return 406;
	}

	return 200;
}

static int files_httpd_playlist_play(struct files_handle *h,
				     struct httpd_req *req,
				     unsigned char **buffer, size_t *size)
{
	int idx;

	/* Get index from URL */
	idx = atoi(req->resource);
	if(idx < 0)
	{
		HTTPD_RESPONSE(strdup("Bad index"));
		return 400;
	}

	/* Play selected file in playlist */
	if(files_play(h, idx) != 0)
	{
		HTTPD_RESPONSE(strdup("Playlist error"));
		return 500;
	}

	return 200;
}

static int files_httpd_playlist_remove(struct files_handle *h,
				       struct httpd_req *req,
				       unsigned char **buffer, size_t *size)
{
	int idx;

	/* Get index from URL */
	idx = atoi(req->resource);
	if(idx < 0)
	{
		HTTPD_RESPONSE(strdup("Bad index"));
		return 400;
	}

	/* Remove from playlist */
	if(files_remove(h, idx) != 0)
	{
		HTTPD_RESPONSE(strdup("Playlist error"));
		return 500;
	}

	return 200;
}

static int files_httpd_playlist_flush(struct files_handle *h,
				      struct httpd_req *req,
				      unsigned char **buffer, size_t *size)
{
	/* Flush playlist */
	files_flush(h);

	return 200;
}

static int files_httpd_playlist(struct files_handle *h, struct httpd_req *req,
			 	unsigned char **buffer, size_t *size)
{
	char *list = NULL;

	/* Get playlist */
	list = files_get_json_playlist(h);
	if(list == NULL)
	{
		HTTPD_RESPONSE(strdup("Playlist error"));
		return 500;
	}

	HTTPD_RESPONSE(list);
	return 200;
}

static int files_httpd_play(struct files_handle *h, struct httpd_req *req,
			    unsigned char **buffer, size_t *size)
{
	int idx = -1;

	/* Add file to playlist */
	if(*req->resource != 0)
	{
		idx = files_add(h, req->resource);
		if(idx < 0)
		{
			HTTPD_RESPONSE(strdup("File is not supported"));
			return 406;
		}
	}

	/* Play the file now */
	if(files_play(h, idx) != 0)
	{
		HTTPD_RESPONSE(strdup("Cannot play the file"));
		return 406;
	}

	return 200;
}

static int files_httpd_pause(struct files_handle *h, struct httpd_req *req,
			     unsigned char **buffer, size_t *size)
{
	/* Pause file playing */
	files_pause(h);

	return 200;
}

static int files_httpd_stop(struct files_handle *h, struct httpd_req *req,
			    unsigned char **buffer, size_t *size)
{
	/* Stop file playing */
	files_stop(h);

	return 200;
}

static int files_httpd_prev(struct files_handle *h, struct httpd_req *req,
			    unsigned char **buffer, size_t *size)
{
	/* Go to / play previous file in playlist */
	files_prev(h);

	return 200;
}

static int files_httpd_next(struct files_handle *h, struct httpd_req *req,
			    unsigned char **buffer, size_t *size)
{
	/* Go to / play next file in playlist */
	files_next(h);

	return 200;
}

static int files_httpd_status(struct files_handle *h, struct httpd_req *req,
			      unsigned char **buffer, size_t *size)
{
	char *str = NULL;
	int add_pic = 0;

	if(strncmp(req->resource, "img", 3) == 0)
		add_pic = 1;

	/* Get status */
	str = files_get_json_status(h, add_pic);
	if(str == NULL)
	{
		HTTPD_RESPONSE(strdup("Status error"));
		return 500;
	}

	HTTPD_RESPONSE(str);
	return 200;
}

static int files_httpd_seek(struct files_handle *h, struct httpd_req *req,
			    unsigned char **buffer, size_t *size)
{
	unsigned long pos;

	/* Get position from URL */
	pos = strtoul(req->resource, NULL, 10);

	/* Seek in stream */
	if(files_seek(h, pos) != 0)
	{
		HTTPD_RESPONSE(strdup("Bad position"));
		return 400;
	}

	return 200;
}

static int files_httpd_list(struct files_handle *h, struct httpd_req *req,
			    unsigned char **buffer, size_t *size)
{
	char *list = NULL;

	/* Get file list */
	list = files_get_json_list(h, req->resource);
	if(list == NULL)
	{
		HTTPD_RESPONSE(strdup("Bad directory"));
		return 404;
	}

	HTTPD_RESPONSE(list);
	return 200;
}

static struct url_table files_url[] = {
	{"/playlist/add/",    HTTPD_EXT_URL, HTTPD_PUT, 0,
					     (void*) &files_httpd_playlist_add},
	{"/playlist/play/",   HTTPD_EXT_URL, HTTPD_PUT, 0,
					    (void*) &files_httpd_playlist_play},
	{"/playlist/remove/", HTTPD_EXT_URL, HTTPD_PUT, 0,
					  (void*) &files_httpd_playlist_remove},
	{"/playlist/flush",   0,             HTTPD_PUT, 0,
					   (void*) &files_httpd_playlist_flush},
	{"/playlist",         0,             HTTPD_GET, 0,
						 (void*) &files_httpd_playlist},
	{"/play",             HTTPD_EXT_URL, HTTPD_PUT, 0,
						     (void*) &files_httpd_play},
	{"/pause",            0,             HTTPD_PUT, 0,
						    (void*) &files_httpd_pause},
	{"/stop",             0,             HTTPD_PUT, 0,
						     (void*) &files_httpd_stop},
	{"/prev",             0,             HTTPD_PUT, 0,
						     (void*) &files_httpd_prev},
	{"/next",             0,             HTTPD_PUT, 0,
						     (void*) &files_httpd_next},
	{"/seek/",            HTTPD_EXT_URL, HTTPD_PUT, 0,
						     (void*) &files_httpd_seek},
	{"/status",           HTTPD_EXT_URL, HTTPD_GET, 0,
						   (void*) &files_httpd_status},
	{"/list",             HTTPD_EXT_URL, HTTPD_GET, 0,
						     (void*) &files_httpd_list},
	{0, 0, 0}
};

struct module module_entry = {
	.id = "files",
	.name = "File browser",
	.description = "Browse through local and remote folder and play any "
		       "music file.",
	.open = (void*) &files_open,
	.close = (void*) &files_close,
	.set_config = (void*) &files_set_config,
	.get_config = (void*) &files_get_config,
	.urls = (void*) &files_url,
};