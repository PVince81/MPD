/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "../decoder_api.h"

#include "mp4ff.h"

#include <limits.h>
#include <faad.h>
#include <glib.h>
#include <stdlib.h>
#include <unistd.h>

/* all code here is either based on or copied from FAAD2's frontend code */

static int
mp4_get_aac_track(mp4ff_t * infile)
{
	/* find AAC track */
	int i, rc;
	int num_tracks = mp4ff_total_tracks(infile);

	for (i = 0; i < num_tracks; i++) {
		unsigned char *buff = NULL;
		unsigned int buff_size = 0;
#ifdef HAVE_MP4AUDIOSPECIFICCONFIG
		mp4AudioSpecificConfig mp4ASC;
#else
		unsigned long dummy1_32;
		unsigned char dummy2_8, dummy3_8, dummy4_8, dummy5_8, dummy6_8,
		    dummy7_8, dummy8_8;
#endif

		mp4ff_get_decoder_config(infile, i, &buff, &buff_size);

		if (buff) {
#ifdef HAVE_MP4AUDIOSPECIFICCONFIG
			rc = AudioSpecificConfig(buff, buff_size, &mp4ASC);
#else
			rc = AudioSpecificConfig(buff, &dummy1_32, &dummy2_8,
						 &dummy3_8, &dummy4_8,
						 &dummy5_8, &dummy6_8,
						 &dummy7_8, &dummy8_8);
#endif
			free(buff);
			if (rc < 0)
				continue;
			return i;
		}
	}

	/* can't decode this */
	return -1;
}

static uint32_t
mp4_read(void *user_data, void *buffer, uint32_t length)
{
	return input_stream_read((struct input_stream *) user_data,
				   buffer, length);
}

static uint32_t
mp4_seek(void *user_data, uint64_t position)
{
	return input_stream_seek((struct input_stream *) user_data,
				 position, SEEK_SET)
		? 0 : -1;
}

