#ifndef VOICE_ASSIST_H
#define VOICE_ASSIST_H

#include <QObject>
#include <QString>
#include <QThread>
#include <QAtomicInt>
#include <QTimer>

struct VoskModel;
struct VoskRecognizer;

typedef struct alsa_capture_ctx alsa_capture_t;

typedef struct audio_preproc_ctx audio_preproc_t;

/**
 * @brief Offline voice assistant based on Vosk
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
    void onAlsaReadTimeout();

private:
    VoskModel      *m_model;
    VoskRecognizer *m_recognizer;

    alsa_capture_t  *m_alsaCtx;
    QTimer          *m_alsaReadTimer;

    audio_preproc_t *m_preprocCtx;       /**< NS+AGC+VAD */
    bool             m_lastVadSpeech;    /**< VAD state of the previous frame, for detecting speech/silence changes */

    QAtomicInt      m_listening;
    bool            m_available;
    QString         m_partialText;
    QString         m_accumulatedText;

    bool initAlsaCapture();
    void startAlsaListening();
    void stopAlsaListening();

    void processAudioData(const char *data, int size);
    bool initAudioPreproc();
    void checkVoskResult();

    void destroyRecognizer();
};

#endif
