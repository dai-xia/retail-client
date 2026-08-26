#ifndef FACEPAYWIDGET_H
#define FACEPAYWIDGET_H

#include <QDialog>
#include "core/facemanager.h"
#include "common.h"
#include <cJSON.h>

QT_BEGIN_NAMESPACE
namespace Ui { class FacePayWidget; }
QT_END_NAMESPACE

class FacePayWidget : public QDialog
{
    Q_OBJECT

public:
    FacePayWidget(member_info_t member, QList<order_item_t> cart, QWidget *parent = nullptr);
    ~FacePayWidget();

private slots:
    void on_btn_cancel_clicked();
    void on_btn_confirm_clicked();
    void slotUpdateCameraFrame();
    void slotFaceVerifyResult(int code, QString msg);

private:
    Ui::FacePayWidget *ui;
    FaceManager* m_faceManager;
    member_info_t m_member;
    QList<order_item_t> m_cart;
    double m_totalPrice;
    bool m_faceVerified;
    bool m_isProcessing;
    bool m_wasMonitoring;  // Whether monitor streaming was active on open (restored in destructor)
    QTimer* m_cameraTimer;

    void processFaceVerification(const cv::Mat& frame);
};

#endif // FACEPAYWIDGET_H
