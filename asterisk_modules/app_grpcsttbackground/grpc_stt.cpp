/*
 * Asterisk VoiceKit modules
 *
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

extern "C" struct ast_module *AST_MODULE_SELF_SYM(void);
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_SYM

#define typeof __typeof__
#include "stt.grpc.pb.h"
#include "roots.pem.h"
#include "grpc_stt.h"
#include "jwt.h"

#include <chrono>
#include <iostream>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
extern "C" {
#include <asterisk.h>
#include <asterisk/autoconfig.h>
#include <asterisk/compiler.h>
#include <asterisk/time.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/format_cache.h>
#include <asterisk/alaw.h>
#include <asterisk/ulaw.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <jansson.h>
}


// 7 days
#define EXPIRATION_PERIOD (7*86400)

#define INTERNAL_SAMPLE_RATE 8000
#define MAX_FRAME_DURATION_MSEC 100
#define MAX_FRAME_SAMPLES 800
#define ALIGNMENT_SAMPLES 80


static const std::string grpc_roots_pem_string ((const char *) grpc_roots_pem, sizeof(grpc_roots_pem));


static inline int delta_samples(const struct timespec *a, const struct timespec *b)
{
	struct timespec delta;
	delta.tv_sec = a->tv_sec - b->tv_sec;
	delta.tv_nsec = a->tv_nsec - b->tv_nsec;
	if (delta.tv_nsec < 0) {
		delta.tv_sec--;
		delta.tv_nsec += 1000000000;
	}
	
	return delta.tv_sec*INTERNAL_SAMPLE_RATE + ((int64_t) delta.tv_nsec)*INTERNAL_SAMPLE_RATE/1000000000;
}
static inline void time_add_samples(struct timespec *t, int samples)
{
	t->tv_sec += samples/INTERNAL_SAMPLE_RATE;
	t->tv_nsec += ((int64_t) (samples%INTERNAL_SAMPLE_RATE))*1000000000/INTERNAL_SAMPLE_RATE;
	if (t->tv_nsec >= 1000000000) {
		t->tv_sec++;
		t->tv_nsec -= 1000000000;
	}
}
static inline int aligned_samples(int samples)
{
	return (samples + ALIGNMENT_SAMPLES/2)/ALIGNMENT_SAMPLES*ALIGNMENT_SAMPLES;
}
static inline void eventfd_skip(int fd)
{
	eventfd_t value;
	read(fd, &value, sizeof(eventfd_t));
}
static json_t *build_json_duration(const google::protobuf::Duration &duration)
{
	json_t *json_duration = json_object();
	json_object_set_new_nocheck(json_duration, "seconds", json_real(duration.seconds()));
	json_object_set_new_nocheck(json_duration, "nanos", json_real(duration.nanos()));
	return json_duration;
}
static json_t *build_json_gender_identification_result(const float male_proba, const float female_proba)
{
	json_t *json_gender_identification = json_object();
	json_object_set_new_nocheck(json_gender_identification, "male_proba", json_real(male_proba));
	json_object_set_new_nocheck(json_gender_identification, "female_proba", json_real(female_proba));
	return json_gender_identification;
}
static std::string build_grpcstt_event(struct ast_channel *chan, const voiptime::cloud::stt::v1::StreamingRecognitionResult &stream_result, bool json_ensure_ascii)
{
	const voiptime::cloud::stt::v1::SpeechRecognitionResult &recognition_result = stream_result.recognition_result();
	json_t *json_root = json_object();
	const char *variable_name = "ai_voicemail";
    const char *variable_value = pbx_builtin_getvar_helper(chan, variable_name);
	{
		json_t *json_alternatives = json_array();
		for (const voiptime::cloud::stt::v1::SpeechRecognitionAlternative &alternative: recognition_result.alternatives()) {
			json_t *json_alternative = json_object();
			json_object_set_new_nocheck(json_alternative, "transcript", json_string(alternative.transcript().c_str()));
			json_object_set_new_nocheck(json_alternative, "confidence", json_real(alternative.confidence()));
			json_array_append_new(json_alternatives, json_alternative);
		}
		json_object_set_new_nocheck(json_root, "alternatives", json_alternatives);
	}
	json_object_set_new_nocheck(json_root, "is_final", json_boolean(stream_result.is_final()));
	json_object_set_new_nocheck(json_root, "stability", json_real(stream_result.stability()));
    json_object_set_new_nocheck(json_root, "request_uuid", json_string(stream_result.request_uuid().c_str()));
    json_object_set_new_nocheck(json_root, "configuration", json_string(variable_value));
	json_object_set_new_nocheck(json_root, "start_time", build_json_duration(recognition_result.start_time()));
	json_object_set_new_nocheck(json_root, "end_time", build_json_duration(recognition_result.end_time()));

	const voiptime::cloud::stt::v1::SpeechGenderIdentificationResult &gender_identification_result = recognition_result.gender_identification_result();
	const float male_proba = gender_identification_result.male_proba();
	const float female_proba = gender_identification_result.female_proba();
	if (!(male_proba == 0 && female_proba == 0)) {
		json_object_set_new_nocheck(json_root, "gender_identification_result", build_json_gender_identification_result(male_proba, female_proba));
	}

	char *dump = json_dumps(json_root, (json_ensure_ascii ? (JSON_COMPACT | JSON_ENSURE_ASCII) : (JSON_COMPACT)));
	std::string result(dump);
	ast_json_free(dump);
	json_decref(json_root);
	return result;
}
static void push_grpcstt_event(struct ast_channel *chan, const std::string &data, bool ensure_ascii)
{
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", (ensure_ascii ? "SpeechRecognition" : "SpeechRecognition"), "eventbody", data.c_str());
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);

	ast_json_unref(blob);
}
static void push_grpcstt_x_request_id_event(struct ast_channel *chan, const std::string &data)
{
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", "SpeechRequest", "eventbody", data.c_str());
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);

	ast_json_unref(blob);
}
static void push_grpcstt_session_finished_event(struct ast_channel *chan, bool success, int error_code, const std::string &error_message)
{
	std::string data = success ? "{\"status\": \"SUCCESS\"}" : ("{\"status\": \"FAILURE\", \"code\":" + std::to_string(error_code) + ", \"message\":\"" + error_message + "\"}");
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", "SpeechSession", "eventbody", data.c_str());
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);

	ast_json_unref(blob);
}
static const char *get_frame_samples(struct ast_frame *f, enum grpc_stt_frame_format frame_format, std::vector<uint8_t> &buffer, size_t *len, bool *warned)
{
	size_t sample_count = f->samples;
	const char *data = NULL;

	switch (frame_format) {
	case GRPC_STT_FRAME_FORMAT_SLINEAR16: {
		*len = sample_count*sizeof(int16_t);
		if (f->subclass.format == ast_format_alaw) {
			buffer.resize(sample_count*sizeof(int16_t));
			int16_t *dptr = (int16_t *) buffer.data();
			uint8_t *sptr = (uint8_t *) f->data.ptr;
			for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
				int16_t slin_sample = AST_ALAW(*sptr);
				*dptr = htole16(slin_sample);
			}
			data = (const char *) buffer.data();
		} else if (f->subclass.format == ast_format_ulaw) {
			buffer.resize(sample_count*sizeof(int16_t));
			int16_t *dptr = (int16_t *) buffer.data();
			uint8_t *sptr = (uint8_t *) f->data.ptr;
			for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
				int16_t slin_sample = AST_MULAW(*sptr);
				*dptr = htole16(slin_sample);
			}
			data = (const char *) buffer.data();
		} else if (f->subclass.format == ast_format_slin) {
			data = (const char *) f->data.ptr;
		} else {
			if (!warned) {
				ast_log(AST_LOG_WARNING, "Unhandled frame format, ignoring!\n");
				*warned = true;
			}
		}
	} break;
	case GRPC_STT_FRAME_FORMAT_MULAW: {
		*len = sample_count;
		if (f->subclass.format == ast_format_alaw) {
			buffer.resize(sample_count);
			uint8_t *dptr = buffer.data();
			uint8_t *sptr = (uint8_t *) f->data.ptr;
			for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
				int16_t slin_sample = AST_ALAW(*sptr);
				*dptr = AST_LIN2MU(slin_sample);
			}
			data = (const char *) buffer.data();
		} else if (f->subclass.format == ast_format_ulaw) {
			data = (const char *) f->data.ptr;
		} else if (f->subclass.format == ast_format_slin) {
			buffer.resize(sample_count);
			uint8_t *dptr = buffer.data();
			int16_t *sptr = (int16_t *) f->data.ptr;
			for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
				int16_t slin_sample = le16toh(*sptr);
				*dptr = AST_LIN2MU(slin_sample);
			}
			data = (const char *) buffer.data();
		} else {
			if (!warned) {
				ast_log(AST_LOG_WARNING, "Unhandled frame format, ignoring!\n");
				*warned = true;
			}
		}
	} break;
	default: /* GRPC_STT_FRAME_FORMAT_ALAW */ {
		*len = sample_count;
		if (f->subclass.format == ast_format_alaw) {
			data = (const char *) f->data.ptr;
		} else if (f->subclass.format == ast_format_ulaw) {
			buffer.resize(sample_count);
			uint8_t *dptr = buffer.data();
			uint8_t *sptr = (uint8_t *) f->data.ptr;
			for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
				int16_t slin_sample = AST_MULAW(*sptr);
				*dptr = AST_LIN2A(slin_sample);
			}
			data = (const char *) buffer.data();
		} else if (f->subclass.format == ast_format_slin) {
			buffer.resize(sample_count);
			uint8_t *dptr = buffer.data();
			int16_t *sptr = (int16_t *) f->data.ptr;
			for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
				int16_t slin_sample = le16toh(*sptr);
				*dptr = AST_LIN2A(slin_sample);
			}
			data = (const char *) buffer.data();
		} else {
			if (!warned) {
				ast_log(AST_LOG_WARNING, "Unhandled frame format, ignoring!\n");
				*warned = true;
			}
		}
	}
	}

	return data;
}
static std::vector<uint8_t> make_silence_samples(enum grpc_stt_frame_format frame_format, size_t samples)
{
	switch (frame_format) {
	case GRPC_STT_FRAME_FORMAT_SLINEAR16:
		return std::vector<uint8_t>(samples*sizeof(int16_t), 0);
	case GRPC_STT_FRAME_FORMAT_MULAW:
		return std::vector<uint8_t>(samples, 0x7F /* SLINEAR16 (0) */);
	default: /* GRPC_STT_FRAME_FORMAT_ALAW */
		return std::vector<uint8_t>(samples, 0xD5 /* SLINEAR16 (8) */);
	}
}


