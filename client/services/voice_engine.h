#ifndef VOICE_ENGINE_H
#define VOICE_ENGINE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class VoiceAssist;

/**
 * @brief Unified rule entry: keyword patterns -> target category / standard name
 *
 * All "text -> result" mappings are expressed with this struct; the storage container is uniformly QVector<PatternRule>.
 * It replaces the previously mixed QMap, QStringList, and regex forms.
 */
struct PatternRule {
    QStringList patterns;       // Trigger keyword list (any match counts as a hit)
    QString     target;         // Target value: intent category / standard product name / numeric mapping target
    QString     description;    // Feature description (only used for help prompts)
};

/**
 * @brief Voice parsing result struct
 */
struct VoiceResult {
    QString     category;   // Matched command category
    QString     rawText;    // Raw voice recognition text
    QStringList args;       // Parsed command arguments
    bool        matched;    // Whether a valid command was successfully matched
};

/**
 * @brief Voice engine core class
 */
class VoiceEngine : public QObject
{
    Q_OBJECT

public:
    explicit VoiceEngine(QObject *parent = nullptr);
    ~VoiceEngine();

    bool init(const QString &modelPath = QString());
    bool isInitialized() const { return m_initialized; }
    bool isVoiceAvailable() const;

    void startVoiceListening();
    void stopVoiceListening();

    VoiceResult processCommand(const QString &text);
    void simulateVoiceInput(const QString &text);

    QStringList getHelpText() const;
    QStringList getRecommendList() const;

signals:
    void signalVoiceRecognized(const QString &text);
    void signalVoicePartial(const QString &text);
    void signalCommandResult(const VoiceResult &result);

    // Business command dispatch signals
    void signalAddToCart(const QString &goodsName, int quantity);
    void signalCheckBalance();
    void signalOpenPayment();
    void signalClearCart();
    void signalOpenRecharge(int amount);
    void signalShowRecommend();
    void signalPlayTTS(const QString &text);
    void signalOpenRegister();
    void signalHelpRequested();
    void signalEndSession();

private:
    bool m_initialized;
    QVector<PatternRule> m_intentTable;    // Intent rule table
    QVector<PatternRule> m_goodsAliases;   // Goods alias table (patterns=aliases, target=standard name)
    QVector<PatternRule> m_numberMap;      // Number mapping table (patterns=number/unit text, target=numeric string)
    VoiceAssist *m_assist;

    void buildIntentTable();
    void buildGoodsAliases();
    void buildNumberMap();

    QString resolveGoodsName(const QString &input) const;
    int     matchNumber(const QVector<PatternRule> &map, const QString &text) const;
    int     extractQuantity(const QString &text) const;
    int     extractAmount(const QString &text) const;
};

#endif
