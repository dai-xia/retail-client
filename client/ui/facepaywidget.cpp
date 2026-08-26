#include "facepaywidget.h"
#include "ui_facepaywidget.h"
#include "services/clientservice.h"
#include "core/hardwareservice.h"
#include <QMessageBox>
#include <QTimer>
#include <QDebug>

FacePayWidget::FacePayWidget(member_info_t member, QList<order_item_t> cart, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::FacePayWidget)
    , m_member(member)
    , m_cart(cart)
    , m_faceVerified(false)
    , m_isProcessing(false)  // Lock flag to prevent re-entrancy during face processing
    , m_wasMonitoring(false)
{
    ui->setupUi(this);
    this->setWindowTitle("刷脸支付");
    this->setModal(true);

    m_totalPrice = 0;
    for(const auto& item : m_cart)
    {
        m_totalPrice += item.subtotal;
    }

    QString orderInfo = QString("会员：%1\n支付金额：%2元")
                        .arg(m_member.name)
                        .arg(m_totalPrice, 0, 'f', 2);
    ui->label_order_info->setText(orderInfo);
    ui->label_status->setText("请将人脸对准摄像头...");
    ui->label_status->setStyleSheet("color: blue; font-size: 14px;");

    m_faceManager = FaceManager::getInstance();

    connect(ClientService::getInstance(), &ClientService::signalFaceVerifyResult,
            this, &FacePayWidget::slotFaceVerifyResult);

    /* If monitor streaming is running, pause it so this dialog owns the camera.
     * Monitor (main path) and face recognition (aux path) share the camera
     * exclusively via pauseMonitor/resumeMonitor to avoid reopening the device. */
    m_wasMonitoring = m_faceManager->isMonitoring();
    if (m_wasMonitoring) {
        m_faceManager->pauseMonitor();
        qDebug() << "FacePay: monitor stream paused, reusing camera";
    }

    m_cameraTimer = new QTimer(this);
    connect(m_cameraTimer, &QTimer::timeout, this, &FacePayWidget::slotUpdateCameraFrame);

    // initCamera returns true and reuses the device if monitor already opened it
    if(m_faceManager->initCamera(0))
    {
        m_cameraTimer->start(33); // ~30 fps
        HardwareService::getInstance()->setFaceLight(true);
        qDebug() << "FacePay: camera started, fill light on";
    }
    else
    {
        ui->label_status->setText("摄像头打开失败，请手动确认支付");
        ui->label_status->setStyleSheet("color: red; font-size: 14px;");
        ui->btn_confirm->setEnabled(true);
        qDebug() << "FacePay: camera start failed";
    }
}

FacePayWidget::~FacePayWidget()
{
    HardwareService::getInstance()->setFaceLight(false);

    if(m_cameraTimer != nullptr)
    {
        m_cameraTimer->stop();
    }

    if (m_wasMonitoring) {
        // Monitoring: do not release the camera, just resume the stream thread
        m_faceManager->resumeMonitor();
        qDebug() << "FacePay: monitor stream resumed";
    } else {
        // Not monitoring: release the camera
        m_faceManager->releaseCamera();
    }
    delete ui;
}

void FacePayWidget::slotUpdateCameraFrame()
{
    cv::Mat frame = m_faceManager->getCameraFrame();
    if(frame.empty()) return;

    std::vector<cv::Rect> faces = m_faceManager->detectFace(frame);

    bool faceDetected = !faces.empty();

    if(faceDetected && !m_isProcessing && !m_faceVerified)
    {
        for(size_t i = 0; i < faces.size(); i++)
        {
            cv::rectangle(frame, faces[i], cv::Scalar(0, 255, 0), 2);

            cv::putText(frame, "Face Detected",
                       cv::Point(faces[i].x, faces[i].y - 10),
                       cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        }

        processFaceVerification(frame);
    }
    else if(!faceDetected)
    {
        ui->label_status->setText("请将人脸对准摄像头...");
        ui->label_status->setStyleSheet("color: blue; font-size: 14px;");
    }

    // Convert OpenCV Mat -> QImage, scale and display in the camera area
    QImage img = m_faceManager->matToQImage(frame);
    ui->label_camera->setPixmap(QPixmap::fromImage(img)
                                .scaled(ui->label_camera->size(), Qt::KeepAspectRatio));
}

void FacePayWidget::processFaceVerification(const cv::Mat& frame)
{
    if(m_isProcessing || m_faceVerified) return;

    m_isProcessing = true;  // Lock to prevent duplicate requests

    qDebug() << "Face verification flow started...";

    // Extract 128-dim face feature and convert to string
    QString faceFeature = m_faceManager->extractFaceFeature(frame);

    if(faceFeature.isEmpty())
    {
        ui->label_status->setText("特征提取失败，请重试");
        ui->label_status->setStyleSheet("color: red; font-size: 14px;");
        m_isProcessing = false;
        return;
    }

    qDebug() << "Feature extracted, sending to server for verification...";

    ClientService::getInstance()->requestFaceVerify(m_member.uid, faceFeature);

    ui->label_status->setText("正在验证身份...");
    ui->label_status->setStyleSheet("color: orange; font-size: 14px;");
}

void FacePayWidget::on_btn_confirm_clicked()
{
    if(!m_faceVerified)
    {
        QMessageBox::warning(this, "警告", "请先完成人脸验证！");
        return;
    }
    this->accept();
}

void FacePayWidget::on_btn_cancel_clicked()
{
    this->reject();
}

void FacePayWidget::slotFaceVerifyResult(int code, QString msg)
{
    m_isProcessing = false;

    if (code == 0)
    {
        m_faceVerified = true;
        ui->label_status->setText("✓ 人脸验证成功！请确认支付");
        ui->label_status->setStyleSheet("color: green; font-size: 16px; font-weight: bold;");
        ui->btn_confirm->setEnabled(true);

        qDebug() << "Face verification succeeded!";

        ui->btn_confirm->setFocus();
    }
    else
    {
        m_faceVerified = false;
        ui->label_status->setText("✗ 验证失败：" + msg + "\n请重新对准摄像头");
        ui->label_status->setStyleSheet("color: red; font-size: 14px;");
        ui->btn_confirm->setEnabled(false);

        qDebug() << "Face verification failed:" << msg;
    }
}
