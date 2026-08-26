#ifndef MEMBERREGISTER_H
#define MEMBERREGISTER_H

#include <QWidget>
#include "core/facemanager.h"
#include "common.h"
#include <cJSON.h>

QT_BEGIN_NAMESPACE
namespace Ui { class MemberRegister; }
QT_END_NAMESPACE

class MemberRegister : public QWidget
{
    Q_OBJECT

public:
    explicit MemberRegister(QWidget *parent = nullptr);
    ~MemberRegister();

public slots:
    void slotGetCardUID(QString uid);
    void onMemberQueryResult(int code, QString msg, member_info_t member);
    void onMemberRegisterResult(int code, QString msg);

private slots:
    void on_checkBox_enable_face_toggled(bool checked);
    void on_btn_capture_face_clicked();
    void on_btn_register_clicked();
    void on_btn_clear_clicked();

private:
    Ui::MemberRegister *ui;
    FaceManager* m_faceManager;
    QImage m_currentFace;

    struct RegisterInfo {
        QString uid;
        QString name;
        QString phone;
        double balance;
        QString password;
        bool hasFace;
        QString facePath;
    } *m_registerInfo;

    bool saveFaceImage(const QString& path);
    bool validateInput();
    void processRegisterRequest();
};

#endif // MEMBERREGISTER_H
