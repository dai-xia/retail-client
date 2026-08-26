#ifndef MOCK_PAYMENT_PROVIDER_H
#define MOCK_PAYMENT_PROVIDER_H

#include "payment_provider.h"

class MockPaymentProvider : public PaymentProvider
{
public:
    void pay(double amount, const QString& orderId,
             std::function<void(bool ok, const QString& msg)> callback) override
    {
        Q_UNUSED(amount);
        Q_UNUSED(orderId);
        callback(true, "模拟支付成功");
    }

    QString name() const override { return "模拟支付"; }
};

#endif // MOCK_PAYMENT_PROVIDER_H
