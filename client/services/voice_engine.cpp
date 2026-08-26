#include "voice_engine.h"
#include "voice_assist.h"
#include <QDebug>
#include <QDir>
#include <QRegularExpression>

VoiceEngine::VoiceEngine(QObject *parent)
    : QObject(parent)
    , m_initialized(false)
    , m_assist(nullptr)
{
}

VoiceEngine::~VoiceEngine()
{
    if (m_assist) {
        m_assist->stopListening();
        delete m_assist;
        m_assist = nullptr;
    }
}

bool VoiceEngine::init(const QString &modelPath)
{
    buildIntentTable();
    buildGoodsAliases();
    buildNumberMap();

    m_assist = new VoiceAssist(this);
    QString path = modelPath;
    if (path.isEmpty()) {
        // Search the Vosk model directory by priority.
        QStringList searchPaths = {
            "./voice_model/vosk-model-small-cn-0.22",
            "../voice_model/vosk-model-small-cn-0.22",
            "../../client/voice_model/vosk-model-small-cn-0.22",
            "/opt/retail/voice_model/vosk-model-small-cn-0.22",
            QString(qgetenv("VOSK_MODEL_PATH")),
        };
        for (const auto &p : searchPaths) {
            if (QDir(p).exists()) {
                path = p;
                break;
            }
        }
    }
    if (path.isEmpty()) {
        qWarning() << "[VoiceEngine] Vosk model not found, set VOSK_MODEL_PATH or place it under ./voice_model/";
    }
    m_assist->init(path);

    connect(m_assist, &VoiceAssist::signalVoiceRecognized, this, &VoiceEngine::signalVoiceRecognized);
    connect(m_assist, &VoiceAssist::signalVoicePartial, this, &VoiceEngine::signalVoicePartial);

    m_initialized = true;
    qDebug() << "[VoiceEngine] initialized, intents =" << m_intentTable.size()
             << "aliases =" << m_goodsAliases.size()
             << "numbers =" << m_numberMap.size()
             << "asr =" << (m_assist->isAvailable() ? "available" : "unavailable");
    return true;
}

bool VoiceEngine::isVoiceAvailable() const
{
    return m_assist && m_assist->isAvailable();
}

void VoiceEngine::startVoiceListening()
{
    if (m_assist && m_assist->isAvailable()) {
        m_assist->startListening();
    }
}

void VoiceEngine::stopVoiceListening()
{
    if (m_assist) {
        m_assist->stopListening();
        m_assist->finalizeResult();
    }
}

// Intent rule table. Each entry: {patterns, target, description}.
// patterns are Chinese keywords matched against the recognized text.
void VoiceEngine::buildIntentTable()
{
    m_intentTable = {
        {{"结账", "买单", "付款", "结算", "支付", "结帐"},
         "checkout", "结算支付"},

        {{"取消", "不要了", "清空", "算了", "不用了", "退掉", "删掉"},
         "cancel", "清空购物车"},

        {{"充值", "充钱", "我要充值", "存钱"},
         "recharge", "余额充值"},

        {{"余额", "多少钱", "还剩", "查余额", "我的钱", "账户"},
         "balance", "查询余额"},

        {{"推荐", "热销", "热门", "大家都在买", "什么好", "有啥"},
         "recommend", "热销商品推荐"},

        {{"注册", "开卡", "办卡", "我要注册", "会员注册"},
         "register", "注册会员"},

        {{"帮助", "能做什么", "怎么用", "功能", "help"},
         "help", "功能帮助"},

        {{"买", "加", "来", "要", "选购", "加入", "拿", "来一个", "来一瓶", "来一包"},
         "buy", "选购商品"},

        {{"你好", "嗨", "hello", "hi", "在吗"},
         "greet", "打招呼"},

        {{"谢谢", "多谢", "thank", "thanks"},
         "thanks", "感谢"},

        {{"结束", "关闭", "退出", "拜拜", "再见", "bye", "好了", "就这样"},
         "end_session", "结束对话"},
    };
}

// Goods alias table. Each entry: {aliases, standard name, description}.
// Aliases are matched against the recognized text to resolve a product.
void VoiceEngine::buildGoodsAliases()
{
    m_goodsAliases = {
        {{"可乐", "coke", "cola", "可口"},       "可口可乐",     ""},
        {{"矿泉水", "水", "农夫", "山泉"},        "农夫山泉",     ""},
        {{"方便面", "泡面", "面", "康师傅"},      "康师傅方便面", ""},
        {{"牙膏", "中华"},                        "中华牙膏",     ""},
        {{"洗发水", "海飞丝", "洗头"},            "海飞丝洗发水", ""},
        {{"雪饼", "旺旺", "旺旺雪饼"},            "旺旺雪饼",     ""},
        {{"巧克力", "德芙", "chocolate"},         "德芙巧克力",   ""},
        {{"啤酒", "beer", "青岛"},                "青岛啤酒",     ""},
    };
}

// Number mapping: {number words, numeric string, tag}.


