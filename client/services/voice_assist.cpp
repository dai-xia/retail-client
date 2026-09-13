#include "voice_assist.h"
#include "vosk_capi.h"
#include "core/alsa_capture.h"
#include "core/audio_preproc.h"
#include "core/facemanager.h"
#include <QDebug>
#include <cJSON.h>
#include <QDir>
#include <thread>
#include <chrono>

VoiceAssist::VoiceAssist(QObject *parent)
    : QObject(parent)
    , m_model(nullptr)
    , m_recognizer(nullptr)
    , m_alsaCtx(nullptr)
    , m_alsaReadTimer(nullptr)
    , m_preprocCtx(nullptr)
    , m_lastVadSpeech(false)
    , m_listening(0)
    , m_available(false)
{
}

VoiceAssist::~VoiceAssist()
{
    stopListening();
    destroyRecognizer();
    if (m_preprocCtx) {
        audio_preproc_destroy(&m_preprocCtx);
    }
    if (m_alsaCtx) {
        alsa_capture_close(&m_alsaCtx);
    }
    if (m_model) {
        vosk_model_free(m_model);
        m_model = nullptr;
    }
}

bool VoiceAssist::init(const QString &modelPath)
{
    QDir modelDir(modelPath);
    if (!modelDir.exists()) {
        qWarning() << "[VoiceAssist] Model directory does not exist:" << modelPath;
        return false;
    }

    QStringList requiredFiles = {"am", "conf", "ivector"};
    bool missing = false;
    for (const auto &f : requiredFiles) {
        if (!modelDir.exists(f)) {
            qWarning() << "[VoiceAssist] Missing model file:" << f;
            missing = true;
        }
    }
    if (missing) {
        qWarning() << "[VoiceAssist] Model is incomplete";
        return false;
    }

    m_model = vosk_model_new(modelPath.toUtf8().constData());
    if (!m_model) {
        qWarning() << "[VoiceAssist] Failed to load Vosk model:" << modelPath;
        return false;
    }

    if (!initAlsaCapture()) {
        qWarning() << "[VoiceAssist] ALSA audio capture initialization failed";
        return false;
    }
    qDebug() << "[VoiceAssist] Audio capture backend: native ALSA API";

    m_available = true;

    if (initAudioPreproc()) {
        qDebug() << "[VoiceAssist] Audio preprocessing: NS+AGC+VAD enabled";
    } else {
        qDebug() << "[VoiceAssist] Audio preprocessing: disabled (SpeexDSP unavailable)";
    }

    qDebug() << "[VoiceAssist] Initialization complete, model:" << modelPath;
    return true;
}

/* NS -> AGC -> VAD; AEC disabled (no far-end reference in retail) */
bool VoiceAssist::initAudioPreproc()
{
    audio_preproc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate   = 16000;
    cfg.channels      = 1;
    cfg.frame_size    = 480;     /* 30ms @ 16kHz */
    cfg.enable_aec    = false;   /* no far-end reference needed */
    cfg.enable_ns     = true;
    cfg.enable_agc    = true;
    cfg.enable_vad    = true;
    cfg.vad_threshold = 0.5f;
    cfg.vad_rknn_path = nullptr; /* built-in SpeexDSP VAD */

    m_preprocCtx = audio_preproc_create(&cfg);
    return m_preprocCtx != nullptr;
}

