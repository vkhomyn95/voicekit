/*
 * Asterisk VoiceKit modules
 *
 * Copyright (c) JSC Tinkoff Bank, 2018 - 2019
 *
 * Grigoriy Okopnik <g.e.okopnik@tinkoff.ru>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

extern struct ast_module *AST_MODULE_SELF_SYM(void);
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_SYM

#define _GNU_SOURCE 1
#include "stream_layers.h"

#include "grpctts.h"

#include <asterisk/utils.h>
#include <asterisk/channel.h>
#include <asterisk/mod_format.h>
#include <asterisk/format_cache.h>
#include <asterisk/alaw.h>
#include <asterisk/ulaw.h>
#include <math.h>


#define SAMPLE_RATE 8000
#define ZERO_FRAME_SAMPLE_RATE SAMPLE_RATE
#define ZERO_FRAME_SAMPLES 160
static int16_t zero_frame_samples[ZERO_FRAME_SAMPLES];
static struct ast_frame zero_frame = {
	.frametype = AST_FRAME_VOICE,
	.subclass.format = NULL /* initialized at module loading */,
	.datalen = ZERO_FRAME_SAMPLES*sizeof(int16_t),
	.samples = ZERO_FRAME_SAMPLES,
	.mallocd = 0,
	.mallocd_hdr_len = 0,
	.offset = 0,
	.src = "PlayBackgroundZeroFrame",
	.data.ptr = zero_frame_samples,
};


void stream_layers_global_init()
{
	ao2_ref(ast_format_slin, 1);
	zero_frame.subclass.format = ast_format_slin;
}
void stream_layers_global_uninit()
{
	struct ast_format *format = zero_frame.subclass.format;
	zero_frame.subclass.format = NULL;
	ao2_ref(format, -1);
}
	

static void *void_generator_alloc(struct ast_channel *chan, void *data)
{
	(void) chan;

	return data;
}
static void void_generator_release(struct ast_channel *chan, void *data)
{
	(void) chan;
	(void) data;
}
static int void_generator_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	(void) chan;
	(void) data;
	(void) len;
	(void) samples;

	return 0;
}

static struct ast_generator void_generator = {
	.alloc = void_generator_alloc,
	.release = void_generator_release,
	.generate = void_generator_generate,
};

static void check_start_void_generator(struct ast_channel *chan)
{
	struct ast_generator *generator;
	ast_channel_lock(chan);
	generator = ast_channel_generator(chan);
	ast_channel_unlock(chan);

	if (generator != &void_generator)
		ast_activate_generator(chan, &void_generator, NULL);
}	
static void check_stop_void_generator(struct ast_channel *chan)
{
	struct ast_generator *generator;
	ast_channel_lock(chan);
	generator = ast_channel_generator(chan);
	ast_channel_unlock(chan);

	if (generator == &void_generator)
		ast_deactivate_generator(chan);
}	


static void push_playbackground_finished_event(struct ast_channel *chan, int layer_i)
{
	char data[32];
	snprintf(data, sizeof(data), "%d", layer_i);
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", "PlayBackgroundFinished", "eventbody", data);
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);

	ast_json_unref(blob);
}
static void push_playbackground_error_event(struct ast_channel *chan, int layer_i, const char *error_message)
{
	char data[4096];
	snprintf(data, sizeof(data), "%d,%s", layer_i, error_message);
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", "PlayBackgroundError", "eventbody", data);
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);

	ast_json_unref(blob);
}
static void push_playbackground_duration_event(struct ast_channel *chan, int layer_i, double duration_secs)
{
	char data[128];
	snprintf(data, sizeof(data), "%d,%.09f", layer_i, duration_secs);
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", "PlayBackgroundDuration", "eventbody", data);
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);

	ast_json_unref(blob);
}