AST_LIST_HEAD(grpcstt_frame_list, ast_frame);

class GRPCSTT
{
public:
	static void AttachToChannel(std::shared_ptr<GRPCSTT> &grpc_stt);
	static void DetachFromChannel(std::shared_ptr<GRPCSTT> &grpc_stt) noexcept;

public:
	GRPCSTT(int terminate_event_fd, std::shared_ptr<grpc::Channel> grpc_channel,
		const char *authorization_api_key, const char *authorization_secret_key,
		const char *authorization_issuer, const char *authorization_subject, const char *authorization_audience,
		struct ast_channel *chan,
		const char *language_code, int max_alternatives, enum grpc_stt_frame_format frame_format,
		bool vad_disable, double vad_min_speech_duration, double vad_max_speech_duration,
		double vad_silence_duration_threshold, double vad_silence_prob_threshold, double vad_aggressiveness,
		bool interim_results_enable, double interim_results_max_interval, int interim_results_max_predictions,
		bool enable_gender_identification);
	~GRPCSTT();
	void ReapAudioFrame(struct ast_frame *frame);
	void Terminate() noexcept;
	bool Run(int &error_status, std::string &error_message);

private:
	int terminate_event_fd;
	std::unique_ptr<voiptime::cloud::stt::v1::SpeechToText::Stub> stt_stub;
	std::string authorization_api_key;
	std::string authorization_secret_key;
	std::string authorization_issuer;
	std::string authorization_subject;
	std::string authorization_audience;
	struct ast_channel *chan;
	std::string language_code;
	int max_alternatives;
	enum grpc_stt_frame_format frame_format;
	int frame_event_fd;
	struct grpcstt_frame_list audio_frames;
	int framehook_id;
	bool vad_disable;
	double vad_min_speech_duration;
	double vad_max_speech_duration;
	double vad_silence_duration_threshold;
	double vad_silence_prob_threshold;
	double vad_aggressiveness;
	bool interim_results_enable;
	double interim_results_max_interval;
	int interim_results_max_predictions;
	bool enable_gender_identification;
};


