#ifndef PAYMENT_PROVIDER_H
#define PAYMENT_PROVIDER_H

#include <QString>
#include <functional>

class PaymentProvider
{
public:
    virtual ~PaymentProvider() = default;

    virtual void pay(double amount, const QString& orderId,
                     std::function<void(bool ok, const QString& msg)> callback) = 0;

    virtual QString name() const = 0;
};

#endif // PAYMENT_PROVIDER_H
