#include "PluginTestDialog.h"
#include "../core/manager/PluginManager.h"
#include "../core/model/ImageData.h"
#include "../core/interface/IModule.h"
#include <QComboBox>
#include <QPushButton>
#include <QTextEdit>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QDateTime>
#include <QDialogButtonBox>

namespace DeepLux {

PluginTestDialog::PluginTestDialog(QWidget* parent)
    : QDialog(parent)
    , m_currentModule(nullptr)
{
    setWindowTitle(tr("插件测试"));
    setMinimumSize(800, 600);
    setupUi();
    populatePluginList();
}

PluginTestDialog::~PluginTestDialog()
{
}

void PluginTestDialog::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // ===== 插件选择 =====
    QGroupBox* selectGroup = new QGroupBox(tr("选择插件"));
    QHBoxLayout* selectLayout = new QHBoxLayout(selectGroup);

    m_pluginCombo = new QComboBox();
    m_refreshBtn = new QPushButton(tr("刷新"));

    selectLayout->addWidget(new QLabel(tr("插件:")));
    selectLayout->addWidget(m_pluginCombo, 1);
    selectLayout->addWidget(m_refreshBtn);

    mainLayout->addWidget(selectGroup);

    // ===== 参数设置 =====
    m_paramsGroup = new QGroupBox(tr("参数设置"));
    QVBoxLayout* paramsMainLayout = new QVBoxLayout(m_paramsGroup);

    m_paramsScroll = new QScrollArea();
    m_paramsScroll->setWidgetResizable(true);
    m_paramsWidget = new QWidget();
    m_paramsLayout = new QFormLayout(m_paramsWidget);
    m_paramsLayout->setLabelAlignment(Qt::AlignLeft);

    m_paramsScroll->setWidget(m_paramsWidget);
    paramsMainLayout->addWidget(m_paramsScroll);

    mainLayout->addWidget(m_paramsGroup, 1);

    // ===== 输入图像 =====
    QGroupBox* inputGroup = new QGroupBox(tr("输入数据"));
    QVBoxLayout* inputLayout = new QVBoxLayout(inputGroup);

    QHBoxLayout* imageLayout = new QHBoxLayout();
    m_imagePathEdit = new QLineEdit();
    m_imagePathEdit->setPlaceholderText(tr("图像路径 (可选)"));
    m_loadImageBtn = new QPushButton(tr("加载图像"));

    imageLayout->addWidget(new QLabel(tr("图像:")));
    imageLayout->addWidget(m_imagePathEdit, 1);
    imageLayout->addWidget(m_loadImageBtn);

    m_imagePreview = new QLabel(tr("(无图像)"));
    m_imagePreview->setMinimumHeight(100);
    m_imagePreview->setAlignment(Qt::AlignCenter);
    m_imagePreview->setStyleSheet("border: 1px solid #ccc; background-color: #f0f0f0;");

    inputLayout->addLayout(imageLayout);
    inputLayout->addWidget(m_imagePreview);

    mainLayout->addWidget(inputGroup);

    // ===== 预期结果 =====
    m_expectedGroup = new QGroupBox(tr("预期结果 (可选)"));
    QVBoxLayout* expectedMainLayout = new QVBoxLayout(m_expectedGroup);

    QHBoxLayout* expectedBtnLayout = new QHBoxLayout();
    QPushButton* addExpectedBtn = new QPushButton(tr("添加预期"));
    QPushButton* removeExpectedBtn = new QPushButton(tr("移除"));
    expectedBtnLayout->addWidget(addExpectedBtn);
    expectedBtnLayout->addStretch();
    expectedBtnLayout->addWidget(removeExpectedBtn);

    QWidget* expectedContainer = new QWidget();
    QVBoxLayout* expectedListLayout = new QVBoxLayout(expectedContainer);
    expectedListLayout->setContentsMargins(0, 0, 0, 0);

    expectedMainLayout->addLayout(expectedBtnLayout);
    expectedMainLayout->addWidget(expectedContainer, 1);

    connect(addExpectedBtn, &QPushButton::clicked, this, &PluginTestDialog::onAddExpectedClicked);
    connect(removeExpectedBtn, &QPushButton::clicked, this, &PluginTestDialog::onRemoveExpectedClicked);

    mainLayout->addWidget(m_expectedGroup);

    // ===== 执行按钮 =====
    QHBoxLayout* btnLayout = new QHBoxLayout();
    m_executeBtn = new QPushButton(tr("执行测试"));
    m_executeBtn->setDefault(true);
    m_clearBtn = new QPushButton(tr("清除"));