static struct ast_frame *framehook_event_callback (struct ast_channel *chan, struct ast_frame *frame, enum ast_framehook_event event, void *data)
{
	if (frame) {
		if (event == AST_FRAMEHOOK_EVENT_READ)
			(*(std::shared_ptr<GRPCSTT>*) data)->ReapAudioFrame(frame);
	}

	return frame;
}
static int framehook_consume_callback (void *data, enum ast_frame_type type)
{
	return 0;
}
static void framehook_destroy_callback (void *data)
{
	(*(std::shared_ptr<GRPCSTT>*) data)->Terminate();
	delete (std::shared_ptr<GRPCSTT>*) data;
}

const char* get_voiptime_value_for_key(const char* input, const char* key) {
    const char* delimiter = "=";
    const char* token = std::strstr(input, key);
    if (token == nullptr) {
        return nullptr;
    }
    token = std::strchr(token, '=') + 1;
    const char* endToken = std::strchr(token, ';');
    if (endToken == nullptr) {
        endToken = std::strchr(token, '\0');
    }
    size_t valueLength = endToken - token;
    char* value = new char[valueLength + 1];
    std::strncpy(value, token, valueLength);
    value[valueLength] = '\0';
    return value;
}

void GRPCSTT::AttachToChannel(std::shared_ptr<GRPCSTT> &grpc_stt)
{
	struct ast_framehook_interface interface = {.version = AST_FRAMEHOOK_INTERFACE_VERSION};
	interface.event_cb = framehook_event_callback;
	interface.consume_cb = framehook_consume_callback;
	interface.destroy_cb = framehook_destroy_callback;
	interface.data = (void*) new std::shared_ptr<GRPCSTT>(grpc_stt);
	ast_channel_lock(grpc_stt->chan);
	int id = ast_framehook_attach(grpc_stt->chan, &interface);
	ast_channel_unlock(grpc_stt->chan);
	grpc_stt->framehook_id = id;
}
void GRPCSTT::DetachFromChannel(std::shared_ptr<GRPCSTT> &grpc_stt) noexcept
{
	int id = grpc_stt->framehook_id;
	if (id == -1)
		return;
	ast_channel_lock(grpc_stt->chan);
	ast_framehook_detach(grpc_stt->chan, id);
	ast_channel_unlock(grpc_stt->chan);
	grpc_stt->framehook_id = -1;
}
GRPCSTT::GRPCSTT(int terminate_event_fd, std::shared_ptr<grpc::Channel> grpc_channel,
		 const char *authorization_api_key, const char *authorization_secret_key,
		 const char *authorization_issuer, const char *authorization_subject, const char *authorization_audience,
		 struct ast_channel *chan, const char *language_code, int max_alternatives, enum grpc_stt_frame_format frame_format,
		 bool vad_disable, double vad_min_speech_duration, double vad_max_speech_duration,
		 double vad_silence_duration_threshold, double vad_silence_prob_threshold, double vad_aggressiveness,
		 bool interim_results_enable, double interim_results_max_interval, int interim_results_max_predictions,
		 bool enable_gender_identification)
	: terminate_event_fd(terminate_event_fd), stt_stub(voiptime::cloud::stt::v1::SpeechToText::NewStub(grpc_channel)),
	authorization_api_key(authorization_api_key), authorization_secret_key(authorization_secret_key),
	authorization_issuer(authorization_issuer), authorization_subject(authorization_subject), authorization_audience(authorization_audience),
	chan(chan), language_code(language_code), max_alternatives(max_alternatives), frame_format(frame_format), framehook_id(-1),
	vad_disable(vad_disable), vad_min_speech_duration(vad_min_speech_duration), vad_max_speech_duration(vad_max_speech_duration),
	vad_silence_duration_threshold(vad_silence_duration_threshold), vad_silence_prob_threshold(vad_silence_prob_threshold), vad_aggressiveness(vad_aggressiveness),
	interim_results_enable(interim_results_enable), interim_results_max_interval(interim_results_max_interval),
	interim_results_max_predictions(interim_results_max_predictions),
	enable_gender_identification(enable_gender_identification)
{
	frame_event_fd = eventfd(0, 0);
	fcntl(frame_event_fd, F_SETFL, fcntl(frame_event_fd, F_GETFL) | O_NONBLOCK);
	AST_LIST_HEAD_INIT(&audio_frames);
}
GRPCSTT::~GRPCSTT()
{
	close(frame_event_fd);

	AST_LIST_LOCK(&audio_frames);
	struct ast_frame *f;
	while ((f = AST_LIST_REMOVE_HEAD(&audio_frames, frame_list)))
		ast_frame_dtor(f);
	AST_LIST_UNLOCK(&audio_frames);
}
void GRPCSTT::ReapAudioFrame(struct ast_frame *frame)
{
	struct ast_frame *f = ast_frdup(frame);
	if (!f)
		return;

	AST_LIST_LOCK(&audio_frames);
	AST_LIST_INSERT_TAIL(&audio_frames, f, frame_list);
	AST_LIST_UNLOCK(&audio_frames);

	eventfd_write(frame_event_fd, 1);
}
void GRPCSTT::Terminate() noexcept
{
	eventfd_write(terminate_event_fd, 1);
}
bool GRPCSTT::Run(int &error_status, std::string &error_message)
{
	error_status = 0;
	error_message = "";

    const char *variable_configuration = "ai_voicemail";
    const char *variable_configuration_value = pbx_builtin_getvar_helper(chan, variable_configuration);

    authorization_api_key = get_voiptime_value_for_key(variable_configuration_value, "access_token");

	grpc::ClientContext context;
	if (authorization_api_key.size() && authorization_secret_key.size() &&
	    authorization_issuer.size() && authorization_subject.size() && authorization_audience.size()) {
		int64_t expiration_time_sec = time(NULL) + EXPIRATION_PERIOD;

		std::string jwt = "Bearer " + GenerateJWT(
			authorization_api_key, authorization_secret_key,
			authorization_issuer, authorization_subject, authorization_audience,
			expiration_time_sec);
		context.AddMetadata("authorization", jwt);
	}

	std::shared_ptr<grpc::ClientReaderWriter<voiptime::cloud::stt::v1::StreamingRecognizeRequest,
						 voiptime::cloud::stt::v1::StreamingRecognizeResponse>> stream(stt_stub->StreamingRecognize(&context));


	try {
		std::thread writer(
			[stream, &variable_configuration_value, this]()
			{
				{
					voiptime::cloud::stt::v1::StreamingRecognizeRequest initial_request;
					voiptime::cloud::stt::v1::StreamingRecognitionConfig *streaming_recognition_config = initial_request.mutable_streaming_config();
					{
						voiptime::cloud::stt::v1::RecognitionConfig *recognition_config = streaming_recognition_config->mutable_config();
						switch (frame_format) {
						case GRPC_STT_FRAME_FORMAT_SLINEAR16:
							recognition_config->set_encoding(voiptime::cloud::stt::v1::LINEAR16);
							break;
						case GRPC_STT_FRAME_FORMAT_MULAW:
							recognition_config->set_encoding(voiptime::cloud::stt::v1::MULAW);
							break;
						default:
							recognition_config->set_encoding(voiptime::cloud::stt::v1::ALAW);
						}
						recognition_config->set_sample_rate_hertz(INTERNAL_SAMPLE_RATE);
						recognition_config->set_num_channels(1);
						if (language_code.size())
							recognition_config->set_language_code(language_code);
						const char *variable_name = "MACRO_EXTEN";
                        const char *variable_value = pbx_builtin_getvar_helper(chan, variable_name);
						recognition_config->set_channel_exten(variable_value);
						const int company_id = atoi(get_voiptime_value_for_key(variable_configuration_value, "company_id"));
                        const int campaign_id = atoi(get_voiptime_value_for_key(variable_configuration_value, "campaign_id"));
                        const int application_id = atoi(get_voiptime_value_for_key(variable_configuration_value, "application_id"));
                        const int statistic_id = atoi(get_voiptime_value_for_key(variable_configuration_value, "statistic_id"));
                        const char *request_uuid = get_voiptime_value_for_key(variable_configuration_value, "request_uuid");
						recognition_config->set_company_id(company_id);
						recognition_config->set_campaign_id(campaign_id);
						recognition_config->set_application_id(application_id);
						recognition_config->set_statistic_id(statistic_id);
						recognition_config->set_request_uuid(request_uuid);
						recognition_config->set_max_alternatives(max_alternatives);
						if (vad_disable) {
							recognition_config->set_do_not_perform_vad(true);
						} else {
							voiptime::cloud::stt::v1::VoiceActivityDetectionConfig *vad_config = recognition_config->mutable_vad_config();
							vad_config->set_min_speech_duration(vad_min_speech_duration);
							vad_config->set_max_speech_duration(vad_max_speech_duration);
							vad_config->set_silence_duration_threshold(vad_silence_duration_threshold);
							vad_config->set_silence_prob_threshold(vad_silence_prob_threshold);
							vad_config->set_aggressiveness(vad_aggressiveness);
						}
						recognition_config->set_enable_gender_identification(enable_gender_identification);
					}
					{
						voiptime::cloud::stt::v1::InterimResultsConfig *interim_results_config = streaming_recognition_config->mutable_interim_results_config();
						interim_results_config->set_enable_interim_results(interim_results_enable);
						interim_results_config->set_interval(interim_results_max_interval);
						interim_results_config->set_max_predictions(interim_results_max_predictions);
					}
					stream->Write(initial_request);
				}
			}
		);
		writer.join();

		stream->WaitForInitialMetadata();
		const std::multimap<grpc::string_ref, grpc::string_ref> &metadata = context.GetServerInitialMetadata();
		std::multimap<grpc::string_ref, grpc::string_ref>::const_iterator x_request_id_it = metadata.find("x-request-id");
		push_grpcstt_x_request_id_event(chan, (x_request_id_it != metadata.end()) ? std::string(x_request_id_it->second.data(), x_request_id_it->second.size()) : "");
	} catch (const std::exception &ex) {
		error_status = -1;
		error_message = std::string("GRPC STT finished with error: ") + ex.what();
		return false;
	} catch (...) {
		error_status = -1;
		error_message = "GRPC STT finished with unknown error";
		return false;
	}

	std::thread writer(
		[stream, this]()
		{
			bool stream_valid = true;
			bool warned = false;
			struct timespec last_frame_moment;
			clock_gettime(CLOCK_MONOTONIC_RAW, &last_frame_moment);
			while (stream_valid && !ast_check_hangup_locked(chan)) {
				struct pollfd pfds[2] = {
					{
						.fd = terminate_event_fd,
						.events = POLLIN,
						.revents = 0,
					},
					{
						.fd = frame_event_fd,
						.events = POLLIN,
						.revents = 0,
					},
				};
				poll(pfds, 2, MAX_FRAME_DURATION_MSEC*2);
				if (pfds[0].revents & POLLIN)
					break;

				if (!(pfds[1].revents & POLLIN)) {
					struct timespec current_moment;
					clock_gettime(CLOCK_MONOTONIC_RAW, &current_moment);
					int gap_samples = aligned_samples(delta_samples(&current_moment, &last_frame_moment) - MAX_FRAME_SAMPLES);
					if (gap_samples > 0) {
//					    ast_log(LOG_WARNING, "Stream GAP SAMPLES specified\n");
						voiptime::cloud::stt::v1::StreamingRecognizeRequest request;
						std::vector<uint8_t> buffer = make_silence_samples(frame_format, gap_samples);
						request.set_audio_content(buffer.data(), buffer.size());
						if (!stream->Write(request))
							stream_valid = false;
						time_add_samples(&last_frame_moment, gap_samples);
					}
					continue;
				}

				eventfd_skip(frame_event_fd);
				bool gap_handled = false;
				while (stream_valid) {
//				    ast_log(LOG_WARNING, "Stream valid specified\n");
					AST_LIST_LOCK(&audio_frames);
					struct ast_frame *f = AST_LIST_REMOVE_HEAD(&audio_frames, frame_list);
					AST_LIST_UNLOCK(&audio_frames);
					if (!f) {
//					    ast_log(LOG_WARNING, "Not f\n");
					    break;
					}
//                    ast_log(LOG_WARNING, "Stream after valid specified\n");
					if (f->frametype == AST_FRAME_VOICE) {
//					    ast_log(LOG_WARNING, "Frame voice specified\n");
						struct timespec current_moment;
						clock_gettime(CLOCK_MONOTONIC_RAW, &current_moment);
						if (!gap_handled) {
							int gap_samples = aligned_samples(delta_samples(&current_moment, &last_frame_moment) - f->samples);
							if (gap_samples > 0) {
								voiptime::cloud::stt::v1::StreamingRecognizeRequest request;
								std::vector<uint8_t> buffer = make_silence_samples(frame_format, gap_samples);
								request.set_audio_content(buffer.data(), buffer.size());
								if (!stream->Write(request))
									stream_valid = false;
								time_add_samples(&last_frame_moment, gap_samples);
							}
							gap_handled = true;
						}

						voiptime::cloud::stt::v1::StreamingRecognizeRequest request;
						std::vector<uint8_t> buffer;
						size_t len = 0;
						const char *data = get_frame_samples(f, frame_format, buffer, &len, &warned);
						if (data) {
//						    ast_log(LOG_WARNING, "Data voice specified\n");
							time_add_samples(&last_frame_moment, f->samples);
							request.set_audio_content(data, len);
							if (!stream->Write(request))
                                stream_valid = false;
						}
					}

					ast_frame_dtor(f);
				}
			}

			stream->WritesDone();
		}
	);

	try {
		voiptime::cloud::stt::v1::StreamingRecognizeResponse response;
		while (stream->Read(&response)) {
//		    ast_log(LOG_WARNING, "RESPONSE received\n");
			for (const voiptime::cloud::stt::v1::StreamingRecognitionResult &stream_result: response.results()) {
				push_grpcstt_event(chan, build_grpcstt_event(chan, stream_result, false), false);
//				push_grpcstt_event(chan, build_grpcstt_event(stream_result, true), true);
			}
		}
	} catch (const std::exception &ex) {
		Terminate();
		writer.join();
		error_status = -1;
		error_message = std::string("GRPC STT finished with error: ") + ex.what();
		return false;
	} catch (...) {
		Terminate();
		writer.join();
		error_status = -1;
		error_message = "GRPC STT finished with unknown error";
		return false;
	}
	Terminate();
	writer.join();
	grpc::Status status = stream->Finish();
	if (!status.ok()) {
		error_status = status.error_code();
		error_message = "GRPC STT finished with error (code = " + std::to_string(status.error_code()) + "): " + std::string(status.error_message());
		return false;
	}
	return true;
}


