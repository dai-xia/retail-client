#include "memberregister.h"
#include "ui_memberregister.h"
#include "services/clientservice.h"
#include "core/hardwareservice.h"
#include <QMessageBox>
#include <QDir>
#include <QDateTime>
#include <cJSON.h>
#include <opencv2/opencv.hpp>
#include <QDebug>

MemberRegister::MemberRegister(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::MemberRegister)
    , m_faceManager(FaceManager::getInstance())
    , m_registerInfo(nullptr)
{
    ui->setupUi(this);
    ui->doubleSpinBox_balance->setRange(0, 99999);
    ui->doubleSpinBox_balance->setValue(0.0);
}

MemberRegister::~MemberRegister()
{
    delete ui;
    if(m_registerInfo)
    {
        delete m_registerInfo;
        m_registerInfo = nullptr;
    }
}

void MemberRegister::on_checkBox_enable_face_toggled(bool checked)
{
    ui->btn_capture_face->setEnabled(checked);
    if(!checked)
    {
        ui->label_face->setText("人脸预览");
        m_currentFace = QImage();
    }
}

bool MemberRegister::validateInput()
{
    QString uid = ui->lineEdit_uid->text().trimmed();
    QString name = ui->lineEdit_name->text().trimmed();
    QString phone = ui->lineEdit_phone->text().trimmed();

    if(uid.isEmpty())
    {
        QMessageBox::warning(this, "警告", "IC卡UID不能为空！");
        return false;
    }
    if(name.isEmpty() || phone.isEmpty())
    {
        QMessageBox::warning(this, "警告", "姓名和手机号不能为空！");
        return false;
    }

    bool hasFace = ui->checkBox_enable_face->isChecked() && !m_currentFace.isNull();
    bool hasPassword = !ui->lineEdit_password->text().isEmpty();

    if(!hasFace && !hasPassword)
    {
        QMessageBox::warning(this, "警告", "请至少设置一种认证凭据（人脸识别或登录密码）！");
        return false;
    }

    if(hasPassword)
    {
        QString password = ui->lineEdit_password->text();
        QString confirmPassword = ui->lineEdit_confirm_password->text();

        if(password.length() < 6 || password.length() > 20)
        {
            QMessageBox::warning(this, "警告", "密码长度必须在6-20位之间！");
            return false;
        }
        if(password != confirmPassword)
        {
            QMessageBox::warning(this, "警告", "两次输入的密码不一致！");
            return false;
        }
    }

    return true;
}

void MemberRegister::slotGetCardUID(QString uid)
{
    ui->lineEdit_uid->setText(uid);
}

void MemberRegister::onMemberQueryResult(int code, QString msg, member_info_t member)
{
    Q_UNUSED(msg); Q_UNUSED(member);
    if (code == -1 && m_registerInfo) {
        processRegisterRequest();
    } else if (code == 0 && m_registerInfo) {
        QMessageBox::warning(this, "警告", "该UID已存在！");
        delete m_registerInfo;
        m_registerInfo = nullptr;
    }
}

void MemberRegister::onMemberRegisterResult(int code, QString msg)
{
    if (code == 0) {
        QMessageBox::information(this, "成功", "会员注册成功！");
        on_btn_clear_clicked();
    } else {
        QMessageBox::warning(this, "失败", "会员注册失败：" + msg);
    }
}

void MemberRegister::on_btn_capture_face_clicked()
{
    /* If monitor streaming is running, pause it to avoid contending with
     * captureFace for the camera (av_read_frame is not thread-safe; the same
     * AVFormatContext cannot be used concurrently). */
    bool wasMonitoring = m_faceManager->isMonitoring();
    if (wasMonitoring) {
        m_faceManager->pauseMonitor();
    }

    HardwareService::getInstance()->setFaceLight(true);
    /* captureFace internally: initCamera(if not opened) -> getCameraFrame -> detectFace */
    if (!m_faceManager->isCameraOpened()) {
        m_faceManager->initCamera(0);
    }
    m_currentFace = m_faceManager->captureFace();
    HardwareService::getInstance()->setFaceLight(false);

    if (wasMonitoring) {
        m_faceManager->resumeMonitor();
    }

    if(!m_currentFace.isNull())
    {
        ui->label_face->setPixmap(QPixmap::fromImage(m_currentFace)
                                  .scaled(ui->label_face->size(), Qt::KeepAspectRatio));
        QMessageBox::information(this, "提示", "人脸采集成功！");
    }
    else
    {
        QMessageBox::warning(this, "警告", "人脸采集失败，请检查摄像头！");
    }
}

void MemberRegister::on_btn_register_clicked()
{
    if(!validateInput())
    {
        return;
    }

    QString uid = ui->lineEdit_uid->text().trimmed();
    QString name = ui->lineEdit_name->text().trimmed();
    QString phone = ui->lineEdit_phone->text().trimmed();

    bool hasFace = ui->checkBox_enable_face->isChecked() && !m_currentFace.isNull();
    QString password = ui->lineEdit_password->text();

    RegisterInfo* registerInfo = new RegisterInfo();
    registerInfo->uid = uid;
    registerInfo->name = name;
    registerInfo->phone = phone;
    registerInfo->balance = 0.0;
    registerInfo->password = password;
    registerInfo->hasFace = hasFace;

    ClientService::getInstance()->requestMemberQuery(uid);

    m_registerInfo = registerInfo;
}

void MemberRegister::processRegisterRequest()
{
    if(!m_registerInfo) return;

    QString uid = m_registerInfo->uid;
    QString name = m_registerInfo->name;
    QString phone = m_registerInfo->phone;
    double balance = m_registerInfo->balance;
    QString password = m_registerInfo->password;
    bool hasFace = m_registerInfo->hasFace;

    QString facePath;
    QString faceFeature;
    if(hasFace)
    {
        facePath = QString("./faces/%1.jpg").arg(uid);
        if(!saveFaceImage(facePath))
        {
            QMessageBox::warning(this, "警告", "人脸图片保存失败！");
            delete m_registerInfo;
            m_registerInfo = nullptr;
            return;
        }

        QImage faceRgb = m_currentFace.convertToFormat(QImage::Format_RGB888);

        // Uploaded to server for cross-client face matching
        faceFeature = m_faceManager->extractFaceFeature(faceRgb);
        if(faceFeature.isEmpty())
        {
            qWarning() << "Face feature extraction failed; registration will have no face feature";
        }

        m_faceManager->registerFace(uid, faceRgb);
    }

    ClientService::getInstance()->requestMemberRegister(uid, name, phone, password,
                                                         hasFace ? facePath : "",
                                                         faceFeature);

    delete m_registerInfo;
    m_registerInfo = nullptr;
}

bool MemberRegister::saveFaceImage(const QString& path)
{
    if(m_currentFace.isNull()) return false;

    QDir dir;
    dir.mkpath(QFileInfo(path).absolutePath());

    return m_currentFace.save(path);
}

void MemberRegister::on_btn_clear_clicked()
{
    ui->lineEdit_uid->clear();
    ui->lineEdit_name->clear();
    ui->lineEdit_phone->clear();
    ui->doubleSpinBox_balance->setValue(0.0);
    ui->lineEdit_password->clear();
    ui->lineEdit_confirm_password->clear();
    ui->checkBox_enable_face->setChecked(false);
    ui->label_face->setText("人脸预览");
    m_currentFace = QImage();

    if(m_registerInfo)
    {
        delete m_registerInfo;
        m_registerInfo = nullptr;
    }
}