static bool
mp4_decode(struct decoder *mpd_decoder, struct input_stream *input_stream)
{
	mp4ff_callback_t callback = {
		.read = mp4_read,
		.seek = mp4_seek,
		.user_data = input_stream,
	};
	mp4ff_t *mp4fh;
	int32_t track;
	float file_time, total_time;
	int32_t scale;
	faacDecHandle decoder;
	faacDecFrameInfo frame_info;
	faacDecConfigurationPtr config;
	struct audio_format audio_format;
	unsigned char *mp4_buffer;
	unsigned int mp4_buffer_size;
	uint32_t sample_rate;
	unsigned char channels;
	long sample_id;
	long num_samples;
	long dur;
	unsigned int sample_count;
	char *sample_buffer;
	size_t sample_buffer_length;
	unsigned int initial = 1;
	float *seek_table;
	long seek_table_end = -1;
	bool seek_position_found = false;
	long offset;
	uint16_t bit_rate = 0;
	bool seeking = false;
	double seek_where = 0;
	bool initialized = false;

	callback.read = mp4_read;
	callback.seek = mp4_seek;
	callback.user_data = input_stream;

	mp4fh = mp4ff_open_read(&callback);
	if (!mp4fh) {
		g_warning("Input does not appear to be a mp4 stream.\n");
		return false;
	}

	track = mp4_get_aac_track(mp4fh);
	if (track < 0) {
		g_warning("No AAC track found in mp4 stream.\n");
		mp4ff_close(mp4fh);
		return false;
	}

	decoder = faacDecOpen();

	config = faacDecGetCurrentConfiguration(decoder);
	config->outputFormat = FAAD_FMT_16BIT;
#ifdef HAVE_FAACDECCONFIGURATION_DOWNMATRIX
	config->downMatrix = 1;
#endif
#ifdef HAVE_FAACDECCONFIGURATION_DONTUPSAMPLEIMPLICITSBR
	config->dontUpSampleImplicitSBR = 0;
#endif
	faacDecSetConfiguration(decoder, config);

	audio_format.bits = 16;

	mp4_buffer = NULL;
	mp4_buffer_size = 0;
	mp4ff_get_decoder_config(mp4fh, track, &mp4_buffer, &mp4_buffer_size);

	if (faacDecInit2(decoder, mp4_buffer, mp4_buffer_size,
			 &sample_rate, &channels) < 0) {
		g_warning("Not an AAC stream.\n");
		faacDecClose(decoder);
		mp4ff_close(mp4fh);
		return false;
	}

	audio_format.sample_rate = sample_rate;
	audio_format.channels = channels;
	file_time = mp4ff_get_track_duration_use_offsets(mp4fh, track);
	scale = mp4ff_time_scale(mp4fh, track);

	g_free(mp4_buffer);

	if (scale < 0) {
		g_warning("Error getting audio format of mp4 AAC track.\n");
		faacDecClose(decoder);
		mp4ff_close(mp4fh);
		return false;
	}
	total_time = ((float)file_time) / scale;

	num_samples = mp4ff_num_samples(mp4fh, track);
	if (num_samples > (long)(INT_MAX / sizeof(float))) {
		 g_warning("Integer overflow.\n");
		 faacDecClose(decoder);
		 mp4ff_close(mp4fh);
		 return false;
	}

	file_time = 0.0;

	seek_table = g_malloc(sizeof(float) * num_samples);

	for (sample_id = 0; sample_id < num_samples; sample_id++) {
		if (decoder_get_command(mpd_decoder) == DECODE_COMMAND_SEEK) {
			seeking = true;
			seek_where = decoder_seek_where(mpd_decoder);
		}

		if (seeking && seek_table_end > 1 &&
		    seek_table[seek_table_end] >= seek_where) {
			int i = 2;
			while (seek_table[i] < seek_where)
				i++;
			sample_id = i - 1;
			file_time = seek_table[sample_id];
		}

		dur = mp4ff_get_sample_duration(mp4fh, track, sample_id);
		offset = mp4ff_get_sample_offset(mp4fh, track, sample_id);

		if (sample_id > seek_table_end) {
			seek_table[sample_id] = file_time;
			seek_table_end = sample_id;
		}

		if (sample_id == 0)
			dur = 0;
		if (offset > dur)
			dur = 0;
		else
			dur -= offset;
		file_time += ((float)dur) / scale;

		if (seeking && file_time > seek_where)
			seek_position_found = true;

		if (seeking && seek_position_found) {
			seek_position_found = false;
			seeking = 0;
			decoder_command_finished(mpd_decoder);
		}

		if (seeking)
			continue;

		if (mp4ff_read_sample(mp4fh, track, sample_id, &mp4_buffer,
				      &mp4_buffer_size) == 0)
			break;

#ifdef HAVE_FAAD_BUFLEN_FUNCS
		sample_buffer = faacDecDecode(decoder, &frame_info, mp4_buffer,
					      mp4_buffer_size);
#else
		sample_buffer = faacDecDecode(decoder, &frame_info, mp4_buffer);
#endif

		if (mp4_buffer)
			free(mp4_buffer);
		if (frame_info.error > 0) {
			g_warning("faad2 error: %s\n",
				  faacDecGetErrorMessage(frame_info.error));
			break;
		}

		if (!initialized) {
			channels = frame_info.channels;
#ifdef HAVE_FAACDECFRAMEINFO_SAMPLERATE
			scale = frame_info.samplerate;
#endif
			audio_format.sample_rate = scale;
			audio_format.channels = frame_info.channels;
			decoder_initialized(mpd_decoder, &audio_format,
					    input_stream->seekable,
					    total_time);
			initialized = true;
		}

		if (channels * (unsigned long)(dur + offset) > frame_info.samples) {
			dur = frame_info.samples / channels;
			offset = 0;
		}

		sample_count = (unsigned long)(dur * channels);

		if (sample_count > 0) {
			initial = 0;
			bit_rate = frame_info.bytesconsumed * 8.0 *
			    frame_info.channels * scale /
			    frame_info.samples / 1000 + 0.5;
		}

		sample_buffer_length = sample_count * 2;

		sample_buffer += offset * channels * 2;

		decoder_data(mpd_decoder, input_stream, sample_buffer,
			     sample_buffer_length, file_time,
			     bit_rate, NULL);
		if (decoder_get_command(mpd_decoder) == DECODE_COMMAND_STOP)
			break;
	}

	free(seek_table);
	faacDecClose(decoder);
	mp4ff_close(mp4fh);

	if (!initialized)
		return false;

	if (decoder_get_command(mpd_decoder) == DECODE_COMMAND_SEEK && seeking)
		decoder_command_finished(mpd_decoder);

	return true;
}