/* 16 kHz S16_LE mono; 1024 frames/period -> 64 ms timer */
bool VoiceAssist::initAlsaCapture()
{
    m_alsaCtx = alsa_capture_open("default", 16000, 1, 1024);
    if (!m_alsaCtx) {
        /* plughw auto-converts to 16 kHz S16_LE mono. Do not use hw:0,0:
         * set_rate_near rewrites to the hardware native rate (e.g. 48 kHz),
         * breaking Vosk's 16 kHz requirement. */
        m_alsaCtx = alsa_capture_open("plughw:0,0", 16000, 1, 1024);
    }

    if (!m_alsaCtx) {
        qWarning() << "[VoiceAssist] ALSA capture initialization failed";
        return false;
    }

    m_alsaReadTimer = new QTimer(this);
    /* period_size=1024, rate=16000 -> 64 ms/period */
    unsigned int rate = alsa_capture_get_rate(m_alsaCtx);
    int intervalMs = (int)(1024 * 1000 / rate);
    m_alsaReadTimer->setInterval(intervalMs);
    connect(m_alsaReadTimer, &QTimer::timeout, this, &VoiceAssist::onAlsaReadTimeout);

    qDebug() << "[VoiceAssist] ALSA capture initialized:"
             << rate << "Hz"
             << alsa_capture_get_channels(m_alsaCtx) << "ch"
             << "interval=" << intervalMs << "ms";
    return true;
}

void VoiceAssist::startListening()
{
    if (!m_available || !m_model) {
        emit signalAssistError("Voice recognition unavailable");
        return;
    }
    if (m_listening.load()) return;

    /* Pause streaming audio so it releases the ALSA device before we take the mic */
    FaceManager::getInstance()->pauseAudioStream();

    {
        int waitMs = 0;
        while (FaceManager::getInstance()->isAudioStreaming() && waitMs < 300) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            waitMs += 20;
        }
        if (waitMs > 0) {
            qDebug() << "[VoiceAssist] Waited for streaming audio to release:" << waitMs << "ms";
        }
    }

    destroyRecognizer();
    m_recognizer = vosk_recognizer_new(m_model, 16000.0f);
    if (!m_recognizer) {
        emit signalAssistError("Failed to create recognizer");
        return;
    }

    m_accumulatedText.clear();
    m_partialText.clear();
    m_listening.store(1);

    startAlsaListening();

    qDebug() << "[VoiceAssist] Start listening on microphone (ALSA)";
}

void VoiceAssist::stopListening()
{
    if (!m_listening.load()) return;
    m_listening.store(0);

    stopAlsaListening();

    /* Resume the streaming audio track */
    FaceManager::getInstance()->resumeAudioStream();

    qDebug() << "[VoiceAssist] Stop listening";
}

void VoiceAssist::startAlsaListening()
{
    if (m_alsaCtx) {
        /* Re-prepare after drop: prepare restarts DMA, otherwise first read returns -EPIPE */
        if (alsa_capture_prepare(m_alsaCtx) < 0) {
            qWarning() << "[VoiceAssist] ALSA prepare failed, capture may be abnormal";
        }
    }
    if (m_alsaReadTimer) {
        m_alsaReadTimer->start();
    }
}

void VoiceAssist::stopAlsaListening()
{
    if (m_alsaReadTimer) {
        m_alsaReadTimer->stop();
    }
    if (m_alsaCtx) {
        /* Stop DMA and discard buffered data; otherwise buffer overruns and next read returns -EPIPE */
        alsa_capture_drop(m_alsaCtx);
    }
}

void VoiceAssist::onAlsaReadTimeout()
{
    if (!m_listening.load() || !m_recognizer || !m_alsaCtx) return;

    const int frames = 1024;  /* period_size */
    char buffer[4096];  /* 1024 frames * 1 channel * 2 bytes = 2048, 4096 is enough */

    int read = alsa_capture_read(m_alsaCtx, buffer, frames);
    if (read > 0) {
        unsigned int channels = alsa_capture_get_channels(m_alsaCtx);
        int dataSize = read * channels * 2;
        processAudioData(buffer, dataSize);
    }
}