extern "C" void grpc_stt_run(int terminate_event_fd, const char *endpoint, const char *authorization_api_key, const char *authorization_secret_key,
			     const char *authorization_issuer, const char *authorization_subject, const char *authorization_audience,
			     struct ast_channel *chan, int ssl_grpc, const char *ca_data,
			     const char *language_code, int max_alternatives, enum grpc_stt_frame_format frame_format,
			     int vad_disable, double vad_min_speech_duration, double vad_max_speech_duration,
			     double vad_silence_duration_threshold, double vad_silence_prob_threshold, double vad_aggressiveness,
			     int interim_results_enable, double interim_results_max_interval, int interim_results_max_predictions,
			     int enable_gender_identification)
{
	bool success = false;
	int error_status;
	std::string error_message;
	try {
		grpc::SslCredentialsOptions ssl_credentials_options = {
			.pem_root_certs = ca_data ? ca_data : grpc_roots_pem_string,
		};
#define NON_NULL_STRING(str) ((str) ? (str) : "")
		std::shared_ptr<GRPCSTT> grpc_stt = std::make_shared<GRPCSTT>(
			terminate_event_fd,
			grpc::CreateChannel(endpoint, (ssl_grpc ? grpc::SslCredentials(ssl_credentials_options) : grpc::InsecureChannelCredentials())),
			NON_NULL_STRING(authorization_api_key), NON_NULL_STRING(authorization_secret_key),
			NON_NULL_STRING(authorization_issuer), NON_NULL_STRING(authorization_subject), NON_NULL_STRING(authorization_audience),
			chan, (language_code ? language_code : ""), max_alternatives, frame_format,
			vad_disable, vad_min_speech_duration, vad_max_speech_duration,
			vad_silence_duration_threshold, vad_silence_prob_threshold, vad_aggressiveness,
			interim_results_enable, interim_results_max_interval, interim_results_max_predictions,
			enable_gender_identification
		);
#undef NON_NULL_STRING
		GRPCSTT::AttachToChannel(grpc_stt);
		try {
			success = grpc_stt->Run(error_status, error_message);
			GRPCSTT::DetachFromChannel(grpc_stt);
		} catch (...) {
			GRPCSTT::DetachFromChannel(grpc_stt);
			throw;
		}
	} catch (const std::exception &ex) {
		error_status = -1;
		error_message = std::string("GRPCSTTBackgrond background thread finished with exception: ") + ex.what();
	}
	if (!success)
		ast_log(AST_LOG_ERROR, "%s\n", error_message.c_str());
	push_grpcstt_session_finished_event(chan, success, error_status, error_message);
}
