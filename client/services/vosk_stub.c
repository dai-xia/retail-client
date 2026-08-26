
#include "vosk_capi.h"
#include <stdlib.h>

VoskModel *vosk_model_new(const char *model_path) { return NULL; }
void vosk_model_free(VoskModel *model) { (void)model; }
int vosk_model_find_word(VoskModel *model, const char *word) { (void)model; (void)word; return -1; }

VoskRecognizer *vosk_recognizer_new(VoskModel *model, float sample_rate) { (void)model; (void)sample_rate; return NULL; }
void vosk_recognizer_free(VoskRecognizer *recognizer) { (void)recognizer; }
void vosk_recognizer_reset(VoskRecognizer *recognizer) { (void)recognizer; }
int vosk_recognizer_accept_waveform(VoskRecognizer *recognizer, const char *data, int length) { (void)recognizer; (void)data; (void)length; return 0; }
const char *vosk_recognizer_result(VoskRecognizer *recognizer) { return "{\"text\": \"\"}"; }
const char *vosk_recognizer_partial_result(VoskRecognizer *recognizer) { return "{\"partial\": \"\"}"; }
const char *vosk_recognizer_final_result(VoskRecognizer *recognizer) { return "{\"text\": \"\"}"; }

void vosk_set_log_level(int log_level) { (void)log_level; }