    btnLayout->addStretch();
    btnLayout->addWidget(m_executeBtn);
    btnLayout->addWidget(m_clearBtn);

    mainLayout->addLayout(btnLayout);

    // ===== 结果显示 =====
    QGroupBox* resultGroup = new QGroupBox(tr("测试结果"));
    QVBoxLayout* resultLayout = new QVBoxLayout(resultGroup);

    m_resultEdit = new QTextEdit();
    m_resultEdit->setReadOnly(true);
    m_resultEdit->setMaximumHeight(150);
    m_resultEdit->setStyleSheet("font-family: monospace;");

    resultLayout->addWidget(m_resultEdit);

    mainLayout->addWidget(resultGroup);

    // ===== 信号连接 =====
    connect(m_pluginCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PluginTestDialog::onPluginSelected);
    connect(m_refreshBtn, &QPushButton::clicked, this, &PluginTestDialog::populatePluginList);
    connect(m_executeBtn, &QPushButton::clicked, this, &PluginTestDialog::onExecuteClicked);
    connect(m_clearBtn, &QPushButton::clicked, this, &PluginTestDialog::onClearClicked);
    connect(m_loadImageBtn, &QPushButton::clicked, this, &PluginTestDialog::onLoadImageClicked);
}

void PluginTestDialog::populatePluginList()
{
    QString current = m_pluginCombo->currentText();
    m_pluginCombo->clear();

    PluginManager& pm = PluginManager::instance();
    QStringList modules = pm.availableModules();

    for (const QString& name : modules) {
        m_pluginCombo->addItem(name);
    }

    if (!current.isEmpty()) {
        int idx = m_pluginCombo->findText(current);
        if (idx >= 0) {
            m_pluginCombo->setCurrentIndex(idx);
        }
    }
}

void PluginTestDialog::onPluginSelected(int index)
{
    // 清除旧参数
    QLayoutItem* item;
    while ((item = m_paramsLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
    m_paramWidgets.clear();

    if (m_currentModule) {
        m_currentModule->shutdown();
        m_currentModule = nullptr;
    }

    if (index < 0) {
        return;
    }

    QString pluginName = m_pluginCombo->itemText(index);
    m_currentPluginName = pluginName;

    PluginManager& pm = PluginManager::instance();
    m_currentModule = pm.createModule(pluginName);

    if (!m_currentModule) {
        m_resultEdit->setTextColor(Qt::red);
        m_resultEdit->append(QString("[%1] 无法创建插件实例\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss")));
        return;
    }

    if (!m_currentModule->initialize()) {
        m_resultEdit->setTextColor(Qt::red);
        m_resultEdit->append(QString("[%1] 插件初始化失败: %2\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss"), pluginName));
        delete m_currentModule;
        m_currentModule = nullptr;
        return;
    }

    m_resultEdit->setTextColor(Qt::blue);
    m_resultEdit->append(QString("[%1] 已加载: %2\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss"), pluginName));

    loadPluginParams();
}

void PluginTestDialog::loadPluginParams()
{
    if (!m_currentModule) return;

    QJsonObject params = m_currentModule->currentParams();

    for (auto it = params.begin(); it != params.end(); ++it) {
        QString key = it.key();
        QJsonValue value = it.value();

        QWidget* widget = createParamWidget(key, value);
        if (widget) {
            m_paramsLayout->addRow(key, widget);
            m_paramWidgets[key] = widget;
        }
    }
}

QWidget* PluginTestDialog::createParamWidget(const QString& key, const QJsonValue& value)
{
    if (value.isDouble()) {
        double val = value.toDouble();
        if (val != static_cast<int>(val)) {
            return createDoubleWidget(key, val, -999999, 999999);
        } else {
            return createIntWidget(key, static_cast<int>(val), -999999, 999999);
        }
    } else if (value.isBool()) {
        return createBoolWidget(key, value.toBool());
    } else if (value.isString()) {
        return createStringWidget(key, value.toString());
    }
    return nullptr;
}

QWidget* PluginTestDialog::createIntWidget(const QString& key, int value, int min, int max)
{
    QSpinBox* spin = new QSpinBox();
    spin->setRange(min, max);
    spin->setValue(value);
    spin->setProperty("paramKey", key);
    return spin;
}

