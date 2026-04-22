#pragma once

#include <QObject>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QMap>
#include "../platform/Platform.h"

namespace DeepLux {

class ExpressionEngine : public QObject
{
    Q_OBJECT

public:
    static ExpressionEngine& instance();

    bool compile(const QString& expression, const QString& context = QString());
    bool evaluate();
    QVariant result() const;
    QString errorMessage() const { return m_errorMessage; }

    static QVariant evaluateExpression(const QString& expression,
                                       const QMap<QString, QVariant>& variables = QMap<QString, QVariant>());

    static bool validateExpression(const QString& expression, QString& error);

signals:
    void compilationSuccess();
    void compilationFailed(const QString& error);
    void evaluationSuccess(const QVariant& result);
    void evaluationFailed(const QString& error);

private:
    explicit ExpressionEngine(QObject* parent = nullptr);
    DISABLE_COPY(ExpressionEngine)

    static ExpressionEngine* s_instance;

    QString m_expression;
    QString m_context;
    QString m_errorMessage;
    QVariant m_result;
    bool m_compiled = false;
};

} // namespace DeepLux