/* Final result is ready when accept_waveform returns 1 */
void VoiceAssist::checkVoskResult()
{
    if (!m_recognizer) return;

    const char *resultJson = vosk_recognizer_result(m_recognizer);
    if (resultJson && strlen(resultJson) > 0) {
        cJSON *root = cJSON_Parse(resultJson);
        if (root) {
            cJSON *textItem = cJSON_GetObjectItem(root, "text");
            QString text = textItem ? QString::fromUtf8(textItem->valuestring).trimmed() : QString();
            cJSON_Delete(root);
            if (!text.isEmpty()) {
                m_accumulatedText += text;
                qDebug() << "[VoiceAssist] Recognized:" << text;
                emit signalVoiceRecognized(text);
            }
        }
    }
    vosk_recognizer_reset(m_recognizer);
}

/* Feed PCM (S16_LE, 16 kHz, mono) through NS+AGC+VAD into Vosk */
void VoiceAssist::processAudioData(const char *data, int size)
{
    if (!m_recognizer || !data || size <= 0) return;

    if (m_preprocCtx) {
        const int frame_size = 480;  /* Same as in initAudioPreproc, 30 ms @ 16 kHz */
        const int frame_bytes = frame_size * (int)sizeof(int16_t);
        const int16_t *src = (const int16_t *)data;
        int remaining = size / (int)sizeof(int16_t);

        while (remaining >= frame_size) {
            int16_t out_buf[480];
            audio_vad_result_t vad = audio_preproc_process(
                m_preprocCtx, src, nullptr, out_buf);

            bool isSpeech = (vad == AUDIO_VAD_SPEECH);
            if (isSpeech != m_lastVadSpeech) {
                m_lastVadSpeech = isSpeech;
                emit signalVadSpeech(isSpeech);
            }

            /* Only feed speech frames into Vosk; skip silence frames to reduce wasted inference */
            if (isSpeech) {
                int ret = vosk_recognizer_accept_waveform(m_recognizer,
                    (const char *)out_buf, frame_bytes);
                if (ret > 0) {
                    checkVoskResult();
                }
            }

            src += frame_size;
            remaining -= frame_size;
        }

        /* Feed leftover (<1 frame) directly to Vosk */
        if (remaining > 0) {
            int ret = vosk_recognizer_accept_waveform(m_recognizer,
                (const char *)src, remaining * (int)sizeof(int16_t));
            if (ret > 0) {
                checkVoskResult();
            }
        }
    } else {
        int ret = vosk_recognizer_accept_waveform(m_recognizer, data, size);
        if (ret > 0) {
            checkVoskResult();
        }
    }

    const char *partialJson = vosk_recognizer_partial_result(m_recognizer);
    if (partialJson && strlen(partialJson) > 0) {
        cJSON *root = cJSON_Parse(partialJson);
        if (root) {
            cJSON *partialItem = cJSON_GetObjectItem(root, "partial");
            QString partial = partialItem ? QString::fromUtf8(partialItem->valuestring) : QString();
            cJSON_Delete(root);
            if (!partial.isEmpty() && partial != m_partialText) {
                m_partialText = partial;
                emit signalVoicePartial(partial);
            }
        }
    }
}

void VoiceAssist::finalizeResult()
{
    if (!m_recognizer) return;

    const char *finalJson = vosk_recognizer_final_result(m_recognizer);
    if (finalJson && strlen(finalJson) > 0) {
        cJSON *root = cJSON_Parse(finalJson);
        if (root) {
            cJSON *textItem = cJSON_GetObjectItem(root, "text");
            QString text = textItem ? QString::fromUtf8(textItem->valuestring) : QString();
            cJSON_Delete(root);
            if (!text.isEmpty()) {
                m_accumulatedText += text;
                if (!m_accumulatedText.trimmed().isEmpty()) {
                    qDebug() << "[VoiceAssist] Final recognition:" << m_accumulatedText.trimmed();
                    emit signalVoiceRecognized(m_accumulatedText.trimmed());
                }
            }
        }
    }

    destroyRecognizer();
}

void VoiceAssist::destroyRecognizer()
{
    if (m_recognizer) {
        vosk_recognizer_free(m_recognizer);
        m_recognizer = nullptr;
    }
}