QWidget* PluginTestDialog::createDoubleWidget(const QString& key, double value, double min, double max)
{
    QDoubleSpinBox* spin = new QDoubleSpinBox();
    spin->setRange(min, max);
    spin->setDecimals(6);
    spin->setValue(value);
    spin->setProperty("paramKey", key);
    return spin;
}

QWidget* PluginTestDialog::createBoolWidget(const QString& key, bool value)
{
    QCheckBox* check = new QCheckBox();
    check->setChecked(value);
    check->setProperty("paramKey", key);
    return check;
}

QWidget* PluginTestDialog::createStringWidget(const QString& key, const QString& value)
{
    QLineEdit* edit = new QLineEdit();
    edit->setText(value);
    edit->setProperty("paramKey", key);
    return edit;
}

void PluginTestDialog::onLoadImageClicked()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("选择图像"), "", tr("图像文件 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));

    if (!fileName.isEmpty()) {
        m_imagePathEdit->setText(fileName);

        QImage img(fileName);
        if (!img.isNull()) {
            m_imagePreview->setPixmap(QPixmap::fromImage(img).scaled(
                m_imagePreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_resultEdit->setTextColor(Qt::darkGreen);
            m_resultEdit->append(QString("[%1] 已加载图像: %2 (%3x%4)\n")
                .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
                .arg(fileName).arg(img.width()).arg(img.height()));
        } else {
            m_resultEdit->setTextColor(Qt::red);
            m_resultEdit->append(QString("[%1] 无法加载图像: %2\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss")).arg(fileName));
        }
    }
}

ImageData PluginTestDialog::createInputData()
{
    ImageData input;

    QString imagePath = m_imagePathEdit->text();
    if (!imagePath.isEmpty()) {
        QImage img(imagePath);
        if (!img.isNull()) {
            input = ImageData(img);
        }
    } else {
        // 创建一个默认测试图像 (200x200 灰度)
        ImageData defaultInput(200, 200, 1);
        input = defaultInput;
    }

    return input;
}

void PluginTestDialog::onExecuteClicked()
{
    if (!m_currentModule) {
        showTestResult(false, "请先选择一个插件");
        return;
    }

    // 收集参数
    QJsonObject params;
    for (auto it = m_paramWidgets.begin(); it != m_paramWidgets.end(); ++it) {
        QString key = it.key();
        QWidget* widget = it.value();

        if (QSpinBox* spin = qobject_cast<QSpinBox*>(widget)) {
            params[key] = spin->value();
        } else if (QDoubleSpinBox* spin = qobject_cast<QDoubleSpinBox*>(widget)) {
            params[key] = spin->value();
        } else if (QCheckBox* check = qobject_cast<QCheckBox*>(widget)) {
            params[key] = check->isChecked();
        } else if (QLineEdit* edit = qobject_cast<QLineEdit*>(widget)) {
            params[key] = edit->text();
        }
    }

    m_currentModule->setParams(params);

    // 创建输入数据
    ImageData input = createInputData();
    ImageData output;

    // 执行
    m_resultEdit->setTextColor(Qt::blue);
    m_resultEdit->append(QString("[%1] 开始执行: %2\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss")).arg(m_currentPluginName));

    bool success = m_currentModule->execute(input, output);

    if (success) {
        m_resultEdit->setTextColor(Qt::darkGreen);
        m_resultEdit->append(QString("[%1] 执行成功\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss")));
        updateResultDisplay(output);
    } else {
        m_resultEdit->setTextColor(Qt::red);
        m_resultEdit->append(QString("[%1] 执行失败\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss")));
    }
}

