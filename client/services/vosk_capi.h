#ifndef VOSK_CAPI_H
#define VOSK_CAPI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VoskModel VoskModel;
typedef struct VoskRecognizer VoskRecognizer;

VoskModel      *vosk_model_new(const char *model_path);
void            vosk_model_free(VoskModel *model);
int             vosk_model_find_word(VoskModel *model, const char *word);

VoskRecognizer *vosk_recognizer_new(VoskModel *model, float sample_rate);
void            vosk_recognizer_free(VoskRecognizer *recognizer);
void            vosk_recognizer_reset(VoskRecognizer *recognizer);
int             vosk_recognizer_accept_waveform(VoskRecognizer *recognizer, const char *data, int length);
const char     *vosk_recognizer_result(VoskRecognizer *recognizer);
const char     *vosk_recognizer_partial_result(VoskRecognizer *recognizer);
const char     *vosk_recognizer_final_result(VoskRecognizer *recognizer);

void            vosk_set_log_level(int log_level);

#ifdef __cplusplus
}
#endif

#endif