static inline int parse_floating_timespec(struct timespec *ts, const char *source)
{
	if (!*source)
		return -1;
	char *eptr;
	double floating_time = strtod(source, &eptr);
	if (*eptr)
		return -1;
	ts->tv_sec = lrint(floor(floating_time));
	ts->tv_nsec = lrint(remainder(floating_time, 1.0)*1000000000.0);
	return 0;
}
static inline struct ast_frame *read_frame(struct ast_filestream *s, int *whennext)
{
	struct ast_frame *fr, *new_fr;

	if (!s || !s->fmt) {
		return NULL;
	}

	if (!(fr = s->fmt->read(s, whennext))) {
		return NULL;
	}

	if (!(new_fr = ast_frisolate(fr))) {
		ast_frfree(fr);
		return NULL;
	}

	if (new_fr != fr) {
		ast_frfree(fr);
		fr = new_fr;
	}

	return fr;
}
static inline struct ast_frame *alloc_frame(size_t sample_count)
{
	size_t byte_count = sample_count*sizeof(int16_t);
	struct ast_frame *fr = ast_calloc(1, sizeof(struct ast_frame));
	if (!fr)
		return NULL;
	if (!(fr->data.ptr = ast_malloc(byte_count))) {
		ast_free(fr);
		return NULL;
	}
	fr->frametype = AST_FRAME_VOICE;
	ao2_ref(fr->subclass.format = ast_format_slin, 1);
	fr->datalen = byte_count;
	fr->samples = sample_count;
	fr->mallocd = AST_MALLOCD_HDR | AST_MALLOCD_DATA;
	fr->mallocd_hdr_len = sizeof(struct ast_frame);
	fr->offset = 0;
	fr->src = "PlayBackgroundSynthesisFrame";
	return fr;
}
static inline void close_stream(struct ast_filestream *fs)
{
	struct ast_channel *chan = fs->owner;
	ast_channel_lock(chan);
	ast_stopstream(chan);
	ast_channel_unlock(chan);
	ao2_ref(fs, -1);
}
static inline struct ast_filestream *open_stream_simple(struct ast_channel *chan, const char *filename, const char *preflang)
{
	/* Looks like a race condition here: opened stream will be dropped at hangup */
	struct ast_filestream *fs = ast_openstream(chan, filename, preflang);
	if (!fs) {
		struct ast_str *codec_buf = ast_str_alloca(AST_FORMAT_CAP_NAMES_LEN);
		ast_channel_lock(chan);
		ast_log(LOG_WARNING, "Unable to open %s (format %s): %s\n",
			filename, ast_format_cap_get_names(ast_channel_nativeformats(chan), &codec_buf), strerror(errno));
		ast_channel_unlock(chan);
		return NULL;
	}
	ao2_ref(fs, 1);
	
	if (ast_test_flag(ast_channel_flags(chan), AST_FLAG_MASQ_NOSTREAM))
		fs->orig_chan_name = ast_strdup(ast_channel_name(chan));
	if (ast_applystream(chan, fs))
		return NULL;

	return fs;
}
static inline struct ast_filestream *open_stream(struct ast_channel *chan, const char *filename, const char *preflang, off_t *duration_samples)
{
	struct ast_filestream *fs = open_stream_simple(chan, filename, preflang);
	if (!fs)
		return NULL;

	if (!ast_seekstream(fs, 0, SEEK_END)) {
		*duration_samples = ast_tellstream(fs);
		if (ast_seekstream(fs, 0, SEEK_SET)) {
			close_stream(fs);
			fs = open_stream_simple(chan, filename, preflang);
		}
	} else {
		*duration_samples = 0;
	}

	return fs;
}
static inline void time_add_samples(struct timespec *dest, int samples)
{
	dest->tv_sec += samples/ZERO_FRAME_SAMPLE_RATE;
	dest->tv_nsec += samples%ZERO_FRAME_SAMPLE_RATE*(1000000000/ZERO_FRAME_SAMPLE_RATE);
	if (dest->tv_nsec >= 1000000000) {
		dest->tv_sec++;
		dest->tv_nsec -= 1000000000;
	}
}
static inline int wait_for_deadline(const struct timespec *deadline, int efd)
{
	/* Returns: -1 on fail; 0 on timeout; 1 on efd triggered */
	struct timespec current_time;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &current_time)) {
		ast_log(AST_LOG_ERROR, "Failed to get current time: %s\n", strerror(errno));
		return -1;
	}
	if (current_time.tv_sec > deadline->tv_sec ||
	    (current_time.tv_sec == deadline->tv_sec && current_time.tv_nsec >= deadline->tv_nsec))
		return 0;
	struct timespec timeout = {
		.tv_sec = deadline->tv_sec - current_time.tv_sec,
		.tv_nsec = deadline->tv_nsec - current_time.tv_nsec,
	};
	if (timeout.tv_nsec < 0) {
		--timeout.tv_sec;
		timeout.tv_nsec += 1000000000;
	}
	struct pollfd pfds = {
		.fd = efd,
		.events = POLLIN,
		.revents = 0,
	};
	return ppoll(&pfds, 1, &timeout, NULL);
}
static inline void stream_source_init(struct stream_source *source)
{
	source->type = STREAM_SOURCE_NONE;
}
static void stream_source_stop(struct stream_source *source)
{
	switch (source->type) {
	case STREAM_SOURCE_FILE: {
		close_stream(source->source.file.filestream);
		ast_frame_dtor(source->source.file.buffered_frame);
	} break;
	case STREAM_SOURCE_SYNTHESIS: {
		struct ast_channel *chan = source->source.synthesis.chan;
		ast_channel_lock(chan);
		ast_stopstream(chan);
		ast_channel_unlock(chan);
		grpctts_job_destroy(source->source.synthesis.job);
		ast_frame_dtor(source->source.synthesis.buffered_frame);
	} break;
	default: {
	}
	}	
	source->type = STREAM_SOURCE_NONE;
}
static inline int stream_source_start_stream_file(struct stream_source *source, const char *fname, struct stream_state *state, double *duration)
{
	struct ast_channel *chan = state->chan;
	off_t duration_samples;
	struct ast_filestream *filestream = open_stream(chan, fname, ast_channel_language(chan), &duration_samples);
	if (!filestream)
		return -1;

	*duration = (duration_samples >= 0) ? (((double)duration_samples)/SAMPLE_RATE) : 0.0;

	source->type = STREAM_SOURCE_FILE;
	source->source.file.filestream = filestream;
	source->source.file.buffered_frame = NULL;
	return 0;
}
static inline void stream_source_start_sleep(struct stream_source *source, const struct timespec *timeout)
{
	source->type = STREAM_SOURCE_SLEEP;
	source->source.sleep.sample_count = timeout->tv_sec*ZERO_FRAME_SAMPLE_RATE + timeout->tv_nsec/(1000000000/ZERO_FRAME_SAMPLE_RATE);
}
static inline void stream_source_start_synthesis(struct stream_source *source, struct stream_state *state,
						 const struct grpctts_job_conf *conf, const struct grpctts_job_input *job_input)
{
	if (!state->tts_channel) {
		push_playbackground_error_event(state->chan, -1, "Failed to start synthesis job: no TTS channel initialized");
		source->type = STREAM_SOURCE_NONE;
		return;
	}

	source->type = STREAM_SOURCE_SYNTHESIS;
	source->source.synthesis.chan = state->chan;
	source->source.synthesis.job = grpctts_channel_start_job(state->tts_channel, conf, job_input);
	source->source.synthesis.buffered_frame = NULL;
	source->source.synthesis.duration_announced = 0;
}
static inline void stream_source_file_buffer_frame(struct stream_source *source)
{
	int whennext;
	struct ast_frame *fr = read_frame(source->source.file.filestream, &whennext);
	if (!fr)
		return;
	source->source.file.buffered_frame = fr;
	source->source.file.buffered_frame_off = 0;
}
static inline int stream_source_synthesis_get_duration(struct stream_source *source, int64_t *duration)
{
	if (source->source.synthesis.duration_announced) {
		ast_log(LOG_ERROR, "PlayBackground() failed: duration already announced\n");
		return -1;
	}
	grpctts_job_collect(source->source.synthesis.job);
	if (grpctts_job_buffer_size(source->source.synthesis.job) >= sizeof(int64_t)) {
		if (!grpctts_job_take_block(source->source.synthesis.job, sizeof(int64_t), duration)) {
			ast_log(LOG_ERROR, "PlayBackground() failed: memory allocation error\n");
			return -1;
		}
		return 1;
	}
	return 0;
}
static inline int stream_source_synthesis_buffer_frame(struct stream_source *source, int samples_to_buffer)
{
	grpctts_job_collect(source->source.synthesis.job);
	size_t sample_count = grpctts_job_buffer_size(source->source.synthesis.job) >> 1;
	if (sample_count) {
		struct ast_frame *fr = alloc_frame(sample_count);
		if (!fr) {
			ast_log(LOG_ERROR, "PlayBackground() failed: memory allocation error\n");
			return -1;
		}
		if (!grpctts_job_take_block(source->source.synthesis.job, sample_count*sizeof(int16_t), fr->data.ptr)) {
			ast_frfree(fr);
			ast_log(LOG_ERROR, "PlayBackground() failed: memory allocation error\n");
			return -1;
		}
		source->source.synthesis.buffered_frame = fr;
		source->source.synthesis.buffered_frame_off = 0;
	} else {
		if (grpctts_job_termination_called(source->source.synthesis.job))
			return grpctts_job_completion_success(source->source.synthesis.job) ? 0 : -1;

		/* Starving for frames: filling with silence */
		struct ast_frame *fr = alloc_frame(samples_to_buffer);
		memset(fr->data.ptr, 0, samples_to_buffer*sizeof(int16_t));
		source->source.synthesis.buffered_frame = fr;
		source->source.synthesis.buffered_frame_off = 0;
	}
	return 0;
}
static inline struct stream_job *stream_job_create(const char *command)
{
	struct stream_job *job = ast_malloc(sizeof(struct stream_job) + strlen(command) + 1);
	job->command = strcpy((void *) (job + 1), command);
	job->next = NULL;
	return job;
}
static inline struct stream_job *stream_layer_take_job(struct stream_layer *layer)
{
	if (!layer->jobs)
		return NULL;

	struct stream_job *job = layer->jobs;
	layer->jobs = job->next;
	if (!layer->jobs)
		layer->last_job = NULL;
	job->next = NULL;
	return job;
}
#define litlen(literal) (sizeof(literal) - sizeof(""))
#define check_param(argument,param_name) (((strlen(argument) > litlen(param_name "=")) && !memcmp (argument, param_name "=", litlen(param_name "="))) ? (argument + litlen(param_name "=")) : NULL)
static char *find_non_escaped_comma(const char *str)
{
	if (*str == ',')
		return (char *) str;
	const char *sep;
	while ((sep = strchr(str, ','))) {
		if (sep[-1] != '\\')
			return (char *) sep;
		str = sep + 1;
	}
	return NULL;
}
static char *find_non_escaped_colon(char *str)
{
	if (*str == ':')
		return str;
	char *sep;
	while ((sep = strchr(str, ':'))) {
		if (sep[-1] != '\\')
			return sep;
		str = sep + 1;
	}
	return NULL;
}
static char *unescape_str_inplace(char *str)
{
	char *src = str;
	char *dst = str;
	char c;
	while ((c = *(src++))) {
		if (c == '\\') {
			c = *(src++);
			switch (c) {
			case '\0':
				*(dst++) = '\\'; /* terminal backslash */
				*dst = '\0';
				return str;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case ':':
			case ',':
			case '(':
			case ')':
				/* \C -> C */
				break;
			default:
				*(dst++) = '\\';
			}
			*dst = c;
		}
		*(dst++) = c;
	}
	*dst = '\0';
	return str;
}
struct parse_say_input_state {
	struct ast_json *root;
};
static inline void parse_say_input_state_free(struct parse_say_input_state *parse_state)
{
	ast_json_unref(parse_state->root);
}
static inline void parse_say_input(struct grpctts_job_input *input, struct parse_say_input_state *parse_state, const char *source)
{
	memset(input, 0, sizeof(struct grpctts_job_input));
	memset(parse_state, 0, sizeof(struct parse_say_input_state));
	struct ast_json_error json_error;
	struct ast_json *root = ast_json_load_string(source, &json_error);
	if (!root) {
		ast_log(AST_LOG_WARNING, "PlayBackground: failed to parse JSON '%s': %s at offset %d\n", source, json_error.text, json_error.position);
		return;
	}
	if (ast_json_typeof(root) != AST_JSON_OBJECT) {
		ast_log(AST_LOG_WARNING, "PlayBackground: failed to parse JSON '%s': root isn't an object\n", source);
		ast_json_unref(root);
		return;
	}
	parse_state->root = root;
	struct ast_json *json;
	if ((json = ast_json_object_get(root, "text")))
		input->text = ast_json_string_get(json);
}
static int parse_play_args(const char **play_filename, const char *arg)
{
	if (*arg != ',') {
		ast_log(AST_LOG_ERROR, "PlayBackground: failed to parse play arguments '%s': expected 3 arguments\n", arg);
		return -1;
	}
	*play_filename = arg + 1;
	return 0;
}
static int parse_sleep_args(struct timespec *timeout, const char *arg)
{
	if (*arg != ',') {
		ast_log(AST_LOG_ERROR, "PlayBackground: failed to parse sleep arguments '%s': expected 3 arguments\n", arg);
		return -1;
	}
	return parse_floating_timespec(timeout, arg + 1);
}
static int parse_say_args(struct grpctts_job_conf *conf, struct grpctts_job_input *input, struct parse_say_input_state *parse_state, char *arg)
{
	char *arg_sep = find_non_escaped_comma(arg);
	if (!arg_sep) {
		ast_log(AST_LOG_ERROR, "PlayBackground: failed to parse say arguments '%s': expected 3 arguments\n", arg);
		return -1;
	}

	*arg_sep = '\0';
	char *eptr;
	while (*arg) {
		char *sep = find_non_escaped_colon(arg);
		if (!sep)
			break;
		*sep = '\0';
		char *str_value;
		if ((str_value = check_param(arg, "pitch"))) {
			double value = strtod(str_value, &eptr);
			if (*str_value && !*eptr) {
				conf->pitch = value;
			} else {
				ast_log(AST_LOG_ERROR, "PlayBackground: failed to parse '%s'\n", arg);
				return -1;
			}
		} else if ((str_value = check_param(arg, "volume_gain_db"))) {
			double value = strtod(str_value, &eptr);
			if (*str_value && !*eptr) {
				conf->volume_gain_db = value;
			} else {
				ast_log(AST_LOG_ERROR, "PlayBackground: failed to parse '%s'\n", arg);
				return -1;
			}
		} else if ((str_value = check_param(arg, "speaking_rate"))) {
			double value = strtod(str_value, &eptr);
			if (*str_value && !*eptr) {
				conf->speaking_rate = value;
			} else {
				ast_log(AST_LOG_ERROR, "PlayBackground: failed to parse '%s'\n", arg);
				return -1;
			}
		} else if ((str_value = check_param(arg, "voice_language_code"))) {
			ast_free(conf->voice_language_code);
			conf->voice_language_code = ast_strdup(unescape_str_inplace(str_value));
		} else if ((str_value = check_param(arg, "voice_name"))) {
			ast_free(conf->voice_name);
			conf->voice_name = ast_strdup(unescape_str_inplace(str_value));
		} else if ((str_value = check_param(arg, "voice_gender"))) {
			if (!strcmp(str_value, "male")) {
				conf->voice_gender = GRPCTTS_VOICE_GENDER_MALE;
			} else if (!strcmp(str_value, "female")) {
				conf->voice_gender = GRPCTTS_VOICE_GENDER_FEMALE;
			} else if (!strcmp(str_value, "neutral")) {
				conf->voice_gender = GRPCTTS_VOICE_GENDER_NEUTRAL;
			} else if (!strcmp(str_value, "unspecified") || !strcmp(str_value, "")) {
				conf->voice_gender = GRPCTTS_VOICE_GENDER_UNSPECIFIED;
			} else {
				ast_log(AST_LOG_ERROR, "PlayBackground: unsupported voice gender '%s'\n", str_value);
				return -1;
			}
		} else if ((str_value = check_param(arg, "remote_frame_format"))) {
			if (!strcmp(str_value, "slin")) {
				conf->remote_frame_format = GRPCTTS_FRAME_FORMAT_SLINEAR16;
			} else if (!strcmp(str_value, "opus")) {
				conf->remote_frame_format = GRPCTTS_FRAME_FORMAT_OPUS;
			} else {
				ast_log(AST_LOG_ERROR, "PlayBackground: unsupported remote frame format '%s'\n", str_value);
				return -1;
			}
		} else {
			ast_log(AST_LOG_ERROR, "PlayBackground: unsupported arg '%s'\n", arg);
			return -1;
		}

		arg = sep + 1;
	}
	*arg_sep = ',';
	arg = arg_sep + 1;

	parse_say_input(input, parse_state, arg);

	return 0;
}
static inline int stream_layer_check_jobs(struct stream_layer *layer, struct stream_state *state, int layer_i, char *error_message, size_t error_message_len)
{
	/* Returns: 1 if there is job taken or already in progress, -1 on job failure, 0 - otherwise */
	if (layer->override) {
		stream_source_stop(&layer->source);
		layer->override = 0;
	}
	if (layer->source.type != STREAM_SOURCE_NONE)
		return 1;
	struct stream_job *job = stream_layer_take_job(layer);
	if (!job)
		return 0;
	char *command = job->command;
	char *sep = strchr(command, ',');
	if (sep) {
		*sep = '\0';
		if (!strcmp(command, "play")) {
			const char *arg = sep + 1;
			const char *play_filename;
			if (parse_play_args(&play_filename, arg)) {
				ast_free(job);
				push_playbackground_duration_event(state->chan, layer_i, 0.0);
				snprintf(error_message, error_message_len, "Invalid play arguments '%s' specified", arg);
				return -1;
			}
			double duration;
			if (stream_source_start_stream_file(&layer->source, play_filename, state, &duration) == -1) {
				snprintf(error_message, error_message_len, "Failed to start streaming file '%s'", play_filename);
				ast_free(job);
				push_playbackground_duration_event(state->chan, layer_i, 0.0);
				return -1;
			}
			push_playbackground_duration_event(state->chan, layer_i, duration);
		} else if (!strcmp(command, "sleep")) {
			const char *arg = sep + 1;
			struct timespec timeout;
			if (parse_sleep_args(&timeout, arg)) {
				snprintf(error_message, error_message_len, "Invalid sleep arguments '%s' specified", arg);
				ast_free(job);
				push_playbackground_duration_event(state->chan, layer_i, 0.0);
				return -1;
			}
			stream_source_start_sleep(&layer->source, &timeout);

			push_playbackground_duration_event(state->chan, layer_i, timeout.tv_sec + timeout.tv_nsec*0.000000001);
		} else if (!strcmp(command, "say")) {
			char *arg = sep + 1;
			struct grpctts_job_conf job_conf;
			grpctts_job_conf_init(&job_conf);
			grpctts_job_conf_cpy(&job_conf, &state->job_conf);
			struct grpctts_job_input job_input;
			struct parse_say_input_state parse_state;
			if (parse_say_args(&job_conf, &job_input, &parse_state, arg)) {
				snprintf(error_message, error_message_len, "Invalid say arguments '%s' specified", arg);
				ast_free(job);
				push_playbackground_duration_event(state->chan, layer_i, 0.0);
				grpctts_job_conf_clear(&job_conf);
				return -1;
			}
			stream_source_start_synthesis(&layer->source, state, &job_conf, &job_input);
			parse_say_input_state_free(&parse_state);
			grpctts_job_conf_clear(&job_conf);
		} else {
			*sep = ',';
			snprintf(error_message, error_message_len, "Unsupported command '%s'", command);
			ast_free(job);
			push_playbackground_duration_event(state->chan, layer_i, 0.0);
			return -1;
		}
	} else {
		snprintf(error_message, error_message_len, "Unsupported command '%s'", command);
		ast_free(job);
		return -1;
	}
	ast_free(job);
	return 1;
}
static inline void stream_layer_drop_jobs(struct stream_layer *layer)
{
	struct stream_job *job;
	while ((job = stream_layer_take_job(layer)))
		ast_free(job);
}
static inline int merge_source_file(struct stream_source *source, short **target_data, int *samples_to_merge)
{
	struct stream_source_file *file = &source->source.file;
	if (!file->buffered_frame)
		stream_source_file_buffer_frame(source);
	if (!file->buffered_frame) {
		stream_source_stop(source);
		return 0;
	}
	int sample_count = file->buffered_frame->samples;
	struct ast_format *format = file->buffered_frame->subclass.format;
	if (format == ast_format_alaw) {
		const uint8_t *source_data = file->buffered_frame->data.ptr;
		while (file->buffered_frame_off < sample_count) {
			if (!*samples_to_merge)
				return 1;
			short sample = AST_ALAW(source_data[file->buffered_frame_off]);
			ast_slinear_saturated_add(*target_data, &sample);
			++(*target_data);
			--(*samples_to_merge);
			++(file->buffered_frame_off);
		}
	} else if (format == ast_format_ulaw) {
		const uint8_t *source_data = file->buffered_frame->data.ptr;
		while (file->buffered_frame_off < sample_count) {
			if (!*samples_to_merge)
				return 1;
			short sample = AST_MULAW(source_data[file->buffered_frame_off]);
			ast_slinear_saturated_add(*target_data, &sample);
			++(*target_data);
			--(*samples_to_merge);
			++(file->buffered_frame_off);
		}
	} else if (format == ast_format_slin) {
		short *source_data = file->buffered_frame->data.ptr;
		while (file->buffered_frame_off < sample_count) {
			if (!*samples_to_merge)
				return 1;
			ast_slinear_saturated_add(*target_data, source_data + file->buffered_frame_off);
			++(*target_data);
			--(*samples_to_merge);
			++(file->buffered_frame_off);
		}
	} else {
		while (file->buffered_frame_off < sample_count) {
			if (!*samples_to_merge)
				return 1;
			++(*target_data);
			--(*samples_to_merge);
			++(file->buffered_frame_off);
		}
	}
	if (file->buffered_frame_off == sample_count) {
		ast_frfree(file->buffered_frame);
		file->buffered_frame = NULL;
	}
	return 1;
}
static inline void merge_source_sleep(struct stream_source *source, int *samples_to_merge)
{
	struct stream_source_sleep *sleep = &source->source.sleep;
	if (sleep->sample_count >= *samples_to_merge) {
		sleep->sample_count -= *samples_to_merge;
		*samples_to_merge = 0;
	} else {
		*samples_to_merge -= sleep->sample_count;
		stream_source_stop(source);
	}
}
static inline int merge_source_synthesis(struct stream_source *source, short **target_data, int *samples_to_merge)
{
	struct stream_source_synthesis *synthesis = &source->source.synthesis;
	if (!synthesis->buffered_frame) {
		if (stream_source_synthesis_buffer_frame(source, *samples_to_merge) == -1) {
			stream_source_stop(source);
			return -1;
		}
	}
	if (!synthesis->buffered_frame) {
		stream_source_stop(source);
		return 0;
	}
	int sample_count = synthesis->buffered_frame->samples;
	short *source_data = synthesis->buffered_frame->data.ptr;
	while (synthesis->buffered_frame_off < sample_count) {
		if (!*samples_to_merge)
			return 1;
		ast_slinear_saturated_add(*target_data, source_data + synthesis->buffered_frame_off);
		++(*target_data);
		--(*samples_to_merge);
		++(synthesis->buffered_frame_off);
	}
	if (synthesis->buffered_frame_off == sample_count) {
		ast_frfree(synthesis->buffered_frame);
		synthesis->buffered_frame = NULL;
	}
	return 1;
}
static inline void stream_layer_merge_frame(struct ast_frame *target_frame, struct stream_layer *layer, struct stream_state *state, int layer_i)
{
	short *target_data = (short *) target_frame->data.ptr;
	int samples_to_merge = target_frame->samples;
	while (samples_to_merge) {
		char error_message[4096];
		int ret = stream_layer_check_jobs(layer, state, layer_i, error_message, sizeof(error_message));
		if (ret == -1) {
			ast_log(AST_LOG_ERROR, "PlayBackground: %s\n", error_message);
			stream_layer_drop_jobs(layer);
			push_playbackground_error_event(state->chan, layer_i, error_message);
			return;
		}
		if (!ret)
			return;
		struct stream_source *source = &layer->source;
		switch (source->type) {
		case STREAM_SOURCE_FILE: {
			if (!merge_source_file(source, &target_data, &samples_to_merge))
				continue;
		} break;
		case STREAM_SOURCE_SLEEP: {
			merge_source_sleep(source, &samples_to_merge);
		} break;
		case STREAM_SOURCE_SYNTHESIS: {
			if (!source->source.synthesis.duration_announced) {
				int64_t duration_samples;
				int ret = stream_source_synthesis_get_duration(source, &duration_samples);
				if (ret == -1) {
					stream_layer_drop_jobs(layer);
					push_playbackground_error_event(state->chan, layer_i, "Failed to get synthesis task duration");
					return;
				}
				if (!ret)
					continue;
				push_playbackground_duration_event(state->chan, layer_i, ((double) duration_samples)/SAMPLE_RATE);
				source->source.synthesis.duration_announced = 1;
			}
			int ret = merge_source_synthesis(source, &target_data, &samples_to_merge);
			if (ret == -1) {
				stream_layer_drop_jobs(layer);
				push_playbackground_error_event(state->chan, layer_i, "Failed to stream synthesis task");
				return;
			}
			if (!ret)
				continue;
		} break;
		default: {
		}
		}
	}
}
static inline int stream_layer_stream_merged_frame(struct stream_layer *layers, int layer_count, struct stream_state *state)
{
	/* Returns: -1 on error or hangup; 0 on frame written; 1 on efd triggered */
	struct ast_channel *chan = state->chan;
	
	/* 1. Make zero frame */
	struct ast_frame *frame = ast_frdup(&zero_frame);;
	if (!frame) {
		ast_log(AST_LOG_ERROR, "Failed to duplicate zero frame\n");
		return -1;
	}
	frame->src = "PlayBackgroundFrame";

	/* 2.1. Collect info about stream business */
	char have_busy_layers_before = 0;
	char *layer_busy_flags = ast_alloca(layer_count);
	{
		int i;
		for (i = 0; i < layer_count; ++i) {
			int busy = layers[i].source.type != STREAM_SOURCE_NONE;
			have_busy_layers_before |= busy;
			layer_busy_flags[i] = busy;
		}
	}
	if (have_busy_layers_before)
		check_start_void_generator(chan);

	/* 2.2. Merge all layers into the frame */
	{
		int i;
		for (i = 0; i < layer_count; ++i)
			stream_layer_merge_frame(frame, &layers[i], state, i);
	}

	/* 2.3. Send events for finished streams */
	char have_busy_layers_after = 0;
	{
		int i;
		for (i = 0; i < layer_count; ++i) {
			int busy = layers[i].source.type != STREAM_SOURCE_NONE;
			have_busy_layers_after |= busy;
			if (layer_busy_flags[i] && !busy)
				push_playbackground_finished_event(state->chan, i);
		}
	}

	/* 3. Wait for deadline */
	if (state->next_frame_time.tv_sec != -1) {
		int ret = wait_for_deadline(&state->next_frame_time, state->efd);
		if (ret) {
			ast_free(frame);
			return ret;
		}
	}

	/* 4. Write frame */
	if (have_busy_layers_before)
		ast_write(chan, frame);
	ast_free(frame);
	if (!have_busy_layers_after)
		check_stop_void_generator(chan);

	/* 5. Update next frame time */
	if (state->next_frame_time.tv_sec == -1) {
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &state->next_frame_time)) {
			ast_log(AST_LOG_ERROR, "Failed to get current time: %s\n", strerror(errno));
			return -1;
		}
	}
	time_add_samples(&state->next_frame_time, ZERO_FRAME_SAMPLES);

	return 0;
}
static inline void stream_layer_clear_jobs(struct stream_layer *layer)
{
	struct stream_job *job;
	while ((job = layer->jobs)) {
		layer->jobs = job->next;
		ast_free(job);
	}
	layer->last_job = NULL;
}