static struct tag *
mp4_load_tag(const char *file)
{
	struct tag *ret = NULL;
	struct input_stream input_stream;
	mp4ff_callback_t callback = {
		.read = mp4_read,
		.seek = mp4_seek,
		.user_data = &input_stream,
	};
	mp4ff_t *mp4fh;
	int32_t track;
	int32_t file_time;
	int32_t scale;
	int i;

	if (!input_stream_open(&input_stream, file)) {
		g_warning("mp4_load_tag: Failed to open file: %s\n", file);
		return NULL;
	}

	mp4fh = mp4ff_open_read(&callback);
	if (!mp4fh) {
		input_stream_close(&input_stream);
		return NULL;
	}

	track = mp4_get_aac_track(mp4fh);
	if (track < 0) {
		mp4ff_close(mp4fh);
		input_stream_close(&input_stream);
		return NULL;
	}

	ret = tag_new();
	file_time = mp4ff_get_track_duration_use_offsets(mp4fh, track);
	scale = mp4ff_time_scale(mp4fh, track);
	if (scale < 0) {
		mp4ff_close(mp4fh);
		input_stream_close(&input_stream);
		tag_free(ret);
		return NULL;
	}
	ret->time = ((float)file_time) / scale + 0.5;

	for (i = 0; i < mp4ff_meta_get_num_items(mp4fh); i++) {
		char *item;
		char *value;

		mp4ff_meta_get_by_index(mp4fh, i, &item, &value);

		if (0 == strcasecmp("artist", item)) {
			tag_add_item(ret, TAG_ITEM_ARTIST, value);
		} else if (0 == strcasecmp("title", item)) {
			tag_add_item(ret, TAG_ITEM_TITLE, value);
		} else if (0 == strcasecmp("album", item)) {
			tag_add_item(ret, TAG_ITEM_ALBUM, value);
		} else if (0 == strcasecmp("track", item)) {
			tag_add_item(ret, TAG_ITEM_TRACK, value);
		} else if (0 == strcasecmp("disc", item)) {	/* Is that the correct id? */
			tag_add_item(ret, TAG_ITEM_DISC, value);
		} else if (0 == strcasecmp("genre", item)) {
			tag_add_item(ret, TAG_ITEM_GENRE, value);
		} else if (0 == strcasecmp("date", item)) {
			tag_add_item(ret, TAG_ITEM_DATE, value);
		}

		free(item);
		free(value);
	}

	mp4ff_close(mp4fh);
	input_stream_close(&input_stream);

	return ret;
}

static struct tag *
mp4_tag_dup(const char *file)
{
	struct tag *ret = NULL;

	ret = mp4_load_tag(file);
	if (!ret)
		return NULL;
	if (tag_is_empty(ret)) {
		struct tag *temp = tag_id3_load(file);
		if (temp) {
			temp->time = ret->time;
			tag_free(ret);
			ret = temp;
		}
	}

	return ret;
}

static const char *const mp4_suffixes[] = { "m4a", "mp4", NULL };
static const char *const mp4_mime_types[] = { "audio/mp4", "audio/m4a", NULL };

const struct decoder_plugin mp4_plugin = {
	.name = "mp4",
	.stream_decode = mp4_decode,
	.tag_dup = mp4_tag_dup,
	.stream_types = INPUT_PLUGIN_STREAM_FILE | INPUT_PLUGIN_STREAM_URL,
	.suffixes = mp4_suffixes,
	.mime_types = mp4_mime_types,
};
