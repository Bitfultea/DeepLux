#include "ExpressionEngine.h"
#include "common/Logger.h"
#include <QRegularExpression>

namespace DeepLux {

ExpressionEngine* ExpressionEngine::s_instance = nullptr;

ExpressionEngine& ExpressionEngine::instance()
{
    if (!s_instance) {
        s_instance = new ExpressionEngine();
    }
    return *s_instance;
}

ExpressionEngine::ExpressionEngine(QObject* parent)
    : QObject(parent)
{
}

bool ExpressionEngine::compile(const QString& expression, const QString& context)
{
    Q_UNUSED(context)

    m_expression = expression;
    m_errorMessage.clear();

    if (expression.isEmpty()) {
        m_errorMessage = "Empty expression";
        emit compilationFailed(m_errorMessage);
        return false;
    }

    // Simplified validation - just check if it's not obviously broken
    if (expression.contains("{{") || expression.contains("}}")) {
        m_errorMessage = "Invalid expression syntax";
        emit compilationFailed(m_errorMessage);
        return false;
    }

    m_compiled = true;
    emit compilationSuccess();
    return true;
}

bool ExpressionEngine::evaluate()
{
    if (!m_compiled) {
        m_errorMessage = "Expression not compiled";
        emit evaluationFailed(m_errorMessage);
        return false;
    }

    // Simple expression evaluation
    QString expr = m_expression.trimmed();

    // Boolean literals
    if (expr == "true" || expr == "True" || expr == "TRUE") {
        m_result = true;
        emit evaluationSuccess(m_result);
        return true;
    }
    if (expr == "false" || expr == "False" || expr == "FALSE") {
        m_result = false;
        emit evaluationSuccess(m_result);
        return true;
    }

    // Numeric values
    bool ok;
    double num = expr.toDouble(&ok);
    if (ok) {
        m_result = num;
        emit evaluationSuccess(m_result);
        return true;
    }

    // String literal
    if ((expr.startsWith("\"") && expr.endsWith("\"")) ||
        (expr.startsWith("'") && expr.endsWith("'"))) {
        m_result = expr.mid(1, expr.length() - 2);
        emit evaluationSuccess(m_result);
        return true;
    }

    m_result = expr;
    emit evaluationSuccess(m_result);
    return true;
}

QVariant ExpressionEngine::result() const
{
    return m_result;
}

QVariant ExpressionEngine::evaluateExpression(const QString& expression,
                                             const QMap<QString, QVariant>& variables)
{
    Q_UNUSED(variables)

    QString expr = expression.trimmed();

    // Boolean literals
    if (expr == "true" || expr == "True" || expr == "TRUE") return true;
    if (expr == "false" || expr == "False" || expr == "FALSE") return false;

    // Numeric values
    bool ok;
    double num = expr.toDouble(&ok);
    if (ok) return num;

    // Check for variable references
    if (variables.contains(expr)) {
        return variables.value(expr);
    }

    // Return as string if no match
    return expr;
}

bool ExpressionEngine::validateExpression(const QString& expression, QString& error)
{
    if (expression.isEmpty()) {
        error = "Empty expression";
        return false;
    }

    // Basic syntax check
    int openBraces = expression.count("{{");
    int closeBraces = expression.count("}}");
    if (openBraces != closeBraces) {
        error = "Mismatched braces";
        return false;
    }

    return true;
}

} // namespace DeepLux