void stream_layer_init(struct stream_layer *layer)
{
	layer->override = 0;
	layer->jobs = NULL;
	layer->last_job = NULL;
	stream_source_init(&layer->source);
}
void stream_layer_uninit(struct stream_layer *layer)
{
	stream_layer_clear_jobs(layer);
	stream_source_stop(&layer->source);
}
void stream_layer_add_job(struct stream_layer *layer, const char *command)
{
	struct stream_job *job = stream_job_create(command);
	if (!job)
		return;
	if (layer->last_job)
		layer->last_job->next = job;
	else
		layer->jobs = job;
	layer->last_job = job;
}
void stream_layer_override(struct stream_layer *layer)
{
	stream_layer_clear_jobs(layer);
	layer->override = 1;
}

void stream_state_init(struct stream_state *state, struct ast_channel *chan, int efd)
{
	state->chan = chan;
	state->tts_channel = NULL;
	grpctts_job_conf_init(&state->job_conf);
	state->efd = efd;
	state->next_frame_time.tv_sec = -1;
}
void stream_state_uninit(struct stream_state *state)
{
	grpctts_job_conf_clear(&state->job_conf);
}

int stream_layers(struct stream_state *state, struct stream_layer *layers, int layer_count)
{
	struct ast_channel *chan = state->chan;
	while (1) {
		int ret = stream_layer_stream_merged_frame(layers, layer_count, state);
		if (ret)
			return ret;
		if (ast_check_hangup_locked(chan))
			break;
	}
	return 0;
}