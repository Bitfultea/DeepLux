#include "VarModel.h"
#include "common/Logger.h"

namespace DeepLux {

VarModel::VarModel(QObject* parent)
    : QObject(parent)
{
}

VarModel::VarModel(const QString& name, VarDataType type, QVariant value, QObject* parent)
    : QObject(parent)
    , m_name(name)
    , m_dataType(type)
    , m_value(value)
{
}

void VarModel::setValue(const QVariant& val)
{
    if (m_value != val) {
        m_value = convertToExpectedType(val, m_dataType);
        emit valueChanged();
    }
}

bool VarModel::compileExpression(const QString& projectId, const QString& moduleName)
{
    Q_UNUSED(projectId)
    Q_UNUSED(moduleName)

    if (m_expression.isEmpty() || m_expression == "NULL") {
        m_compileSuccess = true;
        return true;
    }

    // Simplified expression compilation - just mark as success for now
    // Full expression evaluation would require QJSEngine
    m_compileSuccess = true;
    Logger::instance().debug(QString("Expression prepared: %1 = %2").arg(m_name).arg(m_expression), "Var");

    return true;
}

QVariant VarModel::evaluateExpression()
{
    if (m_expression.isEmpty() || m_expression == "NULL") {
        return m_value;
    }

    // Simplified evaluation - in production would use QJSEngine
    bool ok;
    double num = m_expression.toDouble(&ok);
    if (ok) {
        return convertToExpectedType(num, m_dataType);
    }

    if (m_expression == "true" || m_expression == "True" || m_expression == "TRUE") {
        return true;
    }
    if (m_expression == "false" || m_expression == "False" || m_expression == "FALSE") {
        return false;
    }

    return m_value;
}

QString VarModel::dataTypeToString(VarDataType type)
{
    switch (type) {
        case VarDataType::Int: return "int";
        case VarDataType::Double: return "double";
        case VarDataType::String: return "string";
        case VarDataType::Bool: return "bool";
        case VarDataType::IntArray: return "int[]";
        case VarDataType::DoubleArray: return "double[]";
        case VarDataType::StringArray: return "string[]";
        case VarDataType::BoolArray: return "bool[]";
        case VarDataType::Image: return "Image";
        case VarDataType::Region: return "Region";
        case VarDataType::Point: return "Point";
        case VarDataType::Line: return "Line";
        case VarDataType::Circle: return "Circle";
        case VarDataType::Rect: return "Rect";
        default: return "None";
    }
}

VarDataType VarModel::stringToDataType(const QString& str)
{
    if (str == "int") return VarDataType::Int;
    if (str == "double") return VarDataType::Double;
    if (str == "string") return VarDataType::String;
    if (str == "bool") return VarDataType::Bool;
    if (str == "int[]") return VarDataType::IntArray;
    if (str == "double[]") return VarDataType::DoubleArray;
    if (str == "string[]") return VarDataType::StringArray;
    if (str == "bool[]") return VarDataType::BoolArray;
    if (str == "Image") return VarDataType::Image;
    if (str == "Region") return VarDataType::Region;
    if (str == "Point") return VarDataType::Point;
    if (str == "Line") return VarDataType::Line;
    if (str == "Circle") return VarDataType::Circle;
    if (str == "Rect") return VarDataType::Rect;
    return VarDataType::None;
}

QVariant VarModel::convertToExpectedType(const QVariant& value, VarDataType targetType)
{
    if (!value.isValid() || value.isNull()) {
        return QVariant();
    }

    switch (targetType) {
        case VarDataType::Double:
            if (value.type() == QVariant::Double || value.canConvert<double>()) {
                return value.toDouble();
            }
            break;
        case VarDataType::Int:
            if (value.type() == QVariant::Int || value.canConvert<int>()) {
                return value.toInt();
            }
            break;
        case VarDataType::String:
            return value.toString();
        case VarDataType::Bool:
            if (value.type() == QVariant::Bool) {
                return value.toBool();
            }
            if (value.canConvert<bool>()) {
                return value.toBool();
            }
            break;
        default:
            break;
    }

    return value;
}

} // namespace DeepLux