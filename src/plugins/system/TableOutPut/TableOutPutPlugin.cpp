#include "TableOutPutPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QSpinBox>

namespace DeepLux {

TableOutPutPlugin::TableOutPutPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"rowCount", 5},
        {"colCount", 3}
    };
    m_params = m_defaultParams;
}

TableOutPutPlugin::~TableOutPutPlugin()
{
}

bool TableOutPutPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "TableOutPutPlugin initialized";
    return true;
}

void TableOutPutPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool TableOutPutPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_rowCount = m_params["rowCount"].toInt();
    m_colCount = m_params["colCount"].toInt();

    // 获取表格数据
    QVariant tableDataVar = input.data("table_data");
    QVariantList tableData;

    if (tableDataVar.isValid()) {
        tableData = tableDataVar.toList();
    }

    // 构建表格输出
    QString tableOutput;
    if (!tableData.isEmpty()) {
        int idx = 0;
        for (int r = 0; r < m_rowCount && idx < tableData.size(); ++r) {
            QStringList row;
            for (int c = 0; c < m_colCount && idx < tableData.size(); ++c) {
                row << tableData[idx++].toString();
            }
            tableOutput += row.join("\t") + "\n";
        }
    } else {
        // 生成空表格
        for (int r = 0; r < m_rowCount; ++r) {
            QStringList row;
            for (int c = 0; c < m_colCount; ++c) {
                row << "";
            }
            tableOutput += row.join("\t") + "\n";
        }
    }

    output.setData("table_output", tableOutput);
    output.setData("table_rows", m_rowCount);
    output.setData("table_cols", m_colCount);

    Logger::instance().debug(QString("表格输出: %1行 x %2列").arg(m_rowCount).arg(m_colCount), "TableOutPut");

    return true;
}

bool TableOutPutPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    int rows = params["rowCount"].toInt();
    int cols = params["colCount"].toInt();

    if (rows < 1 || rows > 100) {
        error = tr("行数必须在1到100之间");
        return false;
    }

    if (cols < 1 || cols > 20) {
        error = tr("列数必须在1到20之间");
        return false;
    }

    return true;
}

QWidget* TableOutPutPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("行数:")));
    QSpinBox* rowSpin = new QSpinBox();
    rowSpin->setRange(1, 100);
    rowSpin->setValue(m_params["rowCount"].toInt());
    layout->addWidget(rowSpin);

    layout->addWidget(new QLabel(tr("列数:")));
    QSpinBox* colSpin = new QSpinBox();
    colSpin->setRange(1, 20);
    colSpin->setValue(m_params["colCount"].toInt());
    layout->addWidget(colSpin);

    layout->addStretch();

    connect(rowSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["rowCount"] = value;
    });

    connect(colSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["colCount"] = value;
    });

    return widget;
}

IModule* TableOutPutPlugin::cloneImpl() const
{
    TableOutPutPlugin* clone = new TableOutPutPlugin();
    return clone;
}

} // namespace DeepLux
