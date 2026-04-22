#pragma once

#include <QObject>
#include <QVariant>
#include <QString>
#include <QList>
#include <QMap>
#include <QMutex>

namespace DeepLux {

enum class VarDataType {
    None,
    Int,
    Double,
    String,
    Bool,
    IntArray,
    DoubleArray,
    StringArray,
    BoolArray,
    Image,
    Region,
    Point,
    Line,
    Circle,
    Rect
};

class VarModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(VarDataType dataType READ dataType WRITE setDataType NOTIFY dataTypeChanged)
    Q_PROPERTY(QVariant value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(QString expression READ expression WRITE setExpression NOTIFY expressionChanged)
    Q_PROPERTY(QString note READ note WRITE setNote NOTIFY noteChanged)
    Q_PROPERTY(bool isLinked READ isLinked WRITE setIsLinked NOTIFY isLinkedChanged)
    Q_PROPERTY(int index READ index WRITE setIndex NOTIFY indexChanged)

public:
    VarModel(QObject* parent = nullptr);
    VarModel(const QString& name, VarDataType type, QVariant value, QObject* parent = nullptr);

    QString name() const { return m_name; }
    void setName(const QString& name) { m_name = name; emit nameChanged(); }

    VarDataType dataType() const { return m_dataType; }
    void setDataType(VarDataType type) { m_dataType = type; emit dataTypeChanged(); }

    QVariant value() const { return m_value; }
    void setValue(const QVariant& val);

    QString expression() const { return m_expression; }
    void setExpression(const QString& expr) { m_expression = expr; emit expressionChanged(); }

    QString note() const { return m_note; }
    void setNote(const QString& note) { m_note = note; emit noteChanged(); }

    bool isLinked() const { return m_isLinked; }
    void setIsLinked(bool linked) { m_isLinked = linked; emit isLinkedChanged(); }

    int index() const { return m_index; }
    void setIndex(int idx) { m_index = idx; emit indexChanged(); }

    bool compileExpression(const QString& projectId, const QString& moduleName);
    QVariant evaluateExpression();

    static QString dataTypeToString(VarDataType type);
    static VarDataType stringToDataType(const QString& str);
    static QVariant convertToExpectedType(const QVariant& value, VarDataType targetType);

signals:
    void nameChanged();
    void dataTypeChanged();
    void valueChanged();
    void expressionChanged();
    void noteChanged();
    void isLinkedChanged();
    void indexChanged();

private:
    QString m_name;
    VarDataType m_dataType = VarDataType::None;
    QVariant m_value;
    QString m_expression;
    QString m_note;
    bool m_isLinked = false;
    int m_index = 0;

    bool m_compileSuccess = false;
};

} // namespace DeepLux