void VoiceEngine::buildNumberMap()
{
    m_numberMap = {
        {{"一"},   "1",  ""},
        {{"二", "两"}, "2",  ""},
        {{"三"},   "3",  ""},
        {{"四"},   "4",  ""},
        {{"五"},   "5",  ""},
        {{"六"},   "6",  ""},
        {{"七"},   "7",  ""},
        {{"八"},   "8",  ""},
        {{"九"},   "9",  ""},
        {{"十"},   "10", ""},
        {{"二十"}, "20", ""},
        {{"三十"}, "30", ""},
        {{"五十"}, "50", ""},
        {{"一百"}, "100", ""},
        {{"两百"}, "200", ""},
        {{"五百"}, "500", ""},
        {{"一千"}, "1000", ""},

        {{"瓶"}, "1", "quantity_unit"},
        {{"包"}, "1", "quantity_unit"},
        {{"个"}, "1", "quantity_unit"},
        {{"罐"}, "1", "quantity_unit"},
        {{"袋"}, "1", "quantity_unit"},
        {{"盒"}, "1", "quantity_unit"},
        {{"支"}, "1", "quantity_unit"},
        {{"件"}, "1", "quantity_unit"},
        {{"份"}, "1", "quantity_unit"},
        {{"杯"}, "1", "quantity_unit"},

        {{"块", "元"}, "1", "amount_unit"},
    };
}

QString VoiceEngine::resolveGoodsName(const QString &input) const
{
    QString lower = input.toLower().simplified();
    for (const auto &rule : m_goodsAliases) {
        for (const QString &alias : rule.patterns) {
            if (lower.contains(alias.toLower())) {
                return rule.target;
            }
        }
    }
    return QString();
}



int VoiceEngine::matchNumber(const QVector<PatternRule> &map, const QString &text) const
{
    static const QRegularExpression arabicUnit("(\\d+)\\s*(瓶|包|个|罐|袋|盒|支|件|份|杯|块|元)");
    QRegularExpressionMatch m = arabicUnit.match(text);
    if (m.hasMatch()) {
        bool ok;
        int v = m.captured(1).toInt(&ok);
        if (ok && v > 0) return v;
    }

    for (const auto &rule : map) {
        if (rule.description == "quantity_unit" || rule.description == "amount_unit") continue;
        for (const QString &p : rule.patterns) {
            if (text.contains(p)) return rule.target.toInt();
        }
    }
    return 0;
}

int VoiceEngine::extractQuantity(const QString &text) const
{
    int v = matchNumber(m_numberMap, text);
    return v > 0 ? v : 1;
}

int VoiceEngine::extractAmount(const QString &text) const
{
    static const QRegularExpression re("(\\d+)\\s*(块|元)");
    QRegularExpressionMatch m = re.match(text);
    if (m.hasMatch()) {
        bool ok;
        int v = m.captured(1).toInt(&ok);
        if (ok && v > 0 && v <= 9999) return v;
    }
    return matchNumber(m_numberMap, text);
}

VoiceResult VoiceEngine::processCommand(const QString &text)
{
    VoiceResult result;
    result.rawText = text;
    result.matched = false;

    if (text.isEmpty()) return result;

    QString lower = text.toLower().simplified();
    for (const auto &intent : m_intentTable)
    {
        for (const QString &kw : intent.patterns)
        {
            if (lower.contains(kw.toLower()))
            {
                result.category = intent.target;
                result.matched = true;
                result.args = text.split(QRegularExpression("\\s+"), QString::SkipEmptyParts);
                break;
            }
        }
        if (result.matched) break;
    }

    if (!result.matched) {
        result.category = "unknown";
        return result;
    }

    if (result.category == "buy")
    {
        QString goods = resolveGoodsName(text);
        int qty = extractQuantity(text);

        if (!goods.isEmpty()) {
            emit signalPlayTTS(QString("已添加 %1 %2件").arg(goods).arg(qty));
            emit signalAddToCart(goods, qty);
        } else {
            emit signalPlayTTS("请说商品名称，比如 买一瓶可乐");
            result.category = "buy_failed";
        }
    }
    else
    {
        emit signalCommandResult(result);

        if (result.category == "recommend")       emit signalShowRecommend();
        else if (result.category == "balance")    emit signalCheckBalance();
        else if (result.category == "checkout")   emit signalOpenPayment();
        else if (result.category == "cancel")     emit signalClearCart();
        else if (result.category == "recharge") {
            emit signalOpenRecharge(extractAmount(text));
        }
        else if (result.category == "register")   emit signalOpenRegister();
        else if (result.category == "help")       emit signalHelpRequested();
        else if (result.category == "greet")      emit signalPlayTTS("你好！我是小售，可以帮你选购商品、查余额、买单。试试说 买一瓶可乐");
        else if (result.category == "thanks")     emit signalPlayTTS("不客气！");
        else if (result.category == "end_session") emit signalEndSession();
    }

    return result;
}

QStringList VoiceEngine::getHelpText() const
{
    QStringList help;
    for (const auto &i : m_intentTable) {
        if (i.target == "greet" || i.target == "thanks") continue;
        help << QString("「%1」→ %2").arg(i.patterns.first()).arg(i.description);
    }
    return help;
}

QStringList VoiceEngine::getRecommendList() const
{
    return {
        "今日推荐：",
        "  ★ 德芙巧克力 — 15.00 元/盒",
        "  ★ 青岛啤酒   —  6.00 元/罐",
        "  ★ 旺旺雪饼   —  8.00 元/袋",
        "  ★ 可口可乐   —  3.50 元/瓶",
    };
}

void VoiceEngine::simulateVoiceInput(const QString &text)
{
    emit signalVoiceRecognized(text);
}