void PluginTestDialog::updateResultDisplay(const ImageData& output)
{
    m_resultEdit->append("\n--- 输出数据 ---");

    // 显示图像信息
    if (output.isValid()) {
        m_resultEdit->append(QString("图像: %1 x %2, 通道: %3")
            .arg(output.width()).arg(output.height()).arg(output.channels()));
    }

    // 显示元数据
    QMap<QString, QVariant> metadata = output.allData();
    for (auto it = metadata.begin(); it != metadata.end(); ++it) {
        QString valueStr;
        QVariant value = it.value();

        if (value.type() == QVariant::PointF) {
            QPointF p = value.toPointF();
            valueStr = QString("(%1, %2)").arg(p.x()).arg(p.y());
        } else if (value.canConvert<QVector<QPointF>>()) {
            QVector<QPointF> points = value.value<QVector<QPointF>>();
            valueStr = QString("[%1 点]").arg(points.size());
            for (int i = 0; i < qMin(3, points.size()); ++i) {
                valueStr += QString("\n  (%1, %2)").arg(points[i].x()).arg(points[i].y());
            }
            if (points.size() > 3) {
                valueStr += "\n  ...";
            }
        } else {
            valueStr = value.toString();
        }

        m_resultEdit->append(QString("%1: %2").arg(it.key()).arg(valueStr));
    }

    // 对比预期结果
    if (!m_expectedWidgets.isEmpty()) {
        m_resultEdit->append("\n--- 预期对比 ---");
        bool allMatch = true;

        for (auto it = m_expectedWidgets.begin(); it != m_expectedWidgets.end(); ++it) {
            QString key = it.value().first;
            QString expectedStr = it.value().second;

            if (output.hasData(key)) {
                QVariant actual = output.data(key);
                QString actualStr;

                if (actual.type() == QVariant::PointF) {
                    QPointF p = actual.toPointF();
                    actualStr = QString("(%1, %2)").arg(p.x()).arg(p.y());
                } else {
                    actualStr = actual.toString();
                }

                bool match = false;
                bool canParse = false;
                double actualNum = actualStr.toDouble(&canParse);
                double expectedNum = expectedStr.toDouble(&canParse);
                if (canParse) {
                    match = qAbs(actualNum - expectedNum) < 0.01;
                } else {
                    match = (actualStr == expectedStr);
                }

                if (match) {
                    m_resultEdit->setTextColor(Qt::darkGreen);
                    m_resultEdit->append(QString("✓ %1: 匹配 (实际: %2, 预期: %3)")
                        .arg(key).arg(actualStr).arg(expectedStr));
                } else {
                    m_resultEdit->setTextColor(Qt::red);
                    m_resultEdit->append(QString("✗ %1: 不匹配 (实际: %2, 预期: %3)")
                        .arg(key).arg(actualStr).arg(expectedStr));
                    allMatch = false;
                }
            } else {
                m_resultEdit->setTextColor(Qt::darkYellow);
                m_resultEdit->append(QString("? %1: 无此数据").arg(key));
            }
        }

        if (allMatch && !m_expectedWidgets.isEmpty()) {
            m_resultEdit->setTextColor(Qt::darkGreen);
            m_resultEdit->append("\n✓ 所有预期值匹配!");
        }
    }
}

void PluginTestDialog::showTestResult(bool success, const QString& message)
{
    if (success) {
        m_resultEdit->setTextColor(Qt::darkGreen);
    } else {
        m_resultEdit->setTextColor(Qt::red);
    }
    m_resultEdit->append(QString("[%1] %2\n").arg(QDateTime::currentDateTime().toString("hh:mm:ss")).arg(message));
}

void PluginTestDialog::onClearClicked()
{
    m_resultEdit->clear();
    m_imagePathEdit->clear();
    m_imagePreview->clear();
    m_imagePreview->setText(tr("(无图像)"));
    m_imagePreview->setPixmap(QPixmap());

    // 清除预期输入
    for (auto* w : m_expectedWidgets.keys()) {
        delete w;
    }
    m_expectedWidgets.clear();
}

void PluginTestDialog::onAddExpectedClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("添加预期结果"));
    QFormLayout form(&dialog);

    QLineEdit keyEdit;
    QLineEdit valueEdit;
    form.addRow(tr("键名:"), &keyEdit);
    form.addRow(tr("预期值:"), &valueEdit);

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);

    if (dialog.exec() == QDialog::Accepted && !keyEdit.text().isEmpty()) {
        QHBoxLayout* row = new QHBoxLayout();
        QLabel* keyLabel = new QLabel(keyEdit.text());
        QLabel* valueLabel = new QLabel(valueEdit.text());
        QLabel* sepLabel = new QLabel(" → ");

        row->addWidget(keyLabel);
        row->addWidget(sepLabel);
        row->addWidget(valueLabel);
        row->addStretch();

        QWidget* container = new QWidget();
        container->setLayout(row);

        m_expectedLayout->addWidget(container);
        m_expectedWidgets[container] = qMakePair(keyEdit.text(), valueEdit.text());
    }
}

void PluginTestDialog::onRemoveExpectedClicked()
{
    if (!m_expectedWidgets.isEmpty()) {
        QWidget* last = m_expectedWidgets.lastKey();
        m_expectedWidgets.remove(last);
        delete last;
    }
}

} // namespace DeepLux