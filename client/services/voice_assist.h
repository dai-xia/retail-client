#ifndef VOICE_ASSIST_H
#define VOICE_ASSIST_H

#include <QObject>
#include <QString>
#include <QThread>
#include <QAtomicInt>
#include <QTimer>

// Forward declarations for Vosk voice model and recognizer
struct VoskModel;
struct VoskRecognizer;

// Forward declaration for ALSA capture context
typedef struct alsa_capture_ctx alsa_capture_t;

// Forward declaration for audio preprocessing context
typedef struct audio_preproc_ctx audio_preproc_t;

/**
 * @brief Offline voice assistant, implements audio capture and speech recognition based on Vosk
 *
 * Audio capture: native ALSA API -> audio preprocessing -> Vosk
 *
 * Audio preprocessing pipeline (audio_preproc):
 *   NS (denoise) -> AGC (auto gain) -> VAD (endpoint detection)
 *   Denoise removes ambient noise, AGC compensates volume, VAD decides whether someone is speaking,
 *   and only speech frames are sent to Vosk for recognition, reducing wasted inference and improving response speed.
 */
class VoiceAssist : public QObject
{
    Q_OBJECT

public:
    explicit VoiceAssist(QObject *parent = nullptr);
    ~VoiceAssist();

    bool init(const QString &modelPath);
    bool isAvailable() const { return m_available; }
    bool isListening() const { return m_listening.load(); }
    bool isPreprocEnabled() const { return m_preprocCtx != nullptr; }

public slots:
    void startListening();
    void stopListening();
    void finalizeResult();

signals:
    void signalVoiceRecognized(const QString &text);
    void signalVoicePartial(const QString &text);
    void signalAssistError(const QString &error);
    void signalVadSpeech(bool isSpeech);       /**< VAD detected speech/silence change */

private slots:
    void onAlsaReadTimeout();    // Periodically read audio data from ALSA

private:
    VoskModel      *m_model;
    VoskRecognizer *m_recognizer;

    /* ---- Audio capture ---- */
    alsa_capture_t  *m_alsaCtx;          /**< Native ALSA capture context */
    QTimer          *m_alsaReadTimer;    /**< ALSA periodic-read timer */

    /* ---- Audio preprocessing ---- */
    audio_preproc_t *m_preprocCtx;       /**< Audio preprocessing context (NS+AGC+VAD) */
    bool             m_lastVadSpeech;    /**< VAD state of the previous frame, for detecting speech/silence changes */

    QAtomicInt      m_listening;
    bool            m_available;
    QString         m_partialText;
    QString         m_accumulatedText;

    bool initAlsaCapture();              /**< Initialize native ALSA capture */
    void startAlsaListening();           /**< Start the ALSA capture loop */
    void stopAlsaListening();            /**< Stop ALSA capture */

    void processAudioData(const char *data, int size);  /**< Common audio data handler */
    bool initAudioPreproc();             /**< Initialize the audio preprocessing pipeline */
    void checkVoskResult();              /**< Check the Vosk full recognition result */

    void destroyRecognizer();
};

#endif
