#include "MainWindow.h"

#include "../ThemeManager.h"
#include "../bridge/TerminalBridge.h"
#include "../controllers/ProcessTreeController.h"
#include "../dialogs/AgentSettingsDialog.h"
#include "../dialogs/KnowledgeBaseDialog.h"
#include "../dialogs/LoginDialog.h"
#include "../dialogs/SamAnnotatorDialog.h"
#include "../display/3d/Viewport3DContent.h"
#include "../display/DisplayManager.h"
#include "../display/ImageRefreshCoalescer.h"
#include "../panels/DataSourcePanel.h"
#include "../widgets/AgentActionLogWidget.h"
#include "../widgets/AgentChatPanel.h"
#include "../widgets/AppIconProvider.h"
#include "../widgets/FlowCanvas.h"
#include "../widgets/HImageWidget.h"
#include "../widgets/ModuleInspectorPanel.h"
#include "../widgets/PluginDragPayload.h"
#include "../widgets/TerminalWidget.h"
#include "../widgets/ViewportWidget.h"
#include "CameraSetView.h"
#include "CommunicationSetView.h"
#include "GlobalVarView.h"
#include "SplashScreen.h"
#include "SystemParamView.h"
#include "core/agent/AgentActor.h"
#include "core/agent/AgentAuditLog.h"
#include "core/agent/AgentController.h"
#include "core/agent/AnnotationSessionExport.h"
#include "core/agent/AnnotationSessionOpen.h"
#include "core/agent/OpenAILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/common/ConfigWidgetHelper.h"
#include "core/common/Logger.h"
#include "core/common/ModuleIconProvider.h"
#include "core/common/UserManager.h"
#include "core/config/SystemConfig.h"
#include "core/device/CameraManager.h"
#include "core/display/DisplayData.h"
#include "core/display/IDisplayPort.h"
#include "core/engine/FlowExecutionService.h"
#include "core/engine/RunEngine.h"
#include "core/geometry/MeasurementData.h"
#include "core/interface/IModule.h"
#include "core/io/PlyLoader.h"
#include "core/io/TiffLoader.h"
#include "core/manager/ConfigManager.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Annotation.h"
#include "core/model/DataSource.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "core/platform/PathUtils.h"

#include <Eigen/Geometry>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUndoCommand>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace DeepLux {

AgentChatPanel* MainWindow::agentChatPanel() const {
    return m_agentChatPanel.data();
}

// 前向声明
class FlowCanvas;
class ModuleInspectorPanel;

/**
 * @brief 参数编辑撤销命令
 *
 * 同时更新工程（Project::setModuleParam）和运行时模块（IModule::setParam）。
 * undo/redo 均走此路径，确保两者保持一致。
 */
class SetModuleParamCommand : public QUndoCommand {
public:
    SetModuleParamCommand(IModule* module, Project* project, const QString& instanceId, const QJsonObject& before,
                          const QJsonObject& after, ModuleInspectorPanel* inspector, std::function<void(bool)> result,
                          QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_module(module), m_project(project), m_inspector(inspector), m_instanceId(instanceId),
          m_before(before), m_after(after), m_result(std::move(result)),
          m_expectedInstanceGeneration(project->parameterInstanceGeneration(instanceId)) {
        for (const QString& key : changedKeys(before, after)) {
            m_expected.insert(key, state(before, key));
            m_expectedAgentGenerations.insert(key, project->parameterGenerations(instanceId, key).agent);
        }
        setText(QCoreApplication::translate("MainWindow", "修改参数 %1").arg(instanceId));
    }

    void undo() override {
        apply(m_before);
    }
    void redo() override {
        apply(m_after);
    }

private:
    struct ValueState {
        bool present = false;
        QJsonValue value;
    };

    static QSet<QString> changedKeys(const QJsonObject& before, const QJsonObject& after) {
        QSet<QString> keys;
        for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
            if (!after.contains(it.key()) || after.value(it.key()) != it.value())
                keys.insert(it.key());
        }
        for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
            if (!before.contains(it.key()))
                keys.insert(it.key());
        }
        return keys;
    }

    static ValueState state(const QJsonObject& object, const QString& key) {
        return ValueState{object.contains(key), object.value(key)};
    }

    void expire() {
        if (m_invalid)
            return;
        m_invalid = true;
        if (m_result)
            m_result(false);
    }

    void apply(const QJsonObject& target) {
        if (m_invalid) {
            if (m_result)
                m_result(false);
            return;
        }
        Project* project = m_project.data();
        IModule* module = m_module.data();
        if (!project || ProjectManager::instance().currentProject() != project || !module ||
            project->parameterInstanceGeneration(m_instanceId) != m_expectedInstanceGeneration) {
            expire();
            return;
        }
        const std::optional<ModuleInstance> current = project->moduleById(m_instanceId);
        if (!current) {
            expire();
            return;
        }

        QJsonObject candidate = current->params;
        const QList<QString> keys = m_expectedAgentGenerations.keys();
        for (const QString& key : keys) {
            const ValueState expected = m_expected.value(key);
            const ValueState actual = state(current->params, key);
            const Project::ParameterGenerations generations = project->parameterGenerations(m_instanceId, key);
            if (generations.agent != m_expectedAgentGenerations.value(key) || actual.present != expected.present ||
                (actual.present && actual.value != expected.value)) {
                expire();
                return;
            }
            const ValueState desired = state(target, key);
            if (desired.present)
                candidate.insert(key, desired.value);
            else
                candidate.remove(key);
        }

        QJsonObject effective = module->defaultParams();
        for (auto it = candidate.constBegin(); it != candidate.constEnd(); ++it)
            effective.insert(it.key(), it.value());
        QString validationError;
        if (!module->validateParams(effective, validationError)) {
            expire();
            return;
        }

        QJsonObject runtimeParams = module->currentParams();
        const QJsonObject defaults = module->defaultParams();
        for (const QString& key : keys) {
            if (candidate.contains(key))
                runtimeParams.insert(key, candidate.value(key));
            else if (defaults.contains(key))
                runtimeParams.insert(key, defaults.value(key));
            else
                runtimeParams.remove(key);
        }
        module->setParams(runtimeParams);
        ModuleInstance updated = *current;
        updated.params = candidate;
        project->updateModule(m_instanceId, updated, Project::PositionChangeSource::Unspecified,
                              Project::ParameterChangeSource::Gui);
        for (const QString& key : keys)
            m_expected.insert(key, state(candidate, key));
        if (m_result)
            m_result(true);

        // 撤销/重做后延迟刷新检查器
        if (m_inspector && m_inspector->currentInstanceId() == m_instanceId) {
            QPointer<ModuleInspectorPanel> inspector = m_inspector;
            QMetaObject::invokeMethod(
                inspector.data(),
                [inspector]() {
                    if (inspector)
                        inspector->refreshFromModule();
                },
                Qt::QueuedConnection);
        }
    }

    QPointer<IModule> m_module;
    QPointer<Project> m_project;
    QPointer<ModuleInspectorPanel> m_inspector;
    QString m_instanceId;
    QJsonObject m_before;
    QJsonObject m_after;
    QHash<QString, ValueState> m_expected;
    QHash<QString, quint64> m_expectedAgentGenerations;
    std::function<void(bool)> m_result;
    quint64 m_expectedInstanceGeneration = 0;
    bool m_invalid = false;
};

namespace {
class SetModulePositionCommand final : public QUndoCommand {
public:
    SetModulePositionCommand(Project* project, QString instanceId, int oldX, int oldY, int newX, int newY,
                             quint64 expectedGuiGeneration, std::function<void()> expired)
        : m_project(project), m_instanceId(std::move(instanceId)), m_expectedX(oldX), m_expectedY(oldY), m_oldX(oldX),
          m_oldY(oldY), m_newX(newX), m_newY(newY), m_expectedGuiGeneration(expectedGuiGeneration),
          m_expired(std::move(expired)) {
        setText(QCoreApplication::translate("MainWindow", "移动模块 %1").arg(m_instanceId));
    }

    void undo() override {
        apply(m_oldX, m_oldY);
    }
    void redo() override {
        apply(m_newX, m_newY);
    }

private:
    void apply(int x, int y) {
        if (m_invalid) {
            if (m_expired)
                m_expired();
            return;
        }
        Project* project = m_project.data();
        if (!project || ProjectManager::instance().currentProject() != project) {
            expire();
            return;
        }
        const std::optional<ModuleInstance> current = project->moduleById(m_instanceId);
        const Project::PositionGenerations generations = project->positionGenerations(m_instanceId);
        if (!current || current->posX != m_expectedX || current->posY != m_expectedY ||
            generations.gui != m_expectedGuiGeneration) {
            expire();
            return;
        }
        if (current->posX == x && current->posY == y)
            return;
        ModuleInstance updated = *current;
        updated.posX = x;
        updated.posY = y;
        project->updateModule(m_instanceId, updated, Project::PositionChangeSource::Gui);
        m_expectedX = x;
        m_expectedY = y;
    }

    void expire() {
        if (m_invalid)
            return;
        m_invalid = true;
        if (m_expired)
            m_expired();
    }

    QPointer<Project> m_project;
    QString m_instanceId;
    int m_expectedX;
    int m_expectedY;
    int m_oldX;
    int m_oldY;
    int m_newX;
    int m_newY;
    quint64 m_expectedGuiGeneration;
    std::function<void()> m_expired;
    bool m_invalid = false;
};

QString cleanToolDisplayName(const QString& displayName) {
    const QString text = displayName.trimmed();
    const int firstSpace = text.indexOf(' ');
    if (firstSpace > 0 && !text.at(0).isLetterOrNumber()) {
        return text.mid(firstSpace + 1).trimmed();
    }
    return text;
}

QString measurementInputModeForConsumer(const QString& moduleId) {
    if (moduleId == QStringLiteral("com.deeplux.plugin.fitcircle")) {
        return QStringLiteral("point_set");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.distancepp")) {
        return QStringLiteral("point_pair");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.measuregap")) {
        return QStringLiteral("line_pair");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.distancepl")) {
        return QStringLiteral("point_line");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.linesdistance")) {
        return QStringLiteral("line_pair");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.pointsurfacedistance")) {
        return QStringLiteral("point_plane");
    }
    return QString();
}

QString measurementInputModeText(const QString& mode) {
    if (mode == QStringLiteral("point_pair")) {
        return QStringLiteral("点1、点2");
    }
    if (mode == QStringLiteral("point_line")) {
        return QStringLiteral("点、线段两端点");
    }
    if (mode == QStringLiteral("line_pair")) {
        return QStringLiteral("线1两端点、线2两端点");
    }
    if (mode == QStringLiteral("point_plane")) {
        return QStringLiteral("点、平面三点");
    }
    if (mode == QStringLiteral("point_set")) {
        return QStringLiteral("圆周上的 3 个点");
    }
    return mode;
}

int measurementPickCountForMode(const QString& mode) {
    if (mode == QStringLiteral("point_pair")) {
        return 2;
    }
    if (mode == QStringLiteral("point_line") || mode == QStringLiteral("point_set")) {
        return 3;
    }
    if (mode == QStringLiteral("line_pair") || mode == QStringLiteral("point_plane")) {
        return 4;
    }
    return 0;
}

QString measurementSummary(const ImageData& output) {
    QStringList parts;
    const QMap<QString, QVariant> all = output.allData();

    auto add = [&](const QString& key, const QString& label) {
        if (all.contains(key)) {
            double v = all[key].toDouble();
            parts.append(QString("%1=%2").arg(label).arg(v, 0, 'f', 3));
        }
    };

    add("distance", QString::fromUtf8("距离"));
    add("gap_distance", QString::fromUtf8("间隙距离"));
    add("gap_delta_z", QString::fromUtf8("ΔZ"));
    add("line_length", QString::fromUtf8("线段长度"));
    add("rect_width", QString::fromUtf8("宽度"));
    add("rect_height", QString::fromUtf8("高度"));
    add("surface_roughness", QString::fromUtf8("粗糙度"));

    if (all.contains("point_count")) {
        parts.append(QString::fromUtf8("点数=%1").arg(all["point_count"].toInt()));
    }

    return parts.isEmpty() ? QString() : QString::fromUtf8("测量: ") + parts.join(QString::fromUtf8(", "));
}

QJsonArray pointArray2D(const QPointF& point) {
    QJsonArray arr;
    arr.append(point.x());
    arr.append(point.y());
    return arr;
}

QJsonArray pointArray3D(double x, double y, double z) {
    QJsonArray arr;
    arr.append(x);
    arr.append(y);
    arr.append(z);
    return arr;
}

QString pointText2D(const QPointF& point) {
    return QString("%1,%2").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2);
}

QString pointText3D(const Eigen::Vector3d& point) {
    return QString("%1,%2,%3").arg(point.x(), 0, 'f', 3).arg(point.y(), 0, 'f', 3).arg(point.z(), 0, 'f', 3);
}

QPointF pointFromArray2D(const QJsonArray& arr, int offset = 0) {
    return QPointF(arr.at(offset).toDouble(), arr.at(offset + 1).toDouble());
}

Eigen::Vector3d pointFromArray3D(const QJsonArray& arr, int offset = 0) {
    const double z = arr.size() > offset + 2 ? arr.at(offset + 2).toDouble() : 0.0;
    return Eigen::Vector3d(arr.at(offset).toDouble(), arr.at(offset + 1).toDouble(), z);
}

Eigen::Vector3d linePointFromArray3D(const QJsonArray& arr, int offset = 0) {
    return Eigen::Vector3d(arr.at(offset).toDouble(), arr.at(offset + 1).toDouble(), 0.0);
}

QJsonArray updatedLineArray(const QJsonArray& existing, const QPointF& point, bool firstPoint) {
    QJsonArray line = existing;
    while (line.size() < 4) {
        line.append(0.0);
    }

    const int offset = firstPoint ? 0 : 2;
    line[offset] = point.x();
    line[offset + 1] = point.y();
    return line;
}

QJsonArray updatedPlaneArray(const QJsonArray& existing, const QJsonArray& point, int planePointIndex) {
    QJsonArray plane = existing;
    while (plane.size() < 9) {
        plane.append(0.0);
    }

    const int offset = qBound(0, planePointIndex, 2) * 3;
    for (int i = 0; i < 3; ++i) {
        plane[offset + i] = point[i];
    }
    return plane;
}

QString pluginConfigDialogStyle(bool isDark) {
    const QString bg = isDark ? QStringLiteral("#1e1e1e") : QStringLiteral("#f5f5f5");
    const QString surface = isDark ? QStringLiteral("#252525") : QStringLiteral("#ffffff");
    const QString border = isDark ? QStringLiteral("#3b4148") : QStringLiteral("#dce2e8");
    const QString text = isDark ? QStringLiteral("#ffffff") : QStringLiteral("#212121");
    const QString secondary = isDark ? QStringLiteral("#3a3a3a") : QStringLiteral("#f3f4f6");
    const QString secondaryHover = isDark ? QStringLiteral("#4a4a4a") : QStringLiteral("#e5e7eb");

    return QStringLiteral(
               "QDialog#PluginConfigDialog { background-color: %1; color: %4; }"
               "QDialog#PluginConfigDialog QScrollArea#PluginConfigScrollArea { background-color: %1; border: none; }"
               "QDialog#PluginConfigDialog QScrollArea#PluginConfigScrollArea QWidget { background-color: %2; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigOkButton { background-color: #0078d7; color: "
               "#ffffff; "
               "border: 1px solid #005a9e; border-radius: 4px; padding: 5px 14px; min-height: 28px; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigOkButton:hover { background-color: #1e8ad6; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigCancelButton { background-color: %5; color: %4; "
               "border: 1px solid %3; border-radius: 4px; padding: 5px 14px; min-height: 28px; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigCancelButton:hover { background-color: %6; }")
        .arg(bg, surface, border, text, secondary, secondaryHover);
}

void applyPluginConfigTheme(QWidget* root, bool isDark) {
    if (!root) {
        return;
    }

    const QString surface = isDark ? QStringLiteral("#252525") : QStringLiteral("#ffffff");
    const QString input = isDark ? QStringLiteral("#333333") : QStringLiteral("#ffffff");
    const QString border = isDark ? QStringLiteral("#555555") : QStringLiteral("#cccccc");
    const QString text = isDark ? QStringLiteral("#ffffff") : QStringLiteral("#212121");
    const QString accent = QStringLiteral("#0078d7");
    const QString hover = isDark ? QStringLiteral("#1e8ad6") : QStringLiteral("#1e8ad6");
    const QString spinButton = isDark ? QStringLiteral("#444444") : QStringLiteral("#e5e7eb");
    const QString spinHover = isDark ? QStringLiteral("#555555") : QStringLiteral("#d1d5db");

    const QString containerStyle = QStringLiteral("background-color: %1; color: %2;").arg(surface, text);
    const QString labelStyle = QStringLiteral("color: %1; background-color: transparent; border: none;").arg(text);
    const QString inputStyle =
        QStringLiteral("QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTextEdit { background-color: %1; color: %2; "
                       "border: 1px solid %3; border-radius: 4px; padding: 5px 8px; min-height: 26px; }"
                       "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QTextEdit:focus { "
                       "border: 1px solid %4; }"
                       "QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, "
                       "QDoubleSpinBox::down-button { background-color: %5; border-radius: 2px; }"
                       "QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, "
                       "QDoubleSpinBox::down-button:hover { background-color: %6; }"
                       "QComboBox QAbstractItemView { background-color: %1; color: %2; border: 1px solid %3; "
                       "selection-background-color: %4; selection-color: #ffffff; outline: 0; }"
                       "QComboBox::drop-down { border: none; width: 20px; }"
                       "QComboBox::down-arrow { image: none; border-left: 5px solid transparent; "
                       "border-right: 5px solid transparent; border-top: 5px solid %2; }")
            .arg(input, text, border, accent, spinButton, spinHover);
    const QString buttonStyle =
        QStringLiteral("QPushButton { background-color: %1; color: #ffffff; border: 1px solid #005a9e; "
                       "border-radius: 4px; padding: 5px 14px; min-height: 28px; }"
                       "QPushButton:hover { background-color: %2; }"
                       "QPushButton:disabled { background-color: %3; color: %4; border-color: %3; }")
            .arg(accent, hover, spinButton, isDark ? QStringLiteral("#9ca3af") : QStringLiteral("#777777"));
    const QString optionStyle =
        QStringLiteral("QCheckBox { color: %1; background-color: transparent; spacing: 6px; }"
                       "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %2; "
                       "border-radius: 3px; background-color: %3; }"
                       "QCheckBox::indicator:checked { background-color: %4; border-color: %4; }")
            .arg(text, border, input, accent);
    const QString groupStyle =
        QStringLiteral("QGroupBox { background-color: %1; color: %2; border: 1px solid %3; border-radius: 6px; "
                       "font-weight: bold; margin-top: 12px; padding-top: 12px; }"
                       "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; "
                       "background-color: %1; }")
            .arg(surface, text, border);

    if (QLayout* rootLayout = root->layout()) {
        const QMargins margins = rootLayout->contentsMargins();
        if (margins.isNull())
            rootLayout->setContentsMargins(12, 12, 12, 12);
        rootLayout->setSpacing(qMax(rootLayout->spacing(), 8));
    }

    for (QFormLayout* form : root->findChildren<QFormLayout*>()) {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(10);
    }

    QList<QWidget*> widgets{root};
    widgets.append(root->findChildren<QWidget*>());
    for (QWidget* widget : widgets) {
        if (qobject_cast<QLineEdit*>(widget) || qobject_cast<QSpinBox*>(widget) ||
            qobject_cast<QDoubleSpinBox*>(widget) || qobject_cast<QComboBox*>(widget) ||
            qobject_cast<QTextEdit*>(widget)) {
            widget->setStyleSheet(inputStyle);
            widget->setMinimumHeight(qMax(widget->minimumHeight(), widget->fontMetrics().height() + 14));
        } else if (qobject_cast<QPushButton*>(widget)) {
            widget->setStyleSheet(buttonStyle);
        } else if (QLabel* label = qobject_cast<QLabel*>(widget)) {
            label->setWordWrap(true);
            label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            label->setMinimumHeight(qMax(label->minimumHeight(), label->fontMetrics().height() + 6));
            label->setStyleSheet(labelStyle);
        } else if (qobject_cast<QCheckBox*>(widget)) {
            widget->setStyleSheet(optionStyle);
        } else if (qobject_cast<QGroupBox*>(widget)) {
            widget->setStyleSheet(groupStyle);
        } else {
            widget->setStyleSheet(containerStyle);
        }
    }
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), m_displayManager(new DisplayManager(this)) {

    setupUi();

    // 参数编辑撤销栈
    m_paramUndoStack = new QUndoStack(this);
    connect(m_paramUndoStack, &QUndoStack::canUndoChanged, m_undoAction, &QAction::setEnabled);
    connect(m_paramUndoStack, &QUndoStack::canRedoChanged, m_redoAction, &QAction::setEnabled);

    // 问题 1: 在 applyTheme() 之前从配置加载主题，确保持久化生效
    {
        ConfigManager& cfg = ConfigManager::instance();
        if (cfg.isInitialized()) {
            m_isDarkTheme = cfg.groupBool("appearance", "darkTheme", false);
        }
    }

    applyTheme();

    // RunEngine 信号连接 — 统一执行入口，MainWindow 只做 UI 高亮/状态更新
    // 使用 instanceName → item 映射实现 O(1) 查找
    connect(&RunEngine::instance(), &RunEngine::moduleStarted, this, [this](const QString& moduleName) {
        QTreeWidgetItem* item = m_processTreeController ? m_processTreeController->instanceItem(moduleName) : nullptr;
        if (item) {
            m_currentExecutingItem = item;
            // 删除普通运行过程中的 setCurrentItem 调用
            // 单步模式允许跟随刚执行模块，但检查器固定时不得切换
            bool canFollow = m_isStepMode && !(m_inspectorPanel && m_inspectorPanel->isPinned());
            if (canFollow && m_processTreeController) {
                m_processTreeController->setCurrentItem(item);
            }
            // 使用 data role 替代 setBackground，蓝色点=运行中
            setProcessItemStatus(item, QStringLiteral("running"), tr("执行中..."));
        }
    });

    connect(&RunEngine::instance(), &RunEngine::moduleFinished, this,
            [this](const QString& moduleName, bool success, int elapsedMs) {
                // t28 F1：只按名称从当前 controller 查找；不得回退到 m_currentExecutingItem。
                // 工程关闭/切换会 clear() 流程树并删除 item，迟到的 moduleFinished 若写原始
                // 指针会触发 use-after-free。
                QTreeWidgetItem* item =
                    m_processTreeController ? m_processTreeController->instanceItem(moduleName) : nullptr;
                if (item) {
                    if (success) {
                        clearExecutionHighlight(item);
                    } else {
                        setProcessItemStatus(item, QStringLiteral("failure"), tr("%1 ms").arg(elapsedMs));
                    }
                    m_moduleExecutionTimes[moduleName] = elapsedMs;
                    if (success) {
                        // 绿色=成功
                        setProcessItemStatus(item, QStringLiteral("success"), tr("%1 ms").arg(elapsedMs));
                    }
                }
                m_dirtyModuleIds.remove(moduleName);
                if (m_inspectorPanel && m_selectedModuleId == moduleName) {
                    m_inspectorPanel->setDirty(false);
                }
                // 后台执行（设计 §7.2/§7.3）：删除 moduleFinished → displayImage 的大图刷新路径；
                // 2D 大图只经合并器刷新，这里只更新节点状态/耗时/检查器，并保留 3D 点云的既有路径。
                const ImageData out = RunEngine::instance().moduleOutput(moduleName);
                if (out.isValid() && (m_selectedModuleId.isEmpty() || m_selectedModuleId == moduleName)) {
                    const auto cloud = MeasurementData::pointCloud(out, nullptr);
                    if (cloud && !cloud->isEmpty()) {
                        displayImage(out, QString(), moduleName);
                    }
                }
                // 更新检查器结果（选择模块时同时更新检查器）
                if (m_inspectorPanel && m_selectedModuleId == moduleName) {
                    m_inspectorPanel->setOutput(out, success, elapsedMs);
                }
            });

    // 阶段 B 复核(P2): 禁用/跳过节点在流程栏显示"已跳过"，不记为失败
    connect(&RunEngine::instance(), &RunEngine::moduleSkipped, this, [this](const QString& moduleName) {
        QTreeWidgetItem* item = m_processTreeController ? m_processTreeController->instanceItem(moduleName) : nullptr;
        if (item) {
            const bool disabled = RunEngine::instance().isModuleDisabled(moduleName);
            setProcessItemStatus(item, QStringLiteral("skipped"), disabled ? tr("已禁用") : tr("已跳过"));
        }
    });

    connect(&RunEngine::instance(), &RunEngine::breakpointHit, this, [this](const QString& moduleName) {
        QTreeWidgetItem* item = m_processTreeController ? m_processTreeController->instanceItem(moduleName) : nullptr;
        if (item) {
            m_currentExecutingItem = item;
            setProcessItemStatus(item, QStringLiteral("paused"), tr("断点暂停"));
        }
        updateBreakpointRunControls(true);
        if (m_runStatusLabel) {
            m_runStatusLabel->setText(tr("断点暂停：%1").arg(moduleName));
        }
        Logger::instance().info(tr("断点暂停：%1，点击运行继续").arg(moduleName), "Run");
    });

    connect(&RunEngine::instance(), &RunEngine::runStarted, this, [this]() {
        if (m_isCycleMode && m_isRunning && RunEngine::instance().runMode() == RunMode::RunCycle)
            resetFlowRunPresentation();
    });
    // 后台执行（设计 §8.1）：pauseHit 对 Breakpoint 与 User 都会发。断点已有 breakpointHit
    // 专门处理（断点文案），这里只处理用户暂停（Agent pause_flow / Service::pauseRun），
    // 做与断点同等的 UI 更新并区分原因，避免 Breakpoint 重复触发或状态被覆盖。
    connect(&RunEngine::instance(), &RunEngine::pauseHit, this, [this](const QString& moduleName, PauseReason reason) {
        if (reason != PauseReason::User) {
            return;
        }
        QTreeWidgetItem* item = m_processTreeController ? m_processTreeController->instanceItem(moduleName) : nullptr;
        if (item) {
            m_currentExecutingItem = item;
            setProcessItemStatus(item, QStringLiteral("paused"), tr("用户暂停"));
        }
        updateBreakpointRunControls(true);
        if (m_runStatusLabel) {
            m_runStatusLabel->setText(tr("用户暂停：%1").arg(moduleName));
        }
        Logger::instance().info(tr("用户暂停：%1，点击运行继续").arg(moduleName), "Run");
    });

    connect(&RunEngine::instance(), &RunEngine::runFinished, this, [this](const RunResult& result) {
        if (m_processTimeLabel) {
            m_processTimeLabel->setText(tr("总耗时：%1 ms").arg(result.elapsedMs));
        }
        if (m_runStatusLabel) {
            m_runStatusLabel->setText(tr("最近运行：%1 ms").arg(result.elapsedMs));
        }
        m_currentExecutingItem = nullptr;

        // 追加测量结果摘要到日志并更新叠加层
        const ImageData& lastOut = RunEngine::instance().lastOutput();
        if (lastOut.isValid()) {
            QString summary = measurementSummary(lastOut);
            if (!summary.isEmpty()) {
                Logger::instance().info(summary, "Measurement");
            }
        }

        // 后台执行（设计 §7.2）：runFinished 不代表 job 结束，Cycle 每轮的发帧刷新继续由
        // 50ms 合并器决定，不在此强制刷新；叠加统一在合并器渲染回调内消费帧内快照。
        // 后台执行（设计 §5.3 #8 / §8.1）：runFinished 只表示“一轮执行结束”（Cycle 每轮都会发），
        // 不得据此重启循环或做 job 级收尾；job 级收尾只走 FlowExecutionService::jobFinished。
        const FlowExecutionService& service = FlowExecutionService::instance();
        const RunObservation observation = RunEngine::instance().runObservation(result.runId);
        const bool belongsToFinishedServiceJob = !observation.sessionId.isEmpty() &&
                                                 observation.sessionId == service.lastSessionId();
        if (m_closeAwaitingServiceJob || service.isActive() || belongsToFinishedServiceJob) {
            return;
        }
        // 无 Service job 的同步引擎路径（CLI/测试兼容）：强制 flush 一次（等价于 Single job 结束），
        // 让最终快照帧与叠加立即生效，再沿用既有 UI 收尾与延迟关闭。
        if (m_imageCoalescer) {
            m_imageCoalescer->forceFlush();
        }
        if (m_closeWhenRunFinishes) {
            m_closeWhenRunFinishes = false;
            setUiRunningState(false, false);
            m_isStepMode = false;
            QTimer::singleShot(0, this, &QWidget::close);
            return;
        }
        setUiRunningState(false, false);
        m_isStepMode = false;
    });
    connect(&RunEngine::instance(), &RunEngine::cycleStopped, this, [this]() {
        const bool stoppedWhileWaitingForPick = !m_pendingMeasurementCycleSessionId.isEmpty();
        m_pendingMeasurementCycleSessionId.clear();
        if (stoppedWhileWaitingForPick) {
            ++m_measurementPickContinuationGeneration;
            clearPendingMeasurementPick();
            setUiRunningState(false, false);
        }
        if (m_isCycleMode)
            setUiRunningState(false, false);
    });

    // 后台执行（设计 §7.2/§7.3/§7.4）：所有大图刷新经 50ms latest-wins 合并器，
    // 渲染时保留用户视图；模块完成信号不再直接刷新大图。
    m_imageCoalescer = new ImageRefreshCoalescer(this);
    m_imageCoalescer->setRenderCallback([this](const DisplayFrame& frame) { renderDisplayFrame(frame); });
    connect(&RunEngine::instance(), &RunEngine::displayFrameReady, this,
            [this](const QString& viewportId, quint64 jobId, const QString& runId) {
                if (!m_imageCoalescer) {
                    return;
                }
                // 帧身份必须匹配：迟到通知不能把旧 job/旧轮的画面渲染到当前视图（设计 §7.3）。
                const DisplayFrame frame = RunEngine::instance().latestDisplayFrame(viewportId);
                if (!frame.matchesIdentity(jobId, runId) || runId != RunEngine::instance().lastRunId()) {
                    return;
                }
                m_imageCoalescer->setExpectedIdentity(jobId, runId);
                m_imageCoalescer->schedule(frame);
            });

    // 后台执行（设计 §8.1）：Service 驱动的按钮状态与 job 级收尾。
    // running/cycle/step 状态以 Service 的 job 类型为准；关闭请求只由 jobFinished 重试。
    {
        FlowExecutionService& service = FlowExecutionService::instance();
        connect(&service, &FlowExecutionService::jobStateChanged, this,
                [this](FlowExecutionService::State state, FlowExecutionService::JobType job) {
                    switch (state) {
                    case FlowExecutionService::State::Running:
                        setUiRunningState(true, job == FlowExecutionService::JobType::Cycle);
                        m_isStepMode = (job == FlowExecutionService::JobType::Step);
                        // 新 job：锁定显示帧身份并清空旧 job 的待渲染帧/叠加（设计 §7.3）。
                        if (m_imageCoalescer) {
                            m_imageCoalescer->clearPending();
                            m_imageCoalescer->setExpectedIdentity(RunEngine::instance().displayJobId(), QString());
                        }
                        clearMeasurementOverlays();
                        break;
                    case FlowExecutionService::State::Paused:
                        // 暂停期间保持“运行中”语义：运行按钮变为“继续”，循环标志不变。
                        m_isRunning = true;
                        m_isStepMode = false;
                        // 设计 §7.2：暂停是强制 flush 时点（中间帧无快照 → 清除叠加）。
                        if (m_imageCoalescer) {
                            m_imageCoalescer->forceFlush();
                        }
                        break;
                    case FlowExecutionService::State::Stopping:
                    case FlowExecutionService::State::Idle:
                        break;
                    }
                });
        connect(&service, &FlowExecutionService::jobFinished, this,
                [this](FlowExecutionService::JobType job, bool stopped) {
                    Q_UNUSED(job)
                    if (stopped) {
                        Logger::instance().info(tr("后台执行已停止"), "Run");
                    }
                    // 设计 §7.2：job 结束/停止是唯一强制 flush 时点（Cycle 每轮不 flush）。
                    if (m_imageCoalescer) {
                        m_imageCoalescer->forceFlush();
                    }
                    finishRunUi();
                });
        connect(&service, &FlowExecutionService::jobFailed, this,
                [this](FlowExecutionService::JobType job, const QString& error) {
                    Q_UNUSED(job)
                    Logger::instance().error(tr("后台执行失败：%1").arg(error), "Run");
                    if (m_runStatusLabel) {
                        m_runStatusLabel->setText(tr("运行失败：%1").arg(error));
                    }
                    if (m_imageCoalescer) {
                        m_imageCoalescer->forceFlush();
                    }
                    finishRunUi();
                });
    }

    // Initialize AgentController (Phase 1)
    AgentController::instance().initialize();
    AgentController::instance().registerDataSourceViewHandler(
        this, [this](const QString& toolName, const QJsonObject& params) {
            return executeAgentDataSourceViewAction(toolName, params);
        });
    AgentActor* agentActor = AgentController::instance().actor();
    connect(agentActor, &AgentActor::runEngineSyncRequested, this, [this, agentActor]() {
        if (syncModulesToRunEngine())
            agentActor->noteRunEngineSynchronized(ProjectManager::instance().currentProject());
    });
    connect(agentActor, &AgentActor::modulePositionSyncRequested, this, [this](const QString& instanceId) {
        const Project* project = ProjectManager::instance().currentProject();
        const std::optional<ModuleInstance> instance = project ? project->moduleById(instanceId) : std::nullopt;
        FlowNodeItem* node = m_flowCanvas ? m_flowCanvas->nodeItem(instanceId) : nullptr;
        if (instance && node)
            node->setPos(instance->posX, instance->posY);
    });
    connect(agentActor, &AgentActor::positionUndoExpired, this, [this](const QString& instanceId, bool redo) {
        statusBar()->showMessage(
            tr("模块 %1 的位置撤销记录已失效（%2），未修改工程").arg(instanceId, redo ? tr("重做") : tr("撤销")), 5000);
    });
    if (m_flowCanvas) {
        connect(m_flowCanvas, &FlowCanvas::nodeMoveStarted, this, [this](const QString& instanceId, const QPointF&) {
            m_positionDragActive = true;
            m_positionDragProject = ProjectManager::instance().currentProject();
            m_positionDragInstanceId = instanceId;
            m_positionDragStartX = 0;
            m_positionDragStartY = 0;
            m_positionDragGuiGeneration = 0;
            if (!m_positionDragProject)
                return;
            const auto instance = m_positionDragProject->moduleById(instanceId);
            if (!instance)
                return;
            m_positionDragStartX = instance->posX;
            m_positionDragStartY = instance->posY;
            m_positionDragGuiGeneration = m_positionDragProject->positionGenerations(instanceId).gui;
        });
        connect(m_flowCanvas, &FlowCanvas::nodeMoveFinished, this,
                [this](const QString& instanceId, const QPointF& startVisual, const QPointF& finalVisual,
                       bool crossedThreshold, bool canceled) {
                    if (!m_positionDragActive || m_positionDragInstanceId != instanceId)
                        return;
                    m_positionDragActive = false;
                    Project* project = m_positionDragProject.data();
                    const QString capturedId = m_positionDragInstanceId;
                    const int oldX = m_positionDragStartX;
                    const int oldY = m_positionDragStartY;
                    const quint64 guiGeneration = m_positionDragGuiGeneration;
                    m_positionDragProject.clear();
                    m_positionDragInstanceId.clear();

                    FlowNodeItem* node = m_flowCanvas ? m_flowCanvas->nodeItem(capturedId) : nullptr;
                    Project* currentProject = ProjectManager::instance().currentProject();
                    const auto current = currentProject ? currentProject->moduleById(capturedId) : std::nullopt;
                    if (!node)
                        return;
                    const auto alignToModel = [node, current]() {
                        if (current)
                            node->setPos(current->posX, current->posY);
                    };
                    if (!project || project != currentProject || !current) {
                        alignToModel();
                        statusBar()->showMessage(tr("工程已切换，节点位置未提交"), 4000);
                        return;
                    }
                    if (canceled || RunEngine::instance().isBusy()) {
                        alignToModel();
                        statusBar()->showMessage(tr("运行或维护期间取消节点移动"), 4000);
                        return;
                    }
                    if (project->positionGenerations(capturedId).gui != guiGeneration) {
                        alignToModel();
                        statusBar()->showMessage(tr("节点位置已被其他操作更新，未覆盖新位置"), 5000);
                        return;
                    }
                    if (!crossedThreshold) {
                        node->setPos(startVisual);
                        return;
                    }

                    QPoint quantized;
                    if (!FlowCanvas::quantizeNodePosition(finalVisual, &quantized)) {
                        alignToModel();
                        statusBar()->showMessage(tr("节点坐标超出有效范围，移动已取消"), 5000);
                        return;
                    }
                    const int newX = quantized.x();
                    const int newY = quantized.y();
                    if (current->posX == newX && current->posY == newY) {
                        node->setPos(newX, newY);
                        return;
                    }
                    if (RunEngine::instance().isBusy()) {
                        alignToModel();
                        statusBar()->showMessage(tr("运行或维护期间取消节点移动"), 4000);
                        return;
                    }
                    m_paramUndoStack->push(new SetModulePositionCommand(
                        project, capturedId, oldX, oldY, newX, newY, guiGeneration,
                        [this]() { statusBar()->showMessage(tr("该节点撤销记录已失效，未修改工程"), 5000); }));
                });
    }

    // Load Agent settings from ConfigManager (Phase 3)
    loadAgentSettings();

    // Connect ProjectManager signals to sync process tree with Project model
    connect(&ProjectManager::instance(), &ProjectManager::projectCreated, this, &MainWindow::onProjectOpened);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this, &MainWindow::onProjectOpened);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this, &MainWindow::onProjectClosed);

    // Connect viewport creating to forward picking signals
    connect(m_displayManager, &DisplayManager::viewportCreated, this, &MainWindow::onViewportCreated);
    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        onViewportCreated(viewport->viewportId(), viewport);
    }
    // 按模块身份记录视口归属：仅 2D 图像显示参与测量叠加绑定（点云不参与）
    connect(m_displayManager, &DisplayManager::dataDisplayed, this, [this](const QString& viewportId) {
        if (m_displayingImage2D && !m_displayingModuleId.isEmpty())
            m_moduleViewportIds.insert(m_displayingModuleId, viewportId);
        m_displayingModuleId.clear();
        m_displayingImage2D = false;
    });

    // Connect Agent action log to UI (will be set after m_agentActionLogWidget is created)

    // 阶段 9: 加载持久化设置
    loadSettings();
}

MainWindow::~MainWindow() {
    AgentController::instance().unregisterDataSourceViewHandler(this);
    if (m_samAnnotatorDialog) {
        delete m_samAnnotatorDialog.data();
        m_samAnnotatorDialog.clear();
    }
    TerminalBridge::instance().shutdown();
    if (m_splashScreen) {
        m_splashScreen->deleteLater();
    }
}

void MainWindow::showSplashScreen() {
    if (!m_splashScreen) {
        m_splashScreen = new SplashScreen();
    }
    m_splashScreen->show();
    m_splashScreen->setProgress(0, tr("正在初始化..."));
    QApplication::processEvents();
}

void MainWindow::hideSplashScreen() {
    if (m_splashScreen) {
        m_splashScreen->setProgress(100, tr("加载完成"));
        QApplication::processEvents();
        QTimer::singleShot(300, this, [this]() {
            if (m_splashScreen) {
                m_splashScreen->close();
                m_splashScreen->deleteLater();
                m_splashScreen = nullptr;
            }
            // splash 关闭后显示主窗口
            show();
        });
    }
}

void MainWindow::loadPluginsWithProgress() {
    showSplashScreen();
    m_splashScreen->setProgress(5, tr("初始化插件管理器..."));
    m_splashScreen->appendLog(tr("正在扫描插件..."));
    QApplication::processEvents();

    PluginManager::instance().initialize();

    m_failedPlugins.clear();

    int totalPlugins =
        PluginManager::instance().availableModules().size() + PluginManager::instance().availableCameras().size();

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoaded, this,
        [this](const QString& name) {
            m_splashScreen->appendLog(QString("<span style='color: #3498db;'>✓</span> %1").arg(name));
        },
        Qt::QueuedConnection);

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoadFailed, this,
        [this](const QString& name, const QString& error) {
            Q_UNUSED(error);
            m_failedPlugins.append(name);
            m_splashScreen->appendLog(QString("<span style='color: #e94560;'>✗</span> %1").arg(name));
        },
        Qt::QueuedConnection);

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoadProgress, this,
        [this, totalPlugins](int current, int total, const QString& name) {
            const int progressTotal = qMax(1, total);
            int progress = 30 + (current * 65) / progressTotal;
            m_splashScreen->setProgress(progress, tr("加载: %1 (%2/%3)").arg(name).arg(current).arg(total));
        },
        Qt::QueuedConnection);

    connect(
        &PluginManager::instance(), &PluginManager::allPluginsLoaded, this,
        [this]() {
            if (m_failedPlugins.isEmpty()) {
                m_splashScreen->appendLog("");
                m_splashScreen->appendLog(tr("<span style='color: #2ecc71;'>全部插件加载完成</span>"));
                m_splashScreen->setProgress(100, tr("加载完成"));
                QTimer::singleShot(500, this, [this]() { hideSplashScreen(); });
            } else {
                m_splashScreen->setProgress(100, tr("加载完成，有 %1 个插件失败").arg(m_failedPlugins.size()));
                m_splashScreen->showFailedPlugins(m_failedPlugins);
                QTimer::singleShot(3000, this, [this]() { hideSplashScreen(); });
            }
        },
        Qt::QueuedConnection);

    m_splashScreen->appendLog(tr("开始加载 %1 个插件...").arg(totalPlugins));
    m_splashScreen->setProgress(30, tr("正在加载插件..."));
    QApplication::processEvents();
    PluginManager::instance().loadAllPluginsAsync();
}

void MainWindow::setupUi() {
    setWindowTitle(tr("DeepLux Vision 1.0.0"));
    resize(1280, 800);
    // P1-5: 提高最小尺寸，确保工具栏和面板在最小窗口下仍可用
    setMinimumSize(900, 600);

    setupMenuBar();
    setupToolBar();
    setupStatusBar();
    setupMainLayout();

    // 显示启动画面并加载插件
    loadPluginsWithProgress();
}

void MainWindow::setupMenuBar() {
    // 文件菜单
    QMenu* fileMenu = menuBar()->addMenu(tr("文件 (&F)"));
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::NewFile, 24, QColor("#2563EB")), tr("新建方案"),
                        this, &MainWindow::onNewSolution);
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::List, 24, QColor("#2563EB")), tr("方案列表"), this,
                        &MainWindow::onSolutionList);
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::OpenFolder, 24, QColor("#D97706")), tr("打开"),
                        this, &MainWindow::onOpenProject);
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Save, 24, QColor("#2563EB")), tr("保存"), this,
                        &MainWindow::onSaveProject);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("退出"), this, &QWidget::close);

    // 参数菜单
    QMenu* paramMenu = menuBar()->addMenu(tr("参数 (&P)"));
    m_undoAction = paramMenu->addAction(tr("撤销"), this, [this]() {
        if (RunEngine::instance().isBusy()) {
            statusBar()->showMessage(tr("运行或维护期间不能撤销"), 4000);
            return;
        }
        if (m_paramUndoStack)
            m_paramUndoStack->undo();
    });
    m_undoAction->setShortcut(QKeySequence("Ctrl+Z"));
    m_undoAction->setEnabled(false);
    m_redoAction = paramMenu->addAction(tr("重做"), this, [this]() {
        if (RunEngine::instance().isBusy()) {
            statusBar()->showMessage(tr("运行或维护期间不能重做"), 4000);
            return;
        }
        if (m_paramUndoStack)
            m_paramUndoStack->redo();
    });
    m_redoAction->setShortcut(QKeySequence("Ctrl+Y"));
    m_redoAction->setEnabled(false);
    paramMenu->addSeparator();
    paramMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Variable, 20, QColor("#7C3AED")), tr("全局变量"),
                         this, &MainWindow::onGlobalVar);
    paramMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::User, 20, QColor("#2563EB")), tr("用户登录"),
                         this, &MainWindow::onUserLogin);

    // 视图菜单
    QMenu* viewMenu = menuBar()->addMenu(tr("视图 (&V)"));
    m_viewToolPanelAction = new QAction(tr("工具"), this);
    m_viewToolPanelAction->setObjectName(QStringLiteral("WorkspaceToolsPanelAction"));
    m_viewToolPanelAction->setCheckable(true);
    m_viewToolPanelAction->setChecked(true);
    connect(m_viewToolPanelAction, &QAction::toggled, this, &MainWindow::onToggleToolPanel);
    viewMenu->addAction(m_viewToolPanelAction);

    m_viewProcessPanelAction = new QAction(tr("流程"), this);
    m_viewProcessPanelAction->setObjectName(QStringLiteral("WorkspaceProcessPanelAction"));
    m_viewProcessPanelAction->setCheckable(true);
    m_viewProcessPanelAction->setChecked(true);
    connect(m_viewProcessPanelAction, &QAction::toggled, this, &MainWindow::onToggleProcessPanel);
    viewMenu->addAction(m_viewProcessPanelAction);

    m_viewBottomPanelAction = new QAction(tr("底部面板"), this);
    m_viewBottomPanelAction->setObjectName(QStringLiteral("WorkspaceBottomPanelAction"));
    m_viewBottomPanelAction->setCheckable(true);
    m_viewBottomPanelAction->setChecked(true);
    connect(m_viewBottomPanelAction, &QAction::toggled, this, &MainWindow::onToggleBottomPanel);
    viewMenu->addAction(m_viewBottomPanelAction);

    viewMenu->addSeparator();
    viewMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::QuickMode, 20, QColor("#D97706")), tr("快捷模式"),
                        this, &MainWindow::onQuickMode);
    viewMenu->addSeparator();
    viewMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Theme, 20, QColor("#F59E0B")), tr("切换主题"),
                        this, &MainWindow::onToggleTheme);

    // 3D 渲染模式（菜单项 + 横线分隔）
    m_renderSeparator = viewMenu->addSeparator();
    QActionGroup* renderGroup = new QActionGroup(this);
    renderGroup->setExclusive(true);

    m_renderActions[0] = viewMenu->addAction(tr("统一色"));
    m_renderActions[1] = viewMenu->addAction(tr("RGB"));
    m_renderActions[2] = viewMenu->addAction(tr("高度"));
    m_renderActions[3] = viewMenu->addAction(tr("强度"));
    m_renderActions[4] = viewMenu->addAction(tr("法向"));
    m_renderActions[5] = viewMenu->addAction(tr("BlinnPhong"));
    for (int i = 0; i < 6; ++i) {
        m_renderActions[i]->setCheckable(true);
        m_renderActions[i]->setData(i);
        m_renderActions[i]->setObjectName(QStringLiteral("RenderModeAction%1").arg(i));
        renderGroup->addAction(m_renderActions[i]);
    }
    m_renderActions[5]->setChecked(true);
    connect(renderGroup, &QActionGroup::triggered, this,
            [this](QAction* action) { onRenderModeChanged(action->data().toInt()); });
    updateRenderModeCombo();

    // 工具菜单
    QMenu* toolMenu = menuBar()->addMenu(tr("工具 (&T)"));
    toolMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Camera, 20, QColor("#9CA3AF")), tr("相机设置"),
                        this, &MainWindow::onCameraSettings);
    toolMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Communication, 20, QColor("#0891B2")),
                        tr("通讯设置"), this, &MainWindow::onCommSettings);
    toolMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Hardware, 24, QColor("#D1D5DB")), tr("硬件配置"),
                        this, &MainWindow::onHardwareConfig);
    QAction* agentSettingsAction = toolMenu->addAction(tr("Agent 设置"), this, [this]() {
        AgentSettingsDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            applyAgentSettings(dlg);
    });
    agentSettingsAction->setObjectName(QStringLiteral("AgentSettingsMenuAction"));

    // 调试入口默认不进入生产界面，需要时通过环境变量显式开启。
    if (qEnvironmentVariableIntValue("DEEPLUX_SHOW_DEBUG_MENU") > 0) {
        QMenu* debugMenu = menuBar()->addMenu(tr("Debug"));
        debugMenu->setObjectName("DebugMenu");
        debugMenu->addAction(tr("Test 3D Render"), this, &MainWindow::onTest3DRender);
    }

    // 帮助菜单
    QMenu* helpMenu = menuBar()->addMenu(tr("帮助 (&H)"));
    QAction* aboutAction = helpMenu->addAction(tr("关于"), this, &MainWindow::onAbout);
    aboutAction->setObjectName(QStringLiteral("AboutMenuAction"));

    auto* appHeader = new QWidget(menuBar());
    appHeader->setObjectName("AppBrandHeader");
    auto* appHeaderLayout = new QHBoxLayout(appHeader);
    appHeaderLayout->setContentsMargins(10, 0, 10, 0);
    appHeaderLayout->setSpacing(8);

    auto* appIcon = new QLabel(appHeader);
    appIcon->setPixmap(AppIconProvider::icon(AppIconProvider::Icon::Home, 20, QColor("#1677C8")).pixmap(20, 20));
    appHeaderLayout->addWidget(appIcon);

    auto* appName = new QLabel(tr("DeepLux Vision"), appHeader);
    appName->setObjectName("AppBrandName");
    appHeaderLayout->addWidget(appName);

    auto* divider = new QFrame(appHeader);
    divider->setObjectName("AppBrandDivider");
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedHeight(20);
    appHeaderLayout->addWidget(divider);

    auto* breadcrumb = new QWidget(appHeader);
    breadcrumb->setObjectName("ProjectBreadcrumb");
    breadcrumb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* breadcrumbLayout = new QHBoxLayout(breadcrumb);
    breadcrumbLayout->setContentsMargins(10, 0, 10, 0);
    breadcrumbLayout->setSpacing(6);

    auto* projectIcon = new QLabel(breadcrumb);
    projectIcon->setObjectName("ProjectBreadcrumbIcon");
    projectIcon->setPixmap(AppIconProvider::icon(AppIconProvider::Icon::Folder, 16, QColor("#2563EB")).pixmap(16, 16));
    breadcrumbLayout->addWidget(projectIcon);

    m_projectLabel = new QLabel(breadcrumb);
    m_projectLabel->setObjectName("ProjectBreadcrumbLabel");
    m_projectLabel->setMinimumWidth(100);
    m_projectLabel->setMaximumWidth(320);
    breadcrumbLayout->addWidget(m_projectLabel);
    appHeaderLayout->addWidget(breadcrumb);
    menuBar()->setCornerWidget(appHeader, Qt::TopLeftCorner);
    updateProjectContext(nullptr);
}

void MainWindow::setupToolBar() {
    QToolBar* mainToolbar = addToolBar(tr("主工具栏"));
    mainToolbar->setObjectName("MainToolBar");
    mainToolbar->setMovable(false);
    mainToolbar->setIconSize(QSize(24, 24));
    mainToolbar->setFixedHeight(ThemeManager::layoutMetrics().toolbarHeight);
    mainToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // 文件操作
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::NewFile, 24, QColor("#2563EB")), tr("新建方案"),
                           this, &MainWindow::onNewSolution);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::OpenFolder, 24, QColor("#D97706")), tr("打开"),
                           this, &MainWindow::onOpenProject);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Save, 24, QColor("#16a18a")), tr("保存"), this,
                           &MainWindow::onSaveProject);
    mainToolbar->addSeparator();

    // 运行控制
    QAction* runOnceAction =
        mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Play, 24, QColor("#16A34A")),
                               tr("单次运行"), this, &MainWindow::onRunOnce);
    runOnceAction->setObjectName(QStringLiteral("RunOnceAction"));
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Step, 24, QColor("#0F766E")), tr("单步"), this,
                           &MainWindow::onStepRun);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Cycle, 24, QColor("#2563EB")), tr("循环运行"),
                           this, &MainWindow::onRunCycle);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Stop, 24, QColor("#DC2626")), tr("停止"), this,
                           &MainWindow::onStop);
    mainToolbar->addSeparator();

    // 快速测量（无需加入流程）
    m_quickMeasureAction =
        mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Laser, 20, QColor("#0891B2")),
                               tr("快速测量"), this, &MainWindow::onQuickMeasure);
    m_quickMeasureAction->setCheckable(true);
    m_quickMeasureAction->setShortcut(QKeySequence(Qt::Key_M));
    // mainToolbar->addSeparator();

    // 快速标注（SAM）
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Design, 20, QColor("#7C3AED")), tr("快速标注"),
                           this, &MainWindow::onQuickAnnotate);
    // 方案列表和主题切换保留在菜单中，避免低频命令挤占主工具栏。
}

void MainWindow::setupStatusBar() {
    QStatusBar* status = statusBar();
    status->setSizeGripEnabled(false); // 禁用右下角 resize grip，避免黑块

    m_runStatusLabel = new QLabel(tr("最近运行：—"));
    m_runStatusLabel->setObjectName("RunStatusLabel");
    m_runStatusLabel->setMinimumWidth(140);
    status->addPermanentWidget(m_runStatusLabel);

    // §3.7：Agent token 用量（llmUsageReported 写入；无用量时隐藏）
    m_agentTokenLabel = new QLabel();
    m_agentTokenLabel->setObjectName("AgentTokenStatusLabel");
    m_agentTokenLabel->setMinimumWidth(120);
    m_agentTokenLabel->hide();
    status->addPermanentWidget(m_agentTokenLabel);

    m_userLabel = new QLabel(tr("用户：未登录"));
    m_userLabel->setObjectName("UserStatusLabel");
    m_userLabel->setMinimumWidth(150);
    status->addPermanentWidget(m_userLabel);

    m_timeLabel = new QLabel();
    m_timeLabel->setObjectName("TimeStatusLabel");
    m_timeLabel->setMinimumWidth(150);
    status->addPermanentWidget(m_timeLabel);

    // 更新时间的定时器
    QTimer* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this,
            [this]() { m_timeLabel->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")); });
    timer->start(1000);
    status->showMessage(tr("就绪"));
}

void MainWindow::setupMainLayout() {
    const LayoutMetrics metrics = ThemeManager::layoutMetrics();

    // 创建主 Splitter - 水平分割左右
    m_mainSplitter = new QSplitter(Qt::Horizontal);
    m_mainSplitter->setObjectName("MainSplitter");
    m_mainSplitter->setHandleWidth(metrics.splitterHandleWidth);
    m_mainSplitter->setChildrenCollapsible(false);
    m_mainSplitter->setOpaqueResize(true);

    QWidget* mainContentWidget = new QWidget();
    mainContentWidget->setObjectName("MainContentWidget");
    QVBoxLayout* mainContentLayout = new QVBoxLayout(mainContentWidget);
    const int outerGutter = metrics.baseSpacing;
    mainContentLayout->setContentsMargins(outerGutter, outerGutter, outerGutter, outerGutter);
    mainContentLayout->setSpacing(0);

    // ========== 左侧：工具面板（贯穿整个高度）==========
    QWidget* toolPanelWidget = new QWidget();
    toolPanelWidget->setObjectName("ToolPanelWidget");
    QVBoxLayout* toolPanelLayout = new QVBoxLayout(toolPanelWidget);
    toolPanelLayout->setContentsMargins(0, 0, 0, 0);
    toolPanelLayout->setSpacing(0);

    // 工具分类区域（带滚动）
    QScrollArea* toolCategoryScroll = new QScrollArea();
    toolCategoryScroll->setWidgetResizable(true);
    toolCategoryScroll->setObjectName("ToolCategoryScroll");

    QWidget* toolCategoryWidget = new QWidget();
    QVBoxLayout* toolCategoryLayout = new QVBoxLayout(toolCategoryWidget);
    toolCategoryLayout->setContentsMargins(6, 6, 6, 6);
    toolCategoryLayout->setSpacing(0);
    toolCategoryLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    toolCategoryWidget->setObjectName("ToolCategoryWidget");

    m_toolBoxTree = new QTreeWidget();
    m_toolBoxTree->setHeaderHidden(true);
    m_toolBoxTree->setDragEnabled(true);
    m_toolBoxTree->setDragDropMode(QAbstractItemView::DragOnly);
    m_toolBoxTree->setAnimated(true);
    m_toolBoxTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toolBoxTree->setMouseTracking(true);
    m_toolBoxTree->setAcceptDrops(false); // 禁用工具箱内部拖放，防止生成嵌套菜单
    m_toolBoxTree->setObjectName("ToolBoxTree");

    // 搜索框（带清除按钮）
    m_toolSearchEdit = new QLineEdit();
    m_toolSearchEdit->setObjectName("ToolSearchEdit");
    m_toolSearchEdit->setPlaceholderText(tr("搜索工具..."));
    m_toolSearchEdit->setClearButtonEnabled(true);
    m_toolSearchEdit->setMaximumHeight(28);
    connect(m_toolSearchEdit, &QLineEdit::textChanged, this, &MainWindow::filterToolBox);
    toolCategoryLayout->addWidget(m_toolSearchEdit);

    // 创建工具分类的 lambda，禁用分类标题的拖拽
    auto createCategoryItem = [&](QTreeWidget* tree, const QString& text, const QString& category) -> QTreeWidgetItem* {
        QString displayText = stripCategoryPrefix(text);
        QTreeWidgetItem* item = new QTreeWidgetItem(tree, QStringList(displayText));
        item->setData(0, Qt::UserRole, "category");
        item->setData(0, Qt::UserRole + 3, text);
        item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
        item->setIcon(0, ModuleIconProvider::instance().iconFor(category, category));
        return item;
    };

    // 工具分类（使用 createCategoryItem 禁用拖拽）
    QTreeWidgetItem* commonItem = createCategoryItem(m_toolBoxTree, tr("00 - 常用工具"), "common");
    commonItem->setExpanded(true);

    QTreeWidgetItem* imgProcItem = createCategoryItem(m_toolBoxTree, tr("01 - 图像处理"), "image_processing");
    imgProcItem->setExpanded(false);
    addToolBoxItem(imgProcItem, tr("图像采集"), "GrabImage");
    addToolBoxItem(imgProcItem, tr("保存图像"), "SaveImage");
    addToolBoxItem(imgProcItem, tr("显示图像"), "ShowImage");
    addToolBoxItem(imgProcItem, tr("图像预处理"), "PerProcessing");
    addToolBoxItem(imgProcItem, tr("颜色识别"), "ColorRecognition");
    addToolBoxItem(imgProcItem, tr("斑点分析"), "Blob");
    addToolBoxItem(imgProcItem, tr("内置图像操作"), "ImageScript");
    addToolBoxItem(imgProcItem, tr("拼图"), "JigsawPuzzle");

    QTreeWidgetItem* detectItem = createCategoryItem(m_toolBoxTree, tr("02 - 检测识别"), "detection");
    detectItem->setExpanded(false);
    addToolBoxItem(detectItem, tr("模板匹配"), "Matching");
    addToolBoxItem(detectItem, tr("二维码识别"), "QRCode");
    addToolBoxItem(detectItem, tr("缺陷检测"), "JiErHanDefectsDet");

    QTreeWidgetItem* geometryItem = createCategoryItem(m_toolBoxTree, tr("03 - 几何测量"), "geometry");
    geometryItem->setExpanded(false);
    geometryItem->setData(0, Qt::UserRole, "category");
    addToolBoxItem(geometryItem, tr("距离测量 (点到点)"), "DistancePP");
    addToolBoxItem(geometryItem, tr("距离测量 (点到线)"), "DistancePL");
    addToolBoxItem(geometryItem, tr("线段距离"), "LinesDistance");
    addToolBoxItem(geometryItem, tr("测量矩形"), "MeasureRect");
    addToolBoxItem(geometryItem, tr("测量直线"), "MeasureLine");
    addToolBoxItem(geometryItem, tr("测量间隙"), "MeasureGap");
    addToolBoxItem(geometryItem, tr("测量输入"), "MeasurementInput");
    addToolBoxItem(geometryItem, tr("自由曲面"), "FreeformSurface");
    addToolBoxItem(geometryItem, tr("点到平面距离"), "PointSurfaceDistance");

    QTreeWidgetItem* geoRelationItem = createCategoryItem(m_toolBoxTree, tr("04 - 几何关系"), "relation");
    geoRelationItem->setExpanded(false);
    addToolBoxItem(geoRelationItem, tr("找圆"), "FindCircle");
    addToolBoxItem(geoRelationItem, tr("圆拟合"), "FitCircle");
    addToolBoxItem(geoRelationItem, tr("直线拟合"), "FitLine");

    QTreeWidgetItem* calibItem = createCategoryItem(m_toolBoxTree, tr("05 - 坐标标定"), "calibration");
    calibItem->setExpanded(false);
    addToolBoxItem(calibItem, tr("N 点标定"), "NPointCalibration");

    QTreeWidgetItem* alignItem = createCategoryItem(m_toolBoxTree, tr("06 - 对位工具"), "alignment");
    alignItem->setExpanded(false);

    QTreeWidgetItem* logicItem = createCategoryItem(m_toolBoxTree, tr("07 - 逻辑工具"), "logic");
    logicItem->setExpanded(false);
    addToolBoxItem(logicItem, tr("如果"), "If");
    addToolBoxItem(logicItem, tr("循环"), "Loop");
    addToolBoxItem(logicItem, tr("While 循环"), "While");
    addToolBoxItem(logicItem, tr("停止循环"), "StopWhile");
    addToolBoxItem(logicItem, tr("条件判断"), "Condition");
    addToolBoxItem(logicItem, tr("延时"), "Delay");
    addToolBoxItem(logicItem, tr("并行执行"), "Parallel");
    addToolBoxItem(logicItem, tr("队列入"), "QueueIn");
    addToolBoxItem(logicItem, tr("队列出"), "QueueOut");

    QTreeWidgetItem* systemItem = createCategoryItem(m_toolBoxTree, tr("08 - 系统工具"), "system");
    systemItem->setExpanded(false);
    addToolBoxItem(systemItem, tr("系统时间"), "SystemTime");
    addToolBoxItem(systemItem, tr("文件夹操作"), "Folder");
    addToolBoxItem(systemItem, tr("显示点"), "ShowPoint");
    addToolBoxItem(systemItem, tr("时间切片"), "TimeSlice");

    QTreeWidgetItem* varItem = createCategoryItem(m_toolBoxTree, tr("09 - 变量工具"), "variable");
    varItem->setExpanded(false);
    addToolBoxItem(varItem, tr("变量定义"), "VarDefine");
    addToolBoxItem(varItem, tr("变量设置"), "VarSet");
    addToolBoxItem(varItem, tr("数学运算"), "Math");
    addToolBoxItem(varItem, tr("数据检查"), "DataCheck");
    addToolBoxItem(varItem, tr("显示数据"), "DisplayData");

    QTreeWidgetItem* fileCommItem = createCategoryItem(m_toolBoxTree, tr("10 - 文件通讯"), "file");
    fileCommItem->setExpanded(false);
    addToolBoxItem(fileCommItem, tr("保存数据"), "SaveData");
    addToolBoxItem(fileCommItem, tr("表格输出"), "TableOutPut");
    addToolBoxItem(fileCommItem, tr("写入文本"), "WriteText");

    QTreeWidgetItem* tool3DItem = createCategoryItem(m_toolBoxTree, tr("11 - 3D 工具"), "3d");
    tool3DItem->setExpanded(false);
    addToolBoxItem(tool3DItem, tr("加载点云"), "LoadPointCloud");

    QTreeWidgetItem* dlItem = createCategoryItem(m_toolBoxTree, tr("12 - 深度学习"), "deep_learning");
    dlItem->setExpanded(false);

    QTreeWidgetItem* strItem = createCategoryItem(m_toolBoxTree, tr("13 - 字符串处理"), "string");
    strItem->setExpanded(false);
    addToolBoxItem(strItem, tr("分割字符串"), "SplitString");
    addToolBoxItem(strItem, tr("字符串格式化"), "StrFormat");
    addToolBoxItem(strItem, tr("创建字符串"), "CreateString");

    QTreeWidgetItem* commItem = createCategoryItem(m_toolBoxTree, tr("14 - 通信"), "communication");
    commItem->setExpanded(false);
    addToolBoxItem(commItem, tr("PLC 通信"), "PLCCommunicate");
    addToolBoxItem(commItem, tr("PLC 读取"), "PLCRead");
    addToolBoxItem(commItem, tr("PLC 写入"), "PLCWrite");
    addToolBoxItem(commItem, tr("TCP 客户端"), "TCPClient");
    addToolBoxItem(commItem, tr("TCP 服务器"), "TCPServer");
    addToolBoxItem(commItem, tr("串口通信"), "SerialPort");

    connect(&PluginManager::instance(), &PluginManager::pluginLoaded, this, &MainWindow::updateToolBoxPluginItem);

    toolCategoryLayout->addWidget(m_toolBoxTree);
    toolCategoryScroll->setWidget(toolCategoryWidget);
    toolPanelLayout->addWidget(toolCategoryScroll);
    toolPanelLayout->setStretchFactor(toolCategoryScroll, 10);

    // 创建工具 DockWidget
    m_toolBoxDock = new QDockWidget(this);
    m_toolBoxDock->setObjectName("ToolPanelDock");
    m_toolBoxDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_toolBoxDock->setWidget(toolPanelWidget);
    m_toolBoxDock->setMinimumWidth(metrics.toolPanelWidth);
    m_toolBoxDock->setMaximumWidth(metrics.toolPanelWidth + 90);
    // 自定义标题栏
    QWidget* toolTitleWidget = new QWidget();
    QHBoxLayout* toolTitleLayout = new QHBoxLayout(toolTitleWidget);
    toolTitleLayout->setContentsMargins(10, 2, 4, 2);
    toolTitleLayout->setSpacing(5);
    QLabel* toolTitleLabel = new QLabel(tr("工具"));
    toolTitleLabel->setObjectName("ToolTitleLabel");
    toolTitleLayout->addWidget(toolTitleLabel);
    toolTitleLayout->addStretch();
    QToolButton* toolCloseBtn = new QToolButton();
    toolCloseBtn->setText("×");
    toolCloseBtn->setToolTip(tr("关闭"));
    toolCloseBtn->setFixedSize(metrics.compactIconButtonSize, metrics.compactIconButtonSize);
    toolCloseBtn->setObjectName("ToolCloseBtn");
    toolTitleLayout->addWidget(toolCloseBtn);
    toolTitleWidget->setFixedHeight(metrics.collapsedHeaderHeight);
    toolTitleWidget->setObjectName("ToolTitleWidget");
    m_toolBoxDock->setTitleBarWidget(toolTitleWidget);
    connect(toolCloseBtn, &QToolButton::clicked, this, [this]() {
        m_toolPanelUserClosed = true;
        m_toolBoxDock->setVisible(false);
        if (m_viewToolPanelAction)
            m_viewToolPanelAction->setChecked(false);
    });

    // 将工具面板添加到主 Splitter 左侧
    m_mainSplitter->addWidget(m_toolBoxDock);

    // ========== 右侧：垂直 Splitter（上方：流程 + 显示区域，下方：日志栏）==========
    m_rightSplitter = new QSplitter(Qt::Vertical);
    m_rightSplitter->setObjectName("RightSplitter");
    m_rightSplitter->setHandleWidth(metrics.splitterHandleWidth);
    m_rightSplitter->setChildrenCollapsible(false);
    m_rightSplitter->setOpaqueResize(true);

    // 右上方区域：水平 Splitter（流程面板 + 显示区域 + 检查器）
    m_rightTopSplitter = new QSplitter(Qt::Horizontal);
    m_rightTopSplitter->setObjectName("RightTopSplitter");
    m_rightTopSplitter->setHandleWidth(metrics.splitterHandleWidth);
    m_rightTopSplitter->setChildrenCollapsible(false);
    m_rightTopSplitter->setOpaqueResize(true);

    // ========== 流程面板（QTabWidget：流程 + 画布）==========
    QWidget* processPanelWidget = new QWidget();
    processPanelWidget->setObjectName("ProcessPanelWidget");
    QVBoxLayout* processPanelLayout = new QVBoxLayout(processPanelWidget);
    processPanelLayout->setContentsMargins(0, 0, 0, 0);
    processPanelLayout->setSpacing(0);

    m_processTabWidget = new QTabWidget(this);
    m_processTabWidget->setObjectName("ProcessTabWidget");
    m_processTabWidget->setDocumentMode(true);
    m_processTabWidget->tabBar()->setUsesScrollButtons(false);
    m_processTabWidget->tabBar()->setExpanding(false);
    m_processTabWidget->tabBar()->setElideMode(Qt::ElideNone);
    m_processTabWidget->tabBar()->setMinimumHeight(qMax(34, m_processTabWidget->tabBar()->fontMetrics().height() + 14));

    // ---- Tab 1: 流程 ----
    m_processTabContent = new QWidget();
    m_processTabContent->setObjectName("ProcessTabContent");
    QVBoxLayout* flowLayout = new QVBoxLayout(m_processTabContent);
    flowLayout->setContentsMargins(10, 8, 10, 8);
    flowLayout->setSpacing(6);

    auto* processControlBar = new QWidget(m_processTabContent);
    processControlBar->setObjectName("ProcessControlBar");
    auto* processControlLayout = new QHBoxLayout(processControlBar);
    processControlLayout->setContentsMargins(0, 0, 0, 0);
    processControlLayout->setSpacing(0);
    auto addProcessControl = [this, processControlBar, processControlLayout](
                                 const QString& objectName, AppIconProvider::Icon icon, const QColor& color,
                                 const QString& text, const QString& toolTip, auto slot) {
        auto* button = new QToolButton(processControlBar);
        button->setObjectName(objectName);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(AppIconProvider::icon(icon, 18, color));
        button->setIconSize(QSize(16, 16));
        button->setText(text);
        button->setToolTip(toolTip);
        // 固定水平尺寸，避免紧凑布局按像素压缩按钮导致文字被裁切
        // （不同平台/字体下 sizeHint 与布局取整差异会放大该问题）。
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        connect(button, &QToolButton::clicked, this, slot);
        processControlLayout->addWidget(button);
    };
    addProcessControl("FlowRunButton", AppIconProvider::Icon::Play, QColor("#16A34A"), tr("运行"), tr("运行流程"),
                      &MainWindow::onRunOnce);
    addProcessControl("FlowStepButton", AppIconProvider::Icon::Step, QColor("#0F766E"), tr("单步"), tr("单步执行"),
                      &MainWindow::onStepRun);
    addProcessControl("FlowCycleButton", AppIconProvider::Icon::Cycle, QColor("#2563EB"), tr("循环"), tr("循环运行"),
                      &MainWindow::onRunCycle);
    addProcessControl("FlowStopButton", AppIconProvider::Icon::Stop, QColor("#DC2626"), tr("停止"), tr("停止运行"),
                      &MainWindow::onStop);
    processControlLayout->addStretch();
    flowLayout->addWidget(processControlBar);

    // 模块树
    m_processTree = new QTreeWidget();
    m_processTree->setHeaderHidden(true);
    m_processTree->setAcceptDrops(true);
    m_processTree->setDragEnabled(true);
    m_processTree->setDragDropMode(QAbstractItemView::DragDrop);
    m_processTree->setAnimated(true);
    m_processTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTree->setDefaultDropAction(Qt::CopyAction);
    m_processTree->setDragDropOverwriteMode(false);
    m_processTree->setDropIndicatorShown(true);
    m_processTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_processTree->setObjectName("ProcessTree");
    m_processTree->setColumnCount(2);
    m_processTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_processTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_processTree->setColumnHidden(1, false); // 右列显示每个模块的最近执行耗时
    m_processTree->header()->setHidden(true);
    m_processTree->setUniformRowHeights(true);
    m_processTree->setIconSize(QSize(20, 20));
    m_processTree->viewport()->installEventFilter(this);
    m_processTree->installEventFilter(this);
    connect(m_processTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) { showProcessModuleOutput(item, true); });
    connect(m_processTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { showProcessModuleOutput(current); });

    // 创建流程树控制器
    m_processTreeController = new ProcessTreeController(m_processTree, this);
    m_processTreeController->setFlowModules(&m_flowModules);
    m_processTreeController->setModulesNeedSyncFlag(&m_modulesNeedSync);
    m_processTreeController->setMeasurementPickMaps(&m_measurementPickCursor, &m_measurementPickCount);
    m_processTreeController->setToolDisplayNameCallback(
        [this](const QString& pluginName, const QString& fallback) { return toolDisplayName(pluginName, fallback); });
    m_processTreeController->setClearMeasurementOverlaysCallback([this]() { clearMeasurementOverlays(); });
    connect(m_processTreeController, &ProcessTreeController::moduleAdded, this, [this](const ModuleInstance& module) {
        if (m_flowCanvas && !m_flowCanvas->nodeItem(module.id)) {
            QPointF pos(module.posX, module.posY);
            if (pos.isNull()) {
                const int nodeCount = m_flowCanvas->nodeIds().size();
                pos = QPointF(-110, -220 + nodeCount * 96);
            }
            const QString displayName =
                module.name.trimmed().isEmpty() ? toolDisplayName(module.moduleId, module.name) : module.name;
            m_flowCanvas->addNode(module.moduleId, displayName, pos, module.id);
        }
    });
    connect(m_processTreeController, &ProcessTreeController::moduleRemoved, this, [this](const QString& instanceId) {
        m_currentExecutingItem = nullptr;
        m_dirtyModuleIds.remove(instanceId);
        if (m_activeMeasurementInputId == instanceId) {
            m_activeMeasurementInputId.clear();
        }
        if (m_pendingMeasurementInputId == instanceId) {
            m_pendingMeasurementInputId.clear();
        }
        if (m_flowCanvas && m_flowCanvas->nodeItem(instanceId)) {
            m_flowCanvas->removeNode(instanceId);
        }
        // Fix: 删除当前选中模块时清空检查器和撤销栈，防止悬空指针
        if (m_selectedModuleId == instanceId) {
            m_selectedModuleId.clear();
            if (m_inspectorPanel) {
                m_inspectorPanel->clear();
            }
            if (m_paramUndoStack) {
                m_paramUndoStack->clear();
            }
        }
        m_flowModules.remove(instanceId);
    });
    // Fix: 在模块删除前清空检查器和撤销栈
    connect(m_processTreeController, &ProcessTreeController::moduleBeingRemoved, this,
            [this](const QString& instanceId) {
                if (m_selectedModuleId == instanceId) {
                    m_selectedModuleId.clear();
                    if (m_inspectorPanel) {
                        m_inspectorPanel->clear();
                    }
                    if (m_paramUndoStack) {
                        m_paramUndoStack->clear();
                    }
                    m_dirtyModuleIds.remove(instanceId);
                }
                if (m_activeMeasurementInputId == instanceId) {
                    m_activeMeasurementInputId.clear();
                }
                if (m_pendingMeasurementInputId == instanceId) {
                    m_pendingMeasurementInputId.clear();
                }
            });

    flowLayout->addWidget(m_processTree);
    flowLayout->setStretchFactor(m_processTree, 1);

    // 提示标签（由控制器管理）
    m_processTreeController->showHintLabel();

    // 流程状态栏
    m_processStatusWidget = new QWidget();
    m_processStatusWidget->setObjectName("ProcessStatusBar");
    QHBoxLayout* processStatusLayout = new QHBoxLayout(m_processStatusWidget);
    processStatusLayout->setContentsMargins(8, 5, 8, 5);
    m_processTimeLabel = new QLabel(tr("总耗时：0 ms"));
    processStatusLayout->addWidget(m_processTimeLabel);
    processStatusLayout->addStretch();
    flowLayout->addWidget(m_processStatusWidget);
    flowLayout->setStretchFactor(m_processStatusWidget, 0);

    m_processTabWidget->addTab(m_processTabContent, tr("流程"));

    // ---- Tab 2: 画布 ----
    m_flowCanvas = new FlowCanvas(this);
    m_flowCanvas->setObjectName("FlowCanvas");
    m_flowCanvas->setStyleSheet(QString());
    m_processTabWidget->addTab(m_flowCanvas, tr("画布"));

    // ---- Tab 3: 数据源 ----
    m_dataSourcePanel = new DataSourcePanel(this);
    m_dataSourcePanel->setObjectName("DataSourcePanel");
    m_processTabWidget->addTab(m_dataSourcePanel, tr("数据源"));

    // 连接 DataSourcePanel 信号
    connect(m_dataSourcePanel, &DataSourcePanel::requestDisplay, this, &MainWindow::onDisplayDataSource);
    connect(m_dataSourcePanel, &DataSourcePanel::requestRemove, this, &MainWindow::onRemoveDataSource);
    connect(m_dataSourcePanel, &DataSourcePanel::requestVisibilityChanged, this,
            &MainWindow::onDataSourceVisibilityChanged);
    connect(m_dataSourcePanel, &DataSourcePanel::requestDisplayInNewViewport, this,
            &MainWindow::onDisplayDataSourceInNewViewport);
    connect(m_dataSourcePanel, &DataSourcePanel::requestShowInFolder, this, &MainWindow::onShowDataSourceInFolder);
    connect(m_dataSourcePanel, &DataSourcePanel::requestCopyPath, this, &MainWindow::onCopyDataSourcePath);

    processPanelLayout->addWidget(m_processTabWidget);

    // 图像显示区域 - 使用 DisplayManager 的中央视口
    QWidget* imageDisplayWidget = m_displayManager->centralDisplay();
    imageDisplayWidget->setObjectName("ImageDisplayWidget");
    imageDisplayWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageDisplayWidget->setMinimumSize(0, 0);
    imageDisplayWidget->setAcceptDrops(true);     // 接收文件拖放
    imageDisplayWidget->installEventFilter(this); // route drops to eventFilter

    m_rightTopSplitter->addWidget(processPanelWidget);

    processPanelWidget->setMinimumWidth(metrics.flowPanelWidth);
    processPanelWidget->setMaximumWidth(metrics.flowPanelWidth + 40);

    m_rightTopSplitter->addWidget(imageDisplayWidget);
    // 主视图在窄窗口下仍保留可用的结果检视区域。
    imageDisplayWidget->setMinimumWidth(520);

    // ========== 右侧检查器面板 ==========
    m_inspectorPanel = new ModuleInspectorPanel();
    m_inspectorPanel->setObjectName("ModuleInspectorPanel");
    m_inspectorPanel->setMinimumWidth(metrics.inspectorMinWidth);
    m_inspectorPanel->setMaximumWidth(metrics.inspectorMaxWidth);
    m_rightTopSplitter->addWidget(m_inspectorPanel);

    m_rightTopSplitter->setStretchFactor(0, 1); // 流程面板
    m_rightTopSplitter->setStretchFactor(1, 8); // 主视图
    m_rightTopSplitter->setStretchFactor(2, 0); // 检查器

    // 设置检查器初始宽度
    m_rightTopSplitter->setSizes({metrics.flowPanelWidth, 800, metrics.inspectorWidth});

    // 连接检查器信号
    connect(m_inspectorPanel, &ModuleInspectorPanel::paramsChanged, this,
            [this](const QString& instanceId, const QString& key, const QVariant& value) {
                if (rejectBusyFlowEdit(tr("修改模块参数"))) {
                    m_inspectorPanel->refreshFromModule();
                    return;
                }
                // 推入参数撤销栈（同时更新运行时模块和工程）
                pushParamCommand(instanceId, key, value);
                m_modulesNeedSync = true;
            });
    // 阶段 B 复核: 检查器启用/断点/备注编辑写回 Project 并同步 RunEngine
    auto applyRuntimeField = [this](const QString& instanceId, const QString& action,
                                    std::function<void(ModuleInstance&)> mut,
                                    std::function<void(const ModuleInstance&)> restore) {
        Project* project = ProjectManager::instance().currentProject();
        if (!project)
            return;
        if (rejectBusyFlowEdit(action)) {
            const auto current = project->moduleById(instanceId);
            if (current && m_inspectorPanel)
                restore(*current);
            return;
        }
        if (auto inst = project->moduleById(instanceId)) {
            ModuleInstance updated = *inst;
            mut(updated);
            project->updateModule(instanceId, updated);
            RunEngine& engine = RunEngine::instance();
            engine.setModuleDisabled(instanceId, !updated.enabled);
            engine.setBreakpoint(instanceId, updated.breakpoint);
            m_modulesNeedSync = true;
        }
    };
    connect(m_inspectorPanel, &ModuleInspectorPanel::enabledToggled, this,
            [this, applyRuntimeField](const QString& id, bool enabled) {
                applyRuntimeField(
                    id, QObject::tr("修改模块启用状态"), [enabled](ModuleInstance& m) { m.enabled = enabled; },
                    [this](const ModuleInstance& current) {
                        if (m_inspectorPanel)
                            m_inspectorPanel->setEnabledState(current.enabled);
                    });
            });
    connect(m_inspectorPanel, &ModuleInspectorPanel::breakpointToggled, this,
            [this, applyRuntimeField](const QString& id, bool bp) {
                applyRuntimeField(
                    id, QObject::tr("修改模块断点"), [bp](ModuleInstance& m) { m.breakpoint = bp; },
                    [this](const ModuleInstance& current) {
                        if (m_inspectorPanel)
                            m_inspectorPanel->setBreakpointState(current.breakpoint);
                    });
            });
    connect(m_inspectorPanel, &ModuleInspectorPanel::noteChanged, this,
            [this, applyRuntimeField](const QString& id, const QString& note) {
                applyRuntimeField(
                    id, QObject::tr("修改模块备注"), [note](ModuleInstance& m) { m.note = note; },
                    [this](const ModuleInstance& current) {
                        if (m_inspectorPanel)
                            m_inspectorPanel->setNoteState(current.note);
                    });
            });
    // 恢复默认值作为一个多键命令，保证 stale/验证失败时整组原子失效。
    connect(m_inspectorPanel, &ModuleInspectorPanel::resetDefaultsRequested, this, [this](const QString& instanceId) {
        if (rejectBusyFlowEdit(tr("恢复模块默认参数"))) {
            m_inspectorPanel->refreshFromModule();
            return;
        }
        IModule* module = m_flowModules.value(instanceId, nullptr);
        Project* project = ProjectManager::instance().currentProject();
        const auto instance = project ? project->moduleById(instanceId) : std::nullopt;
        if (!module || !project || !instance || !m_paramUndoStack)
            return;
        const QJsonObject before = instance->params;
        QJsonObject after = before;
        const QJsonObject defaults = module->defaultParams();
        for (auto it = defaults.begin(); it != defaults.end(); ++it) {
            if (it.key().endsWith("_options"))
                continue;
            after.insert(it.key(), it.value());
        }
        if (after == before)
            return;
        auto* command = new SetModuleParamCommand(
            module, project, instanceId, before, after, m_inspectorPanel, [this](bool succeeded) {
                if (!succeeded)
                    statusBar()->showMessage(tr("恢复默认参数已过期，未应用任何更改"), 5000);
            });
        m_paramUndoStack->push(command);
    });
    connect(m_inspectorPanel, &ModuleInspectorPanel::rerunRequested, this, [this]() {
        if (!m_selectedModuleId.isEmpty()) {
            executeFlowOnce();
        }
    });
    connect(m_inspectorPanel, &ModuleInspectorPanel::advancedConfigRequested, this,
            [this](const QString& instanceId) { openAdvancedPluginConfig(instanceId); });
    connect(m_inspectorPanel, &ModuleInspectorPanel::pinChanged, this, [this](bool pinned) {
        Q_UNUSED(pinned)
        // 固定状态由检查器自行管理，此处仅记录
    });
    // 高: 手动折叠时重分配 splitter 空间
    connect(m_inspectorPanel, &ModuleInspectorPanel::collapseToggled, this, [this](bool collapsed) {
        if (!m_rightTopSplitter || !m_inspectorPanel)
            return;
        QList<int> sizes = m_rightTopSplitter->sizes();
        const int inspIdx = m_rightTopSplitter->indexOf(m_inspectorPanel);
        if (inspIdx < 0 || inspIdx >= sizes.size())
            return;
        if (collapsed) {
            int freed = sizes[inspIdx] - 32;
            sizes[inspIdx] = 32;
            if (sizes.size() > 1)
                sizes[1] += freed;
        } else {
            sizes[inspIdx] = 280;
            if (sizes.size() > 1)
                sizes[1] = qMax(620, sizes[1] - 280);
        }
        m_rightTopSplitter->setSizes(sizes);
    });
    connect(m_inspectorPanel, &ModuleInspectorPanel::closeRequested, this, [this]() { setInspectorVisibility(false); });

    // 连接流程树选择信号到检查器
    connect(m_flowCanvas, &FlowCanvas::editRejected, this,
            [this](const QString& reason) { statusBar()->showMessage(reason, 5000); });
    connect(m_flowCanvas, &FlowCanvas::nodeSelected, this,
            [this](const QString& nodeId) { selectModule(nodeId, true); });
    connect(m_flowCanvas, &FlowCanvas::connectionRequest, this,
            [this](const QString& fromId, const QString& fromPort, const QString& toId, const QString& toPort) {
                if (rejectBusyFlowEdit(tr("创建流程连线")))
                    return;
                Project* project = ProjectManager::instance().currentProject();
                if (!project)
                    return;
                // P1-fix: 根据两端端口声明推断边类型
                FlowNodeItem* fromNode = m_flowCanvas->nodeItem(fromId);
                FlowNodeItem* toNode = m_flowCanvas->nodeItem(toId);
                bool isControl = false;
                if (fromNode && toNode) {
                    for (const PortSpec& spec : fromNode->outputPortSpecs()) {
                        if (spec.id == fromPort && spec.control) {
                            for (const PortSpec& inSpec : toNode->inputPortSpecs()) {
                                if (inSpec.id == toPort && inSpec.control) {
                                    isControl = true;
                                }
                            }
                        }
                    }
                }
                ModuleConnection conn;
                conn.fromModuleId = fromId;
                conn.toModuleId = toId;
                conn.fromPort = fromPort;
                conn.toPort = toPort;
                conn.edgeType = isControl ? QStringLiteral("control") : QStringLiteral("data");
                project->addConnection(conn);
            });

    m_rightSplitter->addWidget(m_rightTopSplitter);

    // 日志面板 - 下方区域
    m_logDock = new QDockWidget(this);
    m_logDock->setObjectName("LogDock");
    m_logDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable);

    // 创建 Tab 容器（日志 + 终端）
    m_logTerminalTabs = new QTabWidget();
    m_logTerminalTabs->setObjectName("LogTerminalTabs");
    m_logTerminalTabs->setMovable(false);
    m_logTerminalTabs->setDocumentMode(true);
    m_logTerminalTabs->tabBar()->setMinimumHeight(qMax(30, m_logTerminalTabs->tabBar()->fontMetrics().height() + 10));

    // 隐藏 dock 标题栏，节省空间
    m_logDock->setTitleBarWidget(new QWidget());

    // ===== Tab 1: 日志面板 =====
    QWidget* logWidget = new QWidget();
    logWidget->setObjectName(QStringLiteral("RuntimeLogTab"));
    logWidget->setStyleSheet("background-color: transparent;");
    QVBoxLayout* logLayout = new QVBoxLayout(logWidget);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->setSpacing(0);

    m_logTable = new QTableWidget();
    m_logTable->setColumnCount(3);
    m_logTable->setHorizontalHeaderLabels(QStringList() << tr("时间") << tr("级别 ▼") << tr("消息"));
    m_logTable->horizontalHeader()->setStretchLastSection(true);
    m_logTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_logTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_logTable->setColumnWidth(0, 90);
    m_logTable->setColumnWidth(1, 70);

    // vertical header 固定宽度，替代重复的水平序号列
    const int logTextHeight = m_logTable->fontMetrics().height();
    m_logTable->horizontalHeader()->setFixedHeight(qMax(30, logTextHeight + 10));
    m_logTable->verticalHeader()->setDefaultSectionSize(qMax(26, logTextHeight + 6));
    m_logTable->verticalHeader()->setVisible(false);
    m_logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_logTable->setObjectName("LogTable");
    m_logTable->setFrameShape(QFrame::NoFrame);

    logLayout->addWidget(m_logTable);

    // 连接日志信号到表格
    connect(&Logger::instance(), &Logger::logAdded, this, &MainWindow::onLogAdded);
    connect(&Logger::instance(), &Logger::logsCleared, m_logTable, [this]() { m_logTable->setRowCount(0); });

    // 点击表头"级别"列弹出筛选菜单
    connect(m_logTable->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int logicalIndex) {
        if (logicalIndex == 1)
            showLogLevelMenu();
    });

    int logTabIndex = m_logTerminalTabs->addTab(logWidget, tr("运行日志"));

    auto* logActions = new QWidget(m_logTerminalTabs);
    logActions->setObjectName("LogCornerActions");
    auto* logActionsLayout = new QHBoxLayout(logActions);
    logActionsLayout->setContentsMargins(0, 0, 4, 0);
    logActionsLayout->setSpacing(2);
    auto* clearLogButton = new QToolButton(logActions);
    clearLogButton->setObjectName("ClearLogButton");
    clearLogButton->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Clear, 16, QColor("#64748B")));
    clearLogButton->setToolTip(tr("清空日志"));
    clearLogButton->setAutoRaise(true);
    clearLogButton->setFixedSize(28, 28);
    connect(clearLogButton, &QToolButton::clicked, &Logger::instance(), &Logger::clearLogs);
    logActionsLayout->addWidget(clearLogButton);
    auto* collapseLogButton = new QToolButton(logActions);
    collapseLogButton->setObjectName("CollapseLogButton");
    collapseLogButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarShadeButton));
    collapseLogButton->setToolTip(tr("收起底部面板"));
    collapseLogButton->setAutoRaise(true);
    collapseLogButton->setFixedSize(28, 28);
    connect(collapseLogButton, &QToolButton::clicked, m_logDock, &QDockWidget::hide);
    logActionsLayout->addWidget(collapseLogButton);
    m_logTerminalTabs->setCornerWidget(logActions, Qt::TopRightCorner);

    // 双击日志标签打开落盘文件；单击只切换面板，避免抢焦点。
    m_logTerminalTabs->setTabToolTip(logTabIndex, tr("运行日志；双击打开日志文件"));
    connect(m_logTerminalTabs->tabBar(), &QTabBar::tabBarDoubleClicked, this, [this, logTabIndex](int index) {
        if (index == logTabIndex) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(Logger::instance().logFilePath()));
        }
    });

    // ===== Tab 2: 终端 =====
    m_terminalWidget = new TerminalWidget();
    m_terminalWidget->setObjectName(QStringLiteral("TerminalLogTab"));
    TerminalBridge::instance().initialize(m_terminalWidget);
    m_logTerminalTabs->addTab(m_terminalWidget, tr("终端"));

    // ===== Tab 3: Agent 对话 =====
    m_agentChatPanel = new AgentChatPanel(m_logTerminalTabs);
    m_agentChatTabIndex = m_logTerminalTabs->addTab(m_agentChatPanel, tr("Agent 对话"));
    m_logTerminalTabs->setTabToolTip(m_agentChatTabIndex, tr("与 Agent 对话"));
    auto* expandChatButton = new QToolButton(logActions);
    expandChatButton->setObjectName("ExpandAgentChatButton");
    expandChatButton->setCheckable(true);
    expandChatButton->setAutoRaise(true);
    expandChatButton->setFixedSize(28, 28);
    expandChatButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
    expandChatButton->setToolTip(tr("展开对话"));
    expandChatButton->setAccessibleName(tr("展开对话"));
    expandChatButton->hide();
    logActionsLayout->insertWidget(1, expandChatButton);
    connect(expandChatButton, &QToolButton::toggled, this,
            [this, expandChatButton, savedSizes = QList<int>(), savedMax = 0](bool expanded) mutable {
                if (expanded) {
                    savedSizes = m_rightSplitter->sizes();
                    savedMax = m_logDock->maximumHeight();
                    const int total = savedSizes.value(0) + savedSizes.value(1);
                    m_logDock->setMaximumHeight(QWIDGETSIZE_MAX);
                    m_rightSplitter->setSizes({total / 3, total * 2 / 3});
                } else {
                    m_logDock->setMaximumHeight(savedMax);
                    m_rightSplitter->setSizes(savedSizes);
                }
                expandChatButton->setIcon(
                    style()->standardIcon(expanded ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
                expandChatButton->setToolTip(expanded ? tr("恢复面板高度") : tr("展开对话"));
                expandChatButton->setAccessibleName(expandChatButton->toolTip());
            });
    connect(m_logTerminalTabs, &QTabWidget::currentChanged, this,
            [this, logTabIndex, clearLogButton, expandChatButton](int index) {
                clearLogButton->setVisible(index == logTabIndex);
                expandChatButton->setVisible(index == m_agentChatTabIndex);
                if (index != m_agentChatTabIndex)
                    expandChatButton->setChecked(false);
            });
    connect(m_logDock, &QDockWidget::visibilityChanged, expandChatButton, [expandChatButton](bool visible) {
        if (!visible)
            expandChatButton->setChecked(false);
    });
    auto setAgentTabStatus = [this](const QString& tooltip, bool focus) {
        if (!m_logTerminalTabs || m_agentChatTabIndex < 0)
            return;
        m_logTerminalTabs->setTabToolTip(m_agentChatTabIndex, tooltip);
        if (focus) {
            m_logTerminalTabs->setCurrentIndex(m_agentChatTabIndex);
        }
    };
    connect(m_agentChatPanel, &AgentChatPanel::userMessageSent, this, [this, setAgentTabStatus](const QString& msg) {
        if (!m_agentChatPanel)
            return;
        setAgentTabStatus(tr("Agent 正在思考"), false);
        m_agentChatPanel->setThinking(true);
        AgentController::instance().sendUserMessage(msg);
        // 批3 复核三轮（P2-5）：校验期拒绝（未初始化/无客户端/忙碌）不会进入
        // Thinking——发送未生效，回填草稿供用户原样重试（面板为乐观 UI 已清空；
        // 已上屏的用户气泡保留，紧随的系统错误气泡说明未发送原因）
        if (AgentController::instance().state() != AgentController::AgentState::Thinking) {
            m_agentChatPanel->setThinking(false);
            m_agentChatPanel->restoreDraft(msg, {});
        }
    });
    connect(m_agentChatPanel, &AgentChatPanel::userMessageWithImagesSent, this,
            [this, setAgentTabStatus](const QString& msg, const QList<QPixmap>& images) {
                if (!m_agentChatPanel)
                    return;
                setAgentTabStatus(tr("Agent 正在思考"), false);
                m_agentChatPanel->setThinking(true);
                AgentController::instance().sendUserMessageWithImages(msg, images);
                // 批3 复核三轮（P2-5）：模型不支持图片/附件超限/未初始化等拒绝后
                // 回填草稿与附件（同上）
                if (AgentController::instance().state() != AgentController::AgentState::Thinking) {
                    m_agentChatPanel->setThinking(false);
                    m_agentChatPanel->restoreDraft(msg, images);
                }
            });
    connect(m_agentChatPanel, &AgentChatPanel::cancelRequested, this,
            []() { AgentController::instance().cancelCurrentRequest(); });
    connect(&AgentController::instance(), &AgentController::llmResponseReceived, this,
            [this, setAgentTabStatus](const QString& content, const QJsonArray& toolCalls) {
                if (!m_agentChatPanel)
                    return;
                Q_UNUSED(toolCalls);
                setAgentTabStatus(tr("Agent"), false);
                m_agentChatPanel->setThinking(false);
                m_agentChatPanel->clearToolPreview();
                // 流式：用终态文本对齐已有气泡；非流式/回退：新建气泡（设计 §5.5）
                if (m_agentChatPanel->isStreamActive()) {
                    m_agentChatPanel->finalizeStreamedMessage(content);
                } else {
                    m_agentChatPanel->addMessage(AgentMessageBubble::Sender::Agent, content);
                    // R1：非流式/回退响应也要展示 reasoning（折叠块，正文不含 reasoning）
                    m_agentChatPanel->setLastAgentReasoning(AgentController::instance().lastResponseReasoning());
                }
            });
    connect(&AgentController::instance(), &AgentController::llmErrorOccurred, this,
            [this, setAgentTabStatus](const QString& error) {
                if (!m_agentChatPanel)
                    return;
                setAgentTabStatus(tr("Agent 出错"), false);
                if (m_agentChatPanel->isStreamActive())
                    m_agentChatPanel->failStreamedMessage(tr("已中断"));
                m_agentChatPanel->setError(error);
                m_agentChatPanel->clearToolPreview();
                m_agentChatPanel->addMessage(AgentMessageBubble::Sender::System, QString("Error: %1").arg(error));
                AgentActionLogEntry entry;
                entry.timestamp = QDateTime::currentDateTime();
                entry.actor = "System";
                entry.action = "LLM Error";
                entry.params = error;
                entry.result = "error";
                entry.undoable = false;
                entry.errorCategory = AgentController::instance().lastLLMErrorCategory();
                m_agentActionLogWidget->addEntry(entry);
            });
    connect(&AgentController::instance(), &AgentController::toolsPendingConfirmation, this,
            [this, setAgentTabStatus](const QJsonArray& toolCalls) {
                if (!m_agentChatPanel)
                    return;
                setAgentTabStatus(tr("等待确认"), true);
                m_agentChatPanel->setThinking(false);
                QList<AgentToolPreviewCard::ToolItem> items;
                QStringList toolSummary;
                for (const QJsonValue& v : toolCalls) {
                    QJsonObject tc = v.toObject();
                    AgentToolPreviewCard::ToolItem item;
                    item.name = tc["name"].toString();
                    if (item.name.isEmpty()) {
                        QJsonObject func = tc["function"].toObject();
                        item.name = func["name"].toString();
                        QJsonValue argsVal = func["arguments"];
                        if (argsVal.isString()) {
                            item.params = QJsonDocument::fromJson(argsVal.toString().toUtf8()).object();
                        } else if (argsVal.isObject()) {
                            item.params = argsVal.toObject();
                        }
                    } else {
                        item.params = tc["arguments"].toObject();
                    }
                    if (item.name == QLatin1String("edit_active_annotation_masks")) {
                        const QJsonObject raw = item.params;
                        QJsonObject safe{{QStringLiteral("operation"), raw.value(QStringLiteral("operation"))},
                                         {QStringLiteral("objectIds"), raw.value(QStringLiteral("objectIds"))},
                                         {QStringLiteral("strokePointCount"),
                                          raw.value(QStringLiteral("strokePoints")).toArray().size()}};
                        if (raw.contains(QStringLiteral("radiusPx")))
                            safe.insert(QStringLiteral("radiusPx"), raw.value(QStringLiteral("radiusPx")));
                        item.params = safe;
                    } else if (item.name == QLatin1String("edit_active_annotation_categories")) {
                        const QJsonObject raw = item.params;
                        QJsonObject safe{{QStringLiteral("operation"), raw.value(QStringLiteral("operation"))}};
                        for (const QString& key :
                             {QStringLiteral("category"), QStringLiteral("objectIds"), QStringLiteral("label")}) {
                            if (raw.contains(key))
                                safe.insert(key, raw.value(key));
                        }
                        item.params = safe;
                    }
                    const bool dangerous = ToolSchema::instance().findTool(item.name).dangerous;
                    toolSummary.append(QString("%1%2").arg(item.name, dangerous ? tr(" [高风险]") : QString()));
                    items.append(item);
                }
                if (!toolSummary.isEmpty()) {
                    m_agentChatPanel->addMessage(AgentMessageBubble::Sender::Tool,
                                                 tr("待确认工具：%1").arg(toolSummary.join(", ")));
                }
                // 阶段1 §4（审核整改 R2）：确认卡展示工程/摘要/副作用,并绑定 confirmationId
                AgentToolPreviewCard::ConfirmationContext context;
                const QJsonObject payload = AgentController::instance().pendingConfirmationPayload();
                context.confirmationId = payload.value(QStringLiteral("confirmationId")).toString();
                const QJsonObject ctx = payload.value(QStringLiteral("context")).toObject();
                context.summary = ctx.value(QStringLiteral("summary")).toString();
                context.reversibilityLabel = ctx.value(QStringLiteral("reversibilityLabel")).toString();
                context.projectName = ctx.value(QStringLiteral("projectName")).toString();
                context.projectPath = ctx.value(QStringLiteral("projectPath")).toString();
                for (const QJsonValue& sv : ctx.value(QStringLiteral("sideEffects")).toArray())
                    context.sideEffects.append(sv.toString());
                m_agentChatPanel->showToolPreview(items, context);
            });
    // 阶段1 审核整改 R2：批准/拒绝携带展示时绑定的 confirmationId；
    // 错配（旧批次迟到事件）返回 false——零副作用,给用户可见反馈
    connect(m_agentChatPanel, &AgentChatPanel::toolPreviewConfirmed, this, [this](const QString& confirmationId) {
        if (!m_agentChatPanel)
            return;
        if (!AgentController::instance().confirmPendingTools(confirmationId)) {
            m_agentChatPanel->addMessage(AgentMessageBubble::Sender::Tool,
                                         tr("该确认已失效或已被新批次取代，未执行任何操作"));
        }
    });
    connect(m_agentChatPanel, &AgentChatPanel::toolPreviewCancelled, this, [this](const QString& confirmationId) {
        if (!m_agentChatPanel)
            return;
        if (!AgentController::instance().rejectPendingTools(confirmationId)) {
            m_agentChatPanel->addMessage(AgentMessageBubble::Sender::Tool,
                                         tr("该取消请求对应已失效的确认批次，已忽略"));
        }
    });
    // 停止按钮 → 中止当前轮（设计 §5.6）
    connect(m_agentChatPanel, &AgentChatPanel::stopRequested, &AgentController::instance(),
            &AgentController::abortCurrentTurn);
    connect(&AgentController::instance(), &AgentController::agentTurnAborted, this, [this, setAgentTabStatus]() {
        setAgentTabStatus(tr("已停止"), false);
        m_agentChatPanel->setStopped();
    });
    connect(&AgentController::instance(), &AgentController::agentStateChanged, this,
            [this, setAgentTabStatus](AgentController::AgentState state) {
                switch (state) {
                case AgentController::AgentState::Thinking:
                    setAgentTabStatus(tr("Agent 正在思考"), false);
                    m_agentChatPanel->setThinking(true);
                    break;
                case AgentController::AgentState::Confirming:
                    setAgentTabStatus(tr("等待确认"), true);
                    break;
                case AgentController::AgentState::Executing:
                    setAgentTabStatus(tr("执行中（可停止）"), false);
                    m_agentChatPanel->setExecuting(true);
                    break;
                case AgentController::AgentState::Idle:
                    m_agentChatPanel->setExecuting(false);
                    break;
                }
            });
    // §3.7：token 用量显示
    connect(&AgentController::instance(), &AgentController::llmUsageReported, this,
            [this](int promptTokens, int completionTokens) {
                if (!m_agentTokenLabel)
                    return;
                m_agentTokenLabel->setText(tr("tokens: %1+%2").arg(promptTokens).arg(completionTokens));
                m_agentTokenLabel->show();
            });

    // ===== Tab 4: Agent 操作日志 =====
    m_agentActionLogWidget = new AgentActionLogWidget(m_logTerminalTabs);
    m_agentActionLogWidget->setObjectName(QStringLiteral("AgentLogTab"));
    const int agentLogTabIndex = m_logTerminalTabs->addTab(m_agentActionLogWidget, tr("Agent 日志"));
    m_logTerminalTabs->setTabToolTip(agentLogTabIndex, tr("Agent 操作日志；双击打开审计日志文件"));
    connect(m_logTerminalTabs->tabBar(), &QTabBar::tabBarDoubleClicked, this, [this, agentLogTabIndex](int index) {
        if (index != agentLogTabIndex)
            return;

        const QString auditPath = AgentAuditLog::instance().currentFilePath();
        const QString targetPath = QFileInfo::exists(auditPath) ? auditPath : PathUtils::logPath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(targetPath));
    });
    connect(&AgentController::instance(), &AgentController::actionLogEntryAdded, m_agentActionLogWidget,
            &AgentActionLogWidget::addEntry);
    connect(m_agentActionLogWidget, &AgentActionLogWidget::undoRequested, this,
            [](int) { AgentController::instance().undoLastAgentAction(); });
    connect(&AgentController::instance(), &AgentController::undoRejected, this,
            [this](const QString& reason) { statusBar()->showMessage(reason, 5000); });
    connect(&AgentController::instance(), &AgentController::agentLoopIterating, this, [this, setAgentTabStatus]() {
        if (!m_agentChatPanel)
            return;
        setAgentTabStatus(tr("Agent 正在思考"), false);
        m_agentChatPanel->setThinking(true);
    });

    // ===== 流式接线（设计 §5/§4.4）=====
    connect(&AgentController::instance(), &AgentController::agentStreamStarted, this,
            [this]() { m_agentChatPanel->beginAgentStream(); });
    connect(&AgentController::instance(), &AgentController::agentStreamContentDelta, this,
            [this](const QString& text) { m_agentChatPanel->appendStreamContent(text); });
    connect(&AgentController::instance(), &AgentController::agentStreamReasoningDelta, this,
            [this](const QString& text) { m_agentChatPanel->appendStreamReasoning(text); });
    connect(&AgentController::instance(), &AgentController::agentStreamAborted, this,
            [this]() { m_agentChatPanel->abortStreamedMessage(tr("已停止")); });
    connect(&AgentController::instance(), &AgentController::agentStreamFallback, this,
            [this, setAgentTabStatus](const QString& reason) {
                setAgentTabStatus(tr("流式不可用，已回退整包响应"), false);
                m_agentChatPanel->abortStreamedMessage(tr("已回退"));
                m_agentChatPanel->addMessage(AgentMessageBubble::Sender::System,
                                             tr("流式不可用（%1），已回退为整包响应。").arg(reason));
            });
    connect(&AgentController::instance(), &AgentController::agentStreamTruncated, this,
            [setAgentTabStatus]() { setAgentTabStatus(tr("输出达到 max_tokens 上限"), false); });

    m_logDock->setWidget(m_logTerminalTabs);
    m_logDock->setMinimumHeight(220);

    m_rightSplitter->addWidget(m_logDock);
    m_rightSplitter->setStretchFactor(0, 7);
    m_rightSplitter->setStretchFactor(1, 3);

    // 底部面板使用 LayoutMetrics 的范围
    m_logDock->setMinimumHeight(metrics.bottomPanelMinHeight);
    m_logDock->setMaximumHeight(metrics.bottomPanelMaxHeight);
    m_rightSplitter->setSizes(QList<int>() << 620 << metrics.bottomPanelHeight);

    // 将右侧 Splitter 添加到主 Splitter
    m_mainSplitter->addWidget(m_rightSplitter);
    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 4);

    // 设置中央 widget
    mainContentLayout->addWidget(m_mainSplitter);
    setCentralWidget(mainContentWidget);

    // P2: 同步菜单勾选状态，自适应隐藏不设 userClosed
    // 高: setChecked 会触发 toggled → onToggleToolPanel → 设 userClosed，
    //     用 blockSignals 阻止 toggled 回调
    connect(m_toolBoxDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (m_viewToolPanelAction && !m_focusMode) {
            const QSignalBlocker blocker(m_viewToolPanelAction);
            m_viewToolPanelAction->setChecked(visible);
        }
        if (!visible && !m_toolPanelSuppressSignal && !m_focusMode)
            m_toolPanelUserClosed = true;
    });
    connect(m_logDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (m_viewBottomPanelAction && !m_focusMode)
            m_viewBottomPanelAction->setChecked(visible);
    });

    // 运行控制按钮已移至顶部工具栏，此处无需连接流程面板按钮信号

    // Esc 快捷键退出聚焦模式
    QShortcut* escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escShortcut, &QShortcut::activated, this, [this]() {
        if (m_focusMode) {
            toggleFocusMode();
        }
    });
}

void MainWindow::addToolBoxItem(QTreeWidgetItem* parent, const QString& displayName, const QString& pluginName) {
    const QString text = cleanToolDisplayName(displayName);
    const PluginInfo info = PluginManager::instance().pluginInfo(pluginName);
    QTreeWidgetItem* item = new QTreeWidgetItem(parent, QStringList(text));
    item->setIcon(0, ModuleIconProvider::instance().iconFor(pluginName, info.category));
    item->setToolTip(0, pluginName);
    item->setData(0, Qt::UserRole, "plugin");
    item->setData(0, Qt::UserRole + 1, pluginName);
    m_toolDisplayNames.insert(pluginName, text);
}

QString MainWindow::toolDisplayName(const QString& pluginName, const QString& fallback) const {
    const QString cached = m_toolDisplayNames.value(pluginName).trimmed();
    if (!cached.isEmpty()) {
        return cached;
    }

    const QString managerName = PluginManager::instance().moduleDisplayName(pluginName).trimmed();
    if (!managerName.isEmpty()) {
        return managerName;
    }
    return fallback.isEmpty() ? pluginName : fallback;
}

void MainWindow::updateToolBoxPluginItem(const QString& pluginName) {
    if (!m_toolBoxTree) {
        return;
    }

    const QString displayName = PluginManager::instance().moduleDisplayName(pluginName);
    const PluginInfo info = PluginManager::instance().pluginInfo(pluginName);
    const QIcon icon = ModuleIconProvider::instance().iconFor(pluginName, info.category);

    for (QTreeWidgetItemIterator it(m_toolBoxTree); *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->data(0, Qt::UserRole).toString() == "plugin" &&
            item->data(0, Qt::UserRole + 1).toString() == pluginName) {
            item->setText(0, displayName);
            item->setIcon(0, icon);
            m_toolDisplayNames.insert(pluginName, displayName);
            return;
        }
    }

    // P1-1: 插件不在工具箱中时，按类别自动添加
    static const QMap<QString, QString> categoryKeywords = {
        {"image_processing", QStringLiteral("01 - 图像处理")},
        {"detection", QStringLiteral("02 - 检测识别")},
        {"geometry", QStringLiteral("03 - 几何测量")},
        {"calibration", QStringLiteral("05 - 坐标标定")},
        {"logic", QStringLiteral("07 - 逻辑工具")},
        {"system", QStringLiteral("08 - 系统工具")},
        {"variable", QStringLiteral("09 - 变量工具")},
        {"communication", QStringLiteral("14 - 通信")},
        {"file_comm", QStringLiteral("10 - 文件通讯")},
        {"hymson3d", QStringLiteral("11 - 3D 工具")},
        {"camera", QStringLiteral("00 - 常用工具")},
        {"string", QStringLiteral("13 - 字符串处理")},
    };
    const QString category = info.category.toLower();
    for (auto it = categoryKeywords.constBegin(); it != categoryKeywords.constEnd(); ++it) {
        if (category.contains(it.key())) {
            for (int i = 0; i < m_toolBoxTree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* cat = m_toolBoxTree->topLevelItem(i);
                // 用原始带前缀文本（UserRole+3）做匹配，显示文本已去除前缀
                QString origText = cat->data(0, Qt::UserRole + 3).toString();
                if (origText.isEmpty()) {
                    origText = cat->text(0);
                }
                if (origText.startsWith(it.value().left(2))) {
                    addToolBoxItem(cat, displayName, pluginName);
                    return;
                }
            }
        }
    }
}

void MainWindow::onToggleToolPanel(bool checked) {
    m_toolPanelUserClosed = !checked;
    if (m_toolBoxDock) {
        m_toolBoxDock->setVisible(checked);
    }
}

void MainWindow::onToggleProcessPanel(bool checked) {
    if (m_processTabWidget) {
        m_processTabWidget->setVisible(checked);
    }
}

void MainWindow::onToggleBottomPanel(bool checked) {
    if (m_logDock) {
        m_logDock->setVisible(checked);
    }
}

void MainWindow::updateProjectContext(Project* project) {
    if (!m_projectLabel) {
        return;
    }

    if (!project) {
        m_projectLabel->setText(tr("工程  /  未打开"));
        m_projectLabel->setToolTip(tr("当前未打开工程"));
        if (QWidget* breadcrumb = m_projectLabel->parentWidget()) {
            breadcrumb->adjustSize();
            breadcrumb->updateGeometry();
        }
        return;
    }

    QString name = project->name().trimmed();
    if (name.isEmpty() && !project->filePath().isEmpty()) {
        name = QFileInfo(project->filePath()).completeBaseName();
    }
    if (name.isEmpty()) {
        name = tr("未命名工程");
    }

    const QString breadcrumbText = tr("工程  /  %1").arg(name);
    m_projectLabel->setText(m_projectLabel->fontMetrics().elidedText(breadcrumbText, Qt::ElideMiddle, 190));
    m_projectLabel->setToolTip(project->filePath().isEmpty() ? tr("尚未保存") : project->filePath());
    if (QWidget* breadcrumb = m_projectLabel->parentWidget()) {
        breadcrumb->adjustSize();
        breadcrumb->updateGeometry();
    }
}

void MainWindow::onProjectOpened(Project* project) {
    if (!project)
        return;

    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    setUiRunningState(false, false);
    m_isStepMode = false;

    // 后台执行（设计 §7.3）：工程切换清空合并器待处理帧、最近帧与叠加，避免旧工程残留。
    // 同时清空 RunEngine 的最新帧槽位：迟到通知按 (jobId,runId) 取帧时必须取不到旧工程帧。
    if (m_imageCoalescer) {
        m_imageCoalescer->clearPending();
        m_imageCoalescer->clearExpectedIdentity();
    }
    RunEngine::instance().clearLatestDisplayFrames();
    m_hasDisplayFrame = false;
    m_latestDisplayFrame = DisplayFrame();
    if (m_displayManager) {
        clearMeasurementOverlays();
    }

    // 清空参数撤销栈和脏标记
    if (m_paramUndoStack) {
        m_paramUndoStack->clear();
    }
    m_dirtyModuleIds.clear();
    m_activeMeasurementInputId.clear();
    m_pendingMeasurementInputId.clear();
    m_pendingMeasurementViewportId.clear();
    m_pendingMeasurementViewports.clear();
    clearDataSourceViewports();

    // 清空现有流程树
    // t28 F1：流程树 clear() 会删除 QTreeWidgetItem；先清空缓存指针，避免迟到信号使用悬空指针。
    m_currentExecutingItem = nullptr;
    m_processTreeController->clear();
    if (m_flowCanvas) {
        m_flowCanvas->loadFromProject(nullptr);
    }

    // 加载项目中已有的模块
    for (const ModuleInstance& inst : project->modules()) {
        m_processTreeController->addModule(inst);
    }
    if (m_flowCanvas) {
        m_flowCanvas->loadFromProject(project);
        for (const ModuleInstance& inst : project->modules()) {
            if (inst.name.trimmed().isEmpty()) {
                if (FlowNodeItem* node = m_flowCanvas->nodeItem(inst.id))
                    node->setName(toolDisplayName(inst.moduleId, inst.name));
            }
        }
    }

    // 刷新数据源面板
    if (m_dataSourcePanel) {
        m_dataSourcePanel->refreshFromProject(project);
    }
    updateProjectContext(project);

    // 连接 Project 信号
    disconnect(project, nullptr, this, nullptr);
    disconnect(project, nullptr, m_processTreeController, nullptr);
    connect(project, &Project::moduleAdded, m_processTreeController, &ProcessTreeController::onModuleAddedFromProject);
    connect(project, &Project::moduleRemoved, m_processTreeController,
            &ProcessTreeController::onModuleRemovedFromProject);
    connect(project, &Project::moduleMoved, m_processTreeController, &ProcessTreeController::onModuleMovedFromProject);
    connect(project, &Project::modulePositionChanged, this, [this, project](const QString& instanceId, int x, int y) {
        if (ProjectManager::instance().currentProject() != project || !m_flowCanvas)
            return;
        if (FlowNodeItem* node = m_flowCanvas->nodeItem(instanceId))
            node->setPos(x, y);
    });
    connect(project, &Project::moduleUpdated, this, [this, project](const ModuleInstance& instance) {
        if (ProjectManager::instance().currentProject() != project) {
            return;
        }
        const QString displayName =
            instance.name.trimmed().isEmpty() ? toolDisplayName(instance.moduleId, instance.name) : instance.name;
        if (QTreeWidgetItem* item = m_processTreeController->instanceItem(instance.id))
            item->setText(0, displayName);
        if (m_flowCanvas) {
            if (FlowNodeItem* node = m_flowCanvas->nodeItem(instance.id))
                node->setName(displayName);
        }
        if (IModule* module = m_flowModules.value(instance.id, nullptr)) {
            module->setParams(instance.params);
        }
        m_modulesNeedSync = true;
        markModuleDirty(instance.id);
        if (m_inspectorPanel && m_inspectorPanel->currentInstanceId() == instance.id) {
            m_inspectorPanel->setEnabledState(instance.enabled);
            m_inspectorPanel->setBreakpointState(instance.breakpoint);
            m_inspectorPanel->setNoteState(instance.note);
            QPointer<ModuleInspectorPanel> inspector = m_inspectorPanel;
            const QString instanceId = instance.id;
            QMetaObject::invokeMethod(
                inspector.data(),
                [inspector, instanceId]() {
                    if (inspector && inspector->currentInstanceId() == instanceId) {
                        inspector->refreshFromModule();
                    }
                },
                Qt::QueuedConnection);
        }
    });
    connect(project, &Project::nameChanged, this, [this, project]() {
        if (ProjectManager::instance().currentProject() == project) {
            updateProjectContext(project);
        }
    });
    connect(project, &Project::connectionAdded, this, [this, project](const ModuleConnection& conn) {
        if (m_flowCanvas && m_flowCanvas->nodeItem(conn.fromModuleId) && m_flowCanvas->nodeItem(conn.toModuleId)) {
            const QString fromPort = conn.fromPort.isEmpty() ? QStringLiteral("image") : conn.fromPort;
            const QString toPort = conn.toPort.isEmpty() ? QStringLiteral("image") : conn.toPort;
            const QString edgeType = m_flowCanvas->resolveConnectionEdgeType(conn);
            // P0-fix: 防止 Project→Canvas→Project 回写循环
            m_flowCanvas->m_syncingFromProject = true;
            m_flowCanvas->addConnection(conn.fromModuleId, fromPort, conn.toModuleId, toPort, edgeType);
            m_flowCanvas->m_syncingFromProject = false;
        }
        m_modulesNeedSync = true;
        for (const ModuleInstance& module : project->modules()) {
            markModuleDirty(module.id);
        }
    });
    // P1-fix: 旧信号——按节点对删除（兼容旧代码路径）
    connect(project, &Project::connectionRemoved, this, [this, project](const QString& fromId, const QString& toId) {
        if (m_flowCanvas) {
            m_flowCanvas->m_syncingFromProject = true;
            for (int i = m_flowCanvas->m_connections.size() - 1; i >= 0; --i) {
                FlowConnectionItem* conn = m_flowCanvas->m_connections[i];
                if (conn && conn->fromNodeId() == fromId && conn->toNodeId() == toId) {
                    m_flowCanvas->removeConnection(conn->fromNodeId(), conn->fromPortId(), conn->toNodeId(),
                                                   conn->toPortId());
                }
            }
            m_flowCanvas->m_syncingFromProject = false;
        }
        m_modulesNeedSync = true;
        for (const ModuleInstance& module : project->modules()) {
            markModuleDirty(module.id);
        }
    });
    // P1-fix: 新信号——按完整四元组精确删除一条连接
    connect(
        project, &Project::connectionRemovedWithPorts, this,
        [this, project](const QString& fromId, const QString& fromPort, const QString& toId, const QString& toPort) {
            if (m_flowCanvas) {
                m_flowCanvas->m_syncingFromProject = true;
                m_flowCanvas->removeConnection(fromId, fromPort, toId, toPort);
                m_flowCanvas->m_syncingFromProject = false;
            }
            m_modulesNeedSync = true;
            for (const ModuleInstance& module : project->modules()) {
                markModuleDirty(module.id);
            }
        });
    connect(project, &Project::dataSourceAdded, this, &MainWindow::onDataSourceAdded);
    connect(project, &Project::dataSourceRemoved, this, &MainWindow::onDataSourceRemoved);
}

void MainWindow::onProjectClosed() {
    // RunEngine borrows flow-module pointers; detach them before the controller deletes the modules.
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    setUiRunningState(false, false);
    m_isStepMode = false;

    // 后台执行（设计 §7.3）：工程切换清空合并器待处理帧、最近帧与叠加，迟到帧不得污染新工程。
    // 同时清空 RunEngine 的最新帧槽位（旧 (jobId,runId) 的迟到通知必须取不到可渲染帧）。
    if (m_imageCoalescer) {
        m_imageCoalescer->clearPending();
        m_imageCoalescer->clearExpectedIdentity();
    }
    RunEngine::instance().clearLatestDisplayFrames();
    m_hasDisplayFrame = false;
    m_latestDisplayFrame = DisplayFrame();
    if (m_displayManager) {
        clearMeasurementOverlays();
    }

    // P0: 先清空检查器，防止悬空模块指针
    m_selectedModuleId.clear();
    m_activeMeasurementInputId.clear();
    m_pendingMeasurementInputId.clear();
    m_pendingMeasurementViewportId.clear();
    m_pendingMeasurementViewports.clear();
    if (m_inspectorPanel) {
        m_inspectorPanel->clear();
    }
    // 清空参数撤销栈和脏标记
    if (m_paramUndoStack) {
        m_paramUndoStack->clear();
    }
    m_dirtyModuleIds.clear();
    clearDataSourceViewports();

    // t28 F1：clear() 删除流程树 item；先清空执行项缓存，避免迟到 moduleFinished 写悬空指针。
    m_currentExecutingItem = nullptr;
    m_processTreeController->clear();
    if (m_flowCanvas) {
        m_flowCanvas->loadFromProject(nullptr);
    }
    if (m_dataSourcePanel) {
        m_dataSourcePanel->refreshFromProject(nullptr);
    }
    updateProjectContext(nullptr);
}

void MainWindow::onViewportCreated(const QString& /*viewportId*/, ViewportWidget* viewport) {
    if (!viewport)
        return;
    const QString viewportId = viewport->viewportId();
    connect(viewport, &ViewportWidget::contentGenerationChanged, this, [this, viewportId](quint64 generation) {
        if (m_pendingMeasurementInputId.isEmpty() ||
            (!m_pendingMeasurementViewportId.isEmpty() && m_pendingMeasurementViewportId != viewportId))
            return;
        const auto snapshot = m_pendingMeasurementViewports.constFind(viewportId);
        if (snapshot == m_pendingMeasurementViewports.cend() || snapshot->contentGeneration == generation)
            return;

        Logger::instance().warning(tr("等待测量输入期间视口内容已改变，已取消本轮运行"), "Measurement");
        onStop();
    });
    connect(viewport, &ViewportWidget::point2DSourceClicked, this,
            [this, viewportId](const QPointF& point, const QPointF& sourcePoint, const QString& sourceImageId,
                               bool sourceCoordinateAvailable) {
                if (m_agentQuickMeasurement.status == QLatin1String("awaiting_user_input")) {
                    if (viewportId != m_agentQuickMeasurement.viewportId ||
                        m_agentQuickMeasurement.dimension != QLatin1String("2d"))
                        return;
                    QString reason;
                    if (!agentQuickMeasurementTargetStillValid(&reason)) {
                        cancelAgentQuickMeasurement(QStringLiteral("expired"), reason);
                        return;
                    }
                    QJsonObject picked{{QStringLiteral("imageLocal"), QJsonArray{point.x(), point.y()}},
                                       {QStringLiteral("coordinateFrame"), QStringLiteral("ImageLocalPixel")},
                                       {QStringLiteral("sourceCoordinateAvailable"), sourceCoordinateAvailable}};
                    if (sourceCoordinateAvailable) {
                        picked.insert(QStringLiteral("sourceImage"), sourceImageId);
                        if (ViewportWidget* viewport = m_displayManager->viewport(viewportId)) {
                            const QSize sourceSize = viewport->imageCoordinateProvenance().sourceSize;
                            picked.insert(QStringLiteral("sourceImageSize"),
                                          QJsonArray{sourceSize.width(), sourceSize.height()});
                        }
                        picked.insert(QStringLiteral("sourceImagePixel"), QJsonArray{sourcePoint.x(), sourcePoint.y()});
                    }
                    m_agentQuickMeasurement.pickedCoordinates.append(picked);
                    ++m_agentQuickMeasurement.completedSteps;
                    onPoint2DPicked(point);
                    if (m_agentQuickMeasurement.completedSteps >= m_agentQuickMeasurement.steps.size())
                        completeAgentQuickMeasurement();
                    return;
                }
                if (!bindMeasurementPickViewport(viewportId))
                    return;
                onPoint2DPicked(point);
            });
    connect(viewport, &ViewportWidget::point3DWorldClicked, this, [this, viewportId](double x, double y, double z) {
        if (m_agentQuickMeasurement.status != QLatin1String("awaiting_user_input")) {
            if (!bindMeasurementPickViewport(viewportId))
                return;
            handlePoint3DPicked(x, y, z);
            return;
        }
        if (viewportId != m_agentQuickMeasurement.viewportId ||
            m_agentQuickMeasurement.dimension != QLatin1String("3d"))
            return;
        QString reason;
        if (!agentQuickMeasurementTargetStillValid(&reason)) {
            cancelAgentQuickMeasurement(QStringLiteral("expired"), reason);
            return;
        }
        m_agentQuickMeasurement.pickedCoordinates.append(
            QJsonObject{{QStringLiteral("coordinateFrame"), QStringLiteral("World3D")},
                        {QStringLiteral("pointCloudWorld"), QJsonArray{x, y, z}}});
        ++m_agentQuickMeasurement.completedSteps;
        handlePoint3DPicked(x, y, z);
        if (m_agentQuickMeasurement.completedSteps >= m_agentQuickMeasurement.steps.size())
            completeAgentQuickMeasurement();
    });
    // 关闭按钮：清空视口内容（单视口时不销毁）
    disconnect(viewport, &ViewportWidget::viewportClosed, this, nullptr);
    connect(viewport, &ViewportWidget::viewportClosed, this, [this](const QString& id) { closeViewportById(id); });
    // 高: 视口及其子控件也需要事件过滤器，双击进入焦点模式
    viewport->installEventFilter(this);
    if (viewport->imageWidget())
        viewport->imageWidget()->installEventFilter(this);
    // 高: 延迟创建的 3D 控件也需要安装
    connect(viewport, &ViewportWidget::contentWidgetCreated, this, [this](QWidget* w) { w->installEventFilter(this); });
}

QJsonObject MainWindow::quickMeasurementConfirmationSnapshot(const QJsonObject& params) const {
    const QString viewportId = params.value(QStringLiteral("viewportId")).toString();
    const QString dimension = params.value(QStringLiteral("dimension")).toString();
    const QString measurementType = params.value(QStringLiteral("measurementType")).toString();
    ViewportWidget* viewport = m_displayManager ? m_displayManager->viewport(viewportId) : nullptr;
    const bool is2D = dimension == QLatin1String("2d");
    const bool is3D = dimension == QLatin1String("3d");
    const bool typeValid =
        measurementType == QLatin1String("distance_2_points") || measurementType == QLatin1String("point_to_line") ||
        measurementType == QLatin1String("segment_gap") || measurementType == QLatin1String("angle_3_points") ||
        measurementType == QLatin1String("circle_3_points") || measurementType == QLatin1String("rectangle_area");
    const bool dimensionSupported = is2D || (is3D && measurementType != QLatin1String("rectangle_area"));
    const bool modeMatches =
        viewport &&
        ((is2D && viewport->displayMode() == ViewportWidget::DisplayMode::Auto2D && viewport->imageWidget() &&
          viewport->imageWidget()->hasImage()) ||
         (is3D && viewport->displayMode() == ViewportWidget::DisplayMode::Auto3D && viewport->hasCachedPointCloud()));
    const Project* project = ProjectManager::instance().currentProject();
    return {{QStringLiteral("viewportId"), viewportId},
            {QStringLiteral("viewportIdentity"),
             viewport ? QString::number(reinterpret_cast<quintptr>(viewport)) : QString()},
            {QStringLiteral("viewportContentGeneration"),
             viewport ? QString::number(viewport->contentGeneration()) : QStringLiteral("missing")},
            {QStringLiteral("projectRuntimeIdentity"),
             project ? QString::number(project->runtimeIdentity()) : QStringLiteral("0")},
            {QStringLiteral("dimension"), dimension},
            {QStringLiteral("measurementType"), measurementType},
            {QStringLiteral("targetAvailable"),
             viewport && typeValid && dimensionSupported && modeMatches && !m_quickMeasureActive &&
                 m_agentQuickMeasurement.status != QLatin1String("awaiting_user_input")}};
}

bool MainWindow::agentQuickMeasurementTargetStillValid(QString* reason) const {
    auto invalidate = [reason](const QString& message) {
        if (reason)
            *reason = message;
        return false;
    };
    const AgentQuickMeasurementRequest& request = m_agentQuickMeasurement;
    if (request.status != QLatin1String("awaiting_user_input"))
        return invalidate(QStringLiteral("Measurement request is no longer waiting"));
    ViewportWidget* viewport = m_displayManager ? m_displayManager->viewport(request.viewportId) : nullptr;
    if (!viewport)
        return invalidate(QStringLiteral("Target viewport was closed"));
    if (reinterpret_cast<quintptr>(viewport) != request.viewportIdentity)
        return invalidate(QStringLiteral("Target viewport identity changed"));
    if (viewport->contentGeneration() != request.viewportContentGeneration)
        return invalidate(QStringLiteral("Target viewport content or display mode changed"));
    const Project* project = ProjectManager::instance().currentProject();
    const quint64 currentProjectIdentity = project ? project->runtimeIdentity() : 0;
    if (currentProjectIdentity != request.projectRuntimeIdentity)
        return invalidate(QStringLiteral("The active project changed"));
    const bool compatible = request.dimension == QLatin1String("2d")
                                ? viewport->displayMode() == ViewportWidget::DisplayMode::Auto2D &&
                                      viewport->imageWidget() && viewport->imageWidget()->hasImage()
                                : request.dimension == QLatin1String("3d") &&
                                      viewport->displayMode() == ViewportWidget::DisplayMode::Auto3D &&
                                      viewport->hasCachedPointCloud();
    if (!compatible)
        return invalidate(QStringLiteral("Target viewport no longer has compatible measurement content"));
    return true;
}

QJsonObject MainWindow::currentAgentQuickMeasurementStatus() const {
    const AgentQuickMeasurementRequest& request = m_agentQuickMeasurement;
    if (request.requestId.isEmpty())
        return {{QStringLiteral("error"), QStringLiteral("No quick measurement request exists")},
                {QStringLiteral("errorCode"), QStringLiteral("QUICK_MEASURE_REQUEST_NOT_FOUND")},
                {QStringLiteral("retryable"), false}};
    if (request.status == QLatin1String("awaiting_user_input")) {
        QString reason;
        if (!agentQuickMeasurementTargetStillValid(&reason))
            const_cast<MainWindow*>(this)->cancelAgentQuickMeasurement(QStringLiteral("expired"), reason);
    }
    const AgentQuickMeasurementRequest& current = m_agentQuickMeasurement;
    const int remaining = qMax(0, current.steps.size() - current.completedSteps);
    QJsonArray steps;
    for (const QString& step : current.steps)
        steps.append(step);
    QJsonObject response{{QStringLiteral("requestId"), current.requestId},
                         {QStringLiteral("status"), current.status},
                         {QStringLiteral("viewportId"), current.viewportId},
                         {QStringLiteral("dimension"), current.dimension},
                         {QStringLiteral("measurementType"), current.measurementType},
                         {QStringLiteral("coordinateSystem"), current.coordinateSystem},
                         {QStringLiteral("pickedCoordinates"), current.pickedCoordinates},
                         {QStringLiteral("steps"), steps},
                         {QStringLiteral("completedSteps"), current.completedSteps},
                         {QStringLiteral("remainingSteps"), remaining}};
    if (!current.reason.isEmpty())
        response.insert(QStringLiteral("reason"), current.reason);
    if (current.status == QLatin1String("awaiting_user_input") && remaining > 0)
        response.insert(QStringLiteral("nextStep"), current.steps.at(current.completedSteps));
    return response;
}

void MainWindow::cancelAgentQuickMeasurement(const QString& status, const QString& reason) {
    if (m_agentQuickMeasurement.status != QLatin1String("awaiting_user_input"))
        return;
    QObject::disconnect(m_agentQuickMeasureRoiConnection);
    m_agentQuickMeasureRoiConnection = {};
    if (m_displayManager) {
        if (ViewportWidget* viewport = m_displayManager->viewport(m_agentQuickMeasurement.viewportId)) {
            viewport->setPickMode(false);
            viewport->unsetCursor();
            if (HImageWidget* image = viewport->imageWidget()) {
                image->cancelPendingPointerInput();
                if (m_agentQuickMeasurement.measurementTypeIndex == 5) {
                    image->setRoiMode(RoiType::None);
                    image->clearRois();
                }
                // A request that has not accepted a pick has not created a measurement overlay.
                // Preserve any result the user already had in this viewport.
                if (m_agentQuickMeasurement.completedSteps > 0)
                    image->clearMeasurementOverlay();
            }
            if (m_agentQuickMeasurement.completedSteps > 0) {
                if (Viewport3DContent* content = viewport->viewport3D()) {
                    content->clearMeasurementOverlay();
                    content->clearMeasurementInfoPanel();
                }
            }
        }
    }
    m_quickMeasureActive = false;
    m_quickMeasurePoints.clear();
    m_quickMeasurePoints3D.clear();
    if (m_quickMeasureAction)
        m_quickMeasureAction->setChecked(false);
    m_agentQuickMeasurement.status = status;
    m_agentQuickMeasurement.reason = reason;
}

void MainWindow::completeAgentQuickMeasurement() {
    if (m_agentQuickMeasurement.status != QLatin1String("awaiting_user_input"))
        return;
    QObject::disconnect(m_agentQuickMeasureRoiConnection);
    m_agentQuickMeasureRoiConnection = {};
    if (ViewportWidget* viewport = m_displayManager->viewport(m_agentQuickMeasurement.viewportId)) {
        viewport->setPickMode(false);
        viewport->unsetCursor();
        if (HImageWidget* image = viewport->imageWidget())
            image->setRoiMode(RoiType::None);
    }
    m_quickMeasureActive = false;
    if (m_quickMeasureAction)
        m_quickMeasureAction->setChecked(false);
    m_agentQuickMeasurement.status = QStringLiteral("completed");
    m_agentQuickMeasurement.reason.clear();
}

QJsonObject MainWindow::activeAnnotationMaskConfirmationSnapshot(const QJsonObject& params) const {
    if (!m_samAnnotatorDialog || !m_samAnnotatorDialog->isVisible())
        return {{QStringLiteral("available"), false}, {QStringLiteral("params"), params}};
    const QImage image = m_samAnnotatorDialog->imageSnapshot();
    const QJsonObject sessionJson = m_samAnnotatorDialog->session().toJson();
    const QByteArray sessionBytes = QJsonDocument(sessionJson).toJson(QJsonDocument::Compact);
    const QString sessionDigest =
        QString::fromLatin1(QCryptographicHash::hash(sessionBytes, QCryptographicHash::Sha256).toHex());
    return {{QStringLiteral("available"), !image.isNull()},
            {QStringLiteral("ownerIdentity"), QString::number(reinterpret_cast<quintptr>(this))},
            {QStringLiteral("dialogIdentity"), QString::number(m_samAnnotatorDialog->instanceId())},
            {QStringLiteral("imageWidth"), image.width()},
            {QStringLiteral("imageHeight"), image.height()},
            {QStringLiteral("imageDigest"), AnnotationSession::computeImageDigest(image)},
            {QStringLiteral("sessionDigest"), sessionDigest},
            {QStringLiteral("params"), params}};
}

QJsonObject MainWindow::activeAnnotationCategoryConfirmationSnapshot(const QJsonObject& params) const {
    if (!m_samAnnotatorDialog || !m_samAnnotatorDialog->isVisible())
        return {{QStringLiteral("available"), false}, {QStringLiteral("params"), params}};
    const QByteArray sessionBytes =
        QJsonDocument(m_samAnnotatorDialog->session().toJson()).toJson(QJsonDocument::Compact);
    const QByteArray categoriesBytes =
        QJsonDocument(QJsonArray::fromStringList(m_samAnnotatorDialog->categories())).toJson(QJsonDocument::Compact);
    return {{QStringLiteral("available"), true},
            {QStringLiteral("ownerIdentity"), QString::number(reinterpret_cast<quintptr>(this))},
            {QStringLiteral("dialogIdentity"), QString::number(m_samAnnotatorDialog->instanceId())},
            {QStringLiteral("sessionDigest"),
             QString::fromLatin1(QCryptographicHash::hash(sessionBytes, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("categoriesDigest"),
             QString::fromLatin1(QCryptographicHash::hash(categoriesBytes, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("params"), params}};
}

QJsonObject MainWindow::activeAnnotationPromptCancelConfirmationSnapshot(const QJsonObject& params) const {
    if (!m_samAnnotatorDialog || !m_samAnnotatorDialog->isVisible())
        return {{QStringLiteral("available"), false}, {QStringLiteral("params"), params}};
    const QByteArray sessionBytes =
        QJsonDocument(m_samAnnotatorDialog->session().toJson()).toJson(QJsonDocument::Compact);
    QJsonObject snapshot{
        {QStringLiteral("available"), true},
        {QStringLiteral("ownerIdentity"), QString::number(reinterpret_cast<quintptr>(this))},
        {QStringLiteral("dialogIdentity"), QString::number(m_samAnnotatorDialog->instanceId())},
        {QStringLiteral("sessionDigest"),
         QString::fromLatin1(QCryptographicHash::hash(sessionBytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("params"), params}};
    const QJsonObject promptState = m_samAnnotatorDialog->agentPromptStateSnapshot();
    for (const QString& key : {QStringLiteral("promptGeneration"), QStringLiteral("requestId"),
                               QStringLiteral("predictionInFlight"), QStringLiteral("stateDigest")})
        snapshot.insert(key, promptState.value(key));
    return snapshot;
}

QJsonObject MainWindow::activeAnnotationSessionSaveConfirmationSnapshot(const QJsonObject& params) const {
    if (!m_samAnnotatorDialog || !m_samAnnotatorDialog->isVisible())
        return {{QStringLiteral("available"), false}, {QStringLiteral("params"), params}};
    const AnnotationSession currentSession = m_samAnnotatorDialog->session();
    const QByteArray sessionBytes = QJsonDocument(currentSession.toJson()).toJson(QJsonDocument::Compact);
    const QImage image = m_samAnnotatorDialog->imageSnapshot();
    const QString outputPath = params.value(QStringLiteral("outputPath")).toString();
    return {{QStringLiteral("available"), true},
            {QStringLiteral("ownerIdentity"), QString::number(reinterpret_cast<quintptr>(this))},
            {QStringLiteral("dialogIdentity"), QString::number(m_samAnnotatorDialog->instanceId())},
            {QStringLiteral("sessionDigest"),
             QString::fromLatin1(QCryptographicHash::hash(sessionBytes, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("imageDigest"), AnnotationSession::computeImageDigest(image)},
            {QStringLiteral("objectCount"), currentSession.annotations.size()},
            {QStringLiteral("outputTarget"),
             AnnotationSessionExport::newOutputTargetSnapshot(outputPath, QStringLiteral(".deeplux-anno.json"))},
            {QStringLiteral("params"), params}};
}

QJsonObject MainWindow::activeAnnotationSessionOpenConfirmationSnapshot(const QJsonObject& params) const {
    if (!m_samAnnotatorDialog || !m_samAnnotatorDialog->isVisible())
        return {{QStringLiteral("error"), QStringLiteral("The SAM annotation dialog is not open")},
                {QStringLiteral("errorCode"), QStringLiteral("SAM_DIALOG_NOT_OPEN")},
                {QStringLiteral("retryable"), false}};
    const QString sessionPath = params.value(QStringLiteral("sessionPath")).toString();
    AnnotationSessionOpenCandidate candidate;
    QString errorCode;
    QString errorMessage;
    if (!AnnotationSessionOpen::capture(sessionPath, &candidate, &errorCode, &errorMessage))
        return {{QStringLiteral("error"), errorMessage},
                {QStringLiteral("errorCode"), errorCode},
                {QStringLiteral("retryable"), false}};

    const QByteArray currentSession =
        QJsonDocument(m_samAnnotatorDialog->session().toJson()).toJson(QJsonDocument::Compact);
    const QImage currentImage = m_samAnnotatorDialog->imageSnapshot();
    QJsonObject snapshot = candidate.snapshot;
    snapshot.insert(QStringLiteral("available"), true);
    snapshot.insert(QStringLiteral("ownerIdentity"), QString::number(reinterpret_cast<quintptr>(this)));
    snapshot.insert(QStringLiteral("dialogIdentity"), QString::number(m_samAnnotatorDialog->instanceId()));
    snapshot.insert(QStringLiteral("activeSessionDigest"),
                    QString::fromLatin1(QCryptographicHash::hash(currentSession, QCryptographicHash::Sha256).toHex()));
    snapshot.insert(QStringLiteral("activeImageDigest"), AnnotationSession::computeImageDigest(currentImage));
    snapshot.insert(QStringLiteral("params"), params);
    return snapshot;
}

QJsonObject MainWindow::executeAgentActiveAnnotationAction(const QString& toolName, const QJsonObject& params) {
    auto error = [](const QString& message, const QString& code) {
        return QJsonObject{{QStringLiteral("error"), message},
                           {QStringLiteral("errorCode"), code},
                           {QStringLiteral("retryable"), false}};
    };
    if (QThread::currentThread() != thread())
        return error(QStringLiteral("SAM annotation GUI action must run on the owner thread"),
                     QStringLiteral("THREAD_AFFINITY"));
    if (toolName == QLatin1String("__sam_mask_edit_confirmation_snapshot"))
        return activeAnnotationMaskConfirmationSnapshot(params);
    if (toolName == QLatin1String("__sam_category_edit_confirmation_snapshot"))
        return activeAnnotationCategoryConfirmationSnapshot(params);
    if (toolName == QLatin1String("__sam_prompt_cancel_confirmation_snapshot"))
        return activeAnnotationPromptCancelConfirmationSnapshot(params);
    if (toolName == QLatin1String("__sam_session_save_confirmation_snapshot"))
        return activeAnnotationSessionSaveConfirmationSnapshot(params);
    if (toolName == QLatin1String("__sam_session_open_confirmation_snapshot"))
        return activeAnnotationSessionOpenConfirmationSnapshot(params);
    if (!m_samAnnotatorDialog || !m_samAnnotatorDialog->isVisible())
        return error(QStringLiteral("The SAM annotation dialog is not open"), QStringLiteral("SAM_DIALOG_NOT_OPEN"));
    if (toolName == QLatin1String("__sam_session_save_preflight") ||
        toolName == QLatin1String("save_active_annotation_session")) {
        if (params.keys() != QStringList{QStringLiteral("outputPath")})
            return error(QStringLiteral("Expected only an absolute outputPath"), QStringLiteral("INVALID_PARAM"));
        const QJsonValue outputPathValue = params.value(QStringLiteral("outputPath"));
        const QString outputPath = outputPathValue.toString();
        if (!outputPathValue.isString() || outputPath.trimmed().isEmpty() || outputPath.toUtf8().size() > 4096 ||
            !QFileInfo(outputPath).isAbsolute())
            return error(QStringLiteral("outputPath must be an absolute path of at most 4096 UTF-8 bytes"),
                         QStringLiteral("INVALID_PARAM"));
        const AnnotationSession currentSession = m_samAnnotatorDialog->session();
        if (m_samAnnotatorDialog->imageSnapshot().isNull())
            return error(QStringLiteral("The active SAM dialog has no valid image"), QStringLiteral("NO_IMAGE"));
        if (currentSession.annotations.isEmpty())
            return error(QStringLiteral("The active SAM session has no confirmed annotations"),
                         QStringLiteral("NO_ANNOTATIONS"));
        const QJsonObject snapshot = activeAnnotationSessionSaveConfirmationSnapshot(params);
        QJsonObject target = snapshot.value(QStringLiteral("outputTarget")).toObject();
        if (!target.value(QStringLiteral("suffixValid")).toBool())
            return error(QStringLiteral("outputPath must end in .deeplux-anno.json"), QStringLiteral("INVALID_PARAM"));
        if (target.value(QStringLiteral("pathBytes")).toInt() > 4096 ||
            !target.value(QStringLiteral("parentIdentityAvailable")).toBool() ||
            !target.value(QStringLiteral("parentWritable")).toBool())
            return error(QStringLiteral("The output parent must exist and be writable"),
                         QStringLiteral("PRECONDITION_FAILED"));
        if (target.value(QStringLiteral("targetState")).toString() != QLatin1String("absent") ||
            target.value(QStringLiteral("outputIsSymlink")).toBool())
            return error(QStringLiteral("The output target already exists"), QStringLiteral("OUTPUT_EXISTS"));
        if (toolName == QLatin1String("__sam_session_save_preflight"))
            return {};

        const QByteArray bytes = QJsonDocument(currentSession.toJson()).toJson(QJsonDocument::Indented);
        if (bytes.isEmpty() || bytes.size() > 64LL * 1024 * 1024)
            return error(QStringLiteral("The active annotation session exceeds the 64 MiB output limit"),
                         QStringLiteral("OUTPUT_LIMIT_EXCEEDED"));
        const QJsonObject approvedSnapshot =
            AgentController::instance().confirmedActiveAnnotationSessionSaveSnapshot(outputPath);
        if (approvedSnapshot.isEmpty() || approvedSnapshot != snapshot)
            return error(QStringLiteral("The approved active session or output target changed"),
                         QStringLiteral("CONFIRMATION_EXPIRED"));
        target = approvedSnapshot.value(QStringLiteral("outputTarget")).toObject();
        QString errorCode;
        if (!AnnotationSessionExport::publishSessionBytesNoReplace(outputPath, QStringLiteral(".deeplux-anno.json"),
                                                                   target, bytes, &errorCode)) {
            const QString code = errorCode.isEmpty() ? QStringLiteral("FILE_WRITE_FAILED") : errorCode;
            return error(QStringLiteral("The active annotation session could not be saved"), code);
        }
        const QString savedPath = target.value(QStringLiteral("outputCanonicalParent")).toString() + QLatin1Char('/') +
                                  target.value(QStringLiteral("outputFileName")).toString();
        return {{QStringLiteral("status"), QStringLiteral("saved")},
                {QStringLiteral("outputPath"), QDir::cleanPath(savedPath)},
                {QStringLiteral("bytes"), bytes.size()},
                {QStringLiteral("sha256"),
                 QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
                {QStringLiteral("objectCount"), currentSession.annotations.size()}};
    }
    if (toolName == QLatin1String("__sam_session_open_preflight") ||
        toolName == QLatin1String("open_active_annotation_session")) {
        if (params.keys() != QStringList{QStringLiteral("sessionPath")})
            return error(QStringLiteral("Expected only an absolute sessionPath"), QStringLiteral("INVALID_PARAM"));
        const QJsonValue pathValue = params.value(QStringLiteral("sessionPath"));
        const QString sessionPath = pathValue.toString();
        if (!pathValue.isString() || sessionPath.trimmed().isEmpty() || sessionPath.toUtf8().size() > 4096 ||
            !QFileInfo(sessionPath).isAbsolute() ||
            !sessionPath.endsWith(QStringLiteral(".deeplux-anno.json"), Qt::CaseInsensitive))
            return error(
                QStringLiteral("sessionPath must be an absolute .deeplux-anno.json path of at most 4096 bytes"),
                QStringLiteral("INVALID_PARAM"));

        AnnotationSessionOpenCandidate candidate;
        QString errorCode;
        QString errorMessage;
        if (!AnnotationSessionOpen::capture(sessionPath, &candidate, &errorCode, &errorMessage))
            return error(errorMessage, errorCode);
        if (toolName == QLatin1String("__sam_session_open_preflight"))
            return {};

        const QByteArray currentSession =
            QJsonDocument(m_samAnnotatorDialog->session().toJson()).toJson(QJsonDocument::Compact);
        const QImage visibleImage = m_samAnnotatorDialog->imageSnapshot();
        QJsonObject currentSnapshot = candidate.snapshot;
        currentSnapshot.insert(QStringLiteral("available"), true);
        currentSnapshot.insert(QStringLiteral("ownerIdentity"), QString::number(reinterpret_cast<quintptr>(this)));
        currentSnapshot.insert(QStringLiteral("dialogIdentity"), QString::number(m_samAnnotatorDialog->instanceId()));
        currentSnapshot.insert(
            QStringLiteral("activeSessionDigest"),
            QString::fromLatin1(QCryptographicHash::hash(currentSession, QCryptographicHash::Sha256).toHex()));
        currentSnapshot.insert(QStringLiteral("activeImageDigest"),
                               AnnotationSession::computeImageDigest(visibleImage));
        currentSnapshot.insert(QStringLiteral("params"), params);
        const QJsonObject approvedSnapshot =
            AgentController::instance().confirmedActiveAnnotationSessionOpenSnapshot(sessionPath);
        if (approvedSnapshot.isEmpty() || approvedSnapshot != currentSnapshot)
            return error(QStringLiteral("The approved session, image, or SAM dialog changed"),
                         QStringLiteral("CONFIRMATION_EXPIRED"));

        const QImage imageCandidate = candidate.imageBound ? candidate.boundImage : visibleImage;
        if (!m_samAnnotatorDialog->openAgentAnnotationSessionSnapshot(
                candidate.session, imageCandidate, candidate.imageBound, candidate.legacyImageBindingUnverified,
                candidate.sessionPath, &errorMessage))
            return error(errorMessage.isEmpty() ? QStringLiteral("The SAM session could not be opened") : errorMessage,
                         QStringLiteral("SESSION_OPEN_FAILED"));
        return {{QStringLiteral("status"), QStringLiteral("opened")},
                {QStringLiteral("sessionPath"), candidate.sessionPath},
                {QStringLiteral("objectCount"), candidate.session.annotations.size()},
                {QStringLiteral("imageWidth"), candidate.session.imageWidth},
                {QStringLiteral("imageHeight"), candidate.session.imageHeight},
                {QStringLiteral("imageBound"), candidate.imageBound},
                {QStringLiteral("legacyImageBindingUnverified"), candidate.legacyImageBindingUnverified}};
    }
    const bool requiresImage = toolName == QLatin1String("list_active_annotation_objects") ||
                               toolName == QLatin1String("__sam_mask_edit_preflight") ||
                               toolName == QLatin1String("edit_active_annotation_masks");
    if (requiresImage && m_samAnnotatorDialog->imageSnapshot().isNull())
        return error(QStringLiteral("The active SAM dialog has no image"), QStringLiteral("NO_IMAGE"));

    if (toolName == QLatin1String("list_active_annotation_objects")) {
        for (const QString& key : params.keys()) {
            if (key != QLatin1String("offset") && key != QLatin1String("limit"))
                return error(QStringLiteral("Unknown parameter: %1").arg(key), QStringLiteral("INVALID_PARAM"));
        }
        const QJsonValue offsetValue = params.value(QStringLiteral("offset"));
        const QJsonValue limitValue = params.value(QStringLiteral("limit"));
        const int offset = offsetValue.isUndefined() ? 0 : offsetValue.toInt(-1);
        const int limit = limitValue.isUndefined() ? 20 : limitValue.toInt(-1);
        if ((!offsetValue.isUndefined() &&
             (!offsetValue.isDouble() || offsetValue.toDouble() != offset || offset < 0)) ||
            (!limitValue.isUndefined() &&
             (!limitValue.isDouble() || limitValue.toDouble() != limit || limit < 1 || limit > 50)))
            return error(QStringLiteral("offset or limit is outside the allowed range"),
                         QStringLiteral("INVALID_PARAM"));
        return m_samAnnotatorDialog->listAgentAnnotationObjects(offset, limit);
    }
    if (toolName == QLatin1String("__sam_mask_edit_preflight")) {
        const QJsonObject preflight = m_samAnnotatorDialog->applyAgentMaskEdit(params, true);
        const QString status = preflight.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("ready") || status == QLatin1String("unchanged"))
            return {};
        return preflight;
    }
    if (toolName == QLatin1String("edit_active_annotation_masks"))
        return m_samAnnotatorDialog->applyAgentMaskEdit(params);
    if (toolName == QLatin1String("list_active_annotation_categories")) {
        for (const QString& key : params.keys()) {
            if (key != QLatin1String("offset") && key != QLatin1String("limit"))
                return error(QStringLiteral("Unknown parameter: %1").arg(key), QStringLiteral("INVALID_PARAM"));
        }
        const QJsonValue offsetValue = params.value(QStringLiteral("offset"));
        const QJsonValue limitValue = params.value(QStringLiteral("limit"));
        const int offset = offsetValue.isUndefined() ? 0 : offsetValue.toInt(-1);
        const int limit = limitValue.isUndefined() ? 20 : limitValue.toInt(-1);
        if ((!offsetValue.isUndefined() &&
             (!offsetValue.isDouble() || offsetValue.toDouble() != offset || offset < 0)) ||
            (!limitValue.isUndefined() &&
             (!limitValue.isDouble() || limitValue.toDouble() != limit || limit < 1 || limit > 50)))
            return error(QStringLiteral("offset or limit is outside the allowed range"),
                         QStringLiteral("INVALID_PARAM"));
        return m_samAnnotatorDialog->listAgentAnnotationCategories(offset, limit);
    }
    if (toolName == QLatin1String("get_active_annotation_prompt_status") ||
        toolName == QLatin1String("cancel_active_annotation_prompt") ||
        toolName == QLatin1String("__sam_prompt_cancel_preflight")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This SAM prompt action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));
        if (toolName == QLatin1String("get_active_annotation_prompt_status"))
            return m_samAnnotatorDialog->agentPromptStatus();
        if (toolName == QLatin1String("__sam_prompt_cancel_preflight"))
            return {};
        return m_samAnnotatorDialog->cancelAgentPrompt();
    }
    if (toolName == QLatin1String("__sam_category_edit_preflight")) {
        const QJsonObject preflight = m_samAnnotatorDialog->applyAgentCategoryEdit(params, true);
        const QString status = preflight.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("ready") || status == QLatin1String("unchanged"))
            return {};
        return preflight;
    }
    if (toolName == QLatin1String("edit_active_annotation_categories"))
        return m_samAnnotatorDialog->applyAgentCategoryEdit(params);
    return error(QStringLiteral("Unsupported active annotation action"), QStringLiteral("UNKNOWN_TOOL"));
}

QJsonObject MainWindow::executeAgentQuickMeasurementAction(const QString& toolName, const QJsonObject& params) {
    auto error = [](const QString& message, const QString& code) {
        return QJsonObject{{QStringLiteral("error"), message},
                           {QStringLiteral("errorCode"), code},
                           {QStringLiteral("retryable"), false}};
    };
    if (QThread::currentThread() != thread())
        return error(QStringLiteral("Quick measurement GUI action must run on the owner thread"),
                     QStringLiteral("THREAD_AFFINITY"));
    if (toolName == QLatin1String("__quick_measurement_confirmation_snapshot"))
        return quickMeasurementConfirmationSnapshot(params);
    if (toolName == QLatin1String("__quick_measurement_owner_destroyed")) {
        cancelAgentQuickMeasurement(QStringLiteral("expired"), QStringLiteral("OWNER_DESTROYED"));
        return currentAgentQuickMeasurementStatus();
    }
    if (toolName == QLatin1String("list_viewports")) {
        if (!m_displayManager)
            return error(QStringLiteral("Viewport manager is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));
        QJsonArray viewports;
        for (ViewportWidget* viewport : m_displayManager->allViewports()) {
            if (!viewport)
                continue;
            const bool hasImage =
                viewport->hasCachedImage() || (viewport->imageWidget() && viewport->imageWidget()->hasImage());
            const bool hasPointCloud = viewport->hasCachedPointCloud();
            const QString mode = viewport->displayMode() == ViewportWidget::DisplayMode::Auto2D ? QStringLiteral("2d")
                                                                                                : QStringLiteral("3d");
            viewports.append(QJsonObject{{QStringLiteral("viewportId"), viewport->viewportId()},
                                         {QStringLiteral("title"), viewport->title()},
                                         {QStringLiteral("visible"), viewport->isVisible()},
                                         {QStringLiteral("mode"), mode},
                                         {QStringLiteral("hasImage"), hasImage},
                                         {QStringLiteral("hasPointCloud"), hasPointCloud}});
        }
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("viewports"), viewports},
                {QStringLiteral("total"), viewports.size()}};
    }
    if (toolName == QLatin1String("get_quick_measurement_status")) {
        const QString requestId = params.value(QStringLiteral("requestId")).toString();
        if (requestId.isEmpty() || requestId != m_agentQuickMeasurement.requestId)
            return error(QStringLiteral("Unknown or replaced quick measurement request"),
                         QStringLiteral("QUICK_MEASURE_REQUEST_NOT_FOUND"));
        return currentAgentQuickMeasurementStatus();
    }
    if (toolName == QLatin1String("cancel_quick_measurement")) {
        const QString requestId = params.value(QStringLiteral("requestId")).toString();
        if (requestId.isEmpty() || requestId != m_agentQuickMeasurement.requestId)
            return error(QStringLiteral("Unknown or replaced quick measurement request"),
                         QStringLiteral("QUICK_MEASURE_REQUEST_NOT_FOUND"));
        const QJsonObject current = currentAgentQuickMeasurementStatus();
        if (current.value(QStringLiteral("status")) != QLatin1String("awaiting_user_input"))
            return current;
        cancelAgentQuickMeasurement(QStringLiteral("cancelled"), QStringLiteral("CANCELLED_BY_AGENT"));
        return currentAgentQuickMeasurementStatus();
    }
    if (toolName != QLatin1String("begin_quick_measurement"))
        return error(QStringLiteral("Unsupported quick measurement action"), QStringLiteral("UNKNOWN_TOOL"));
    if (m_quickMeasureActive || m_agentQuickMeasurement.status == QLatin1String("awaiting_user_input"))
        return error(QStringLiteral("Another quick measurement is already active"),
                     QStringLiteral("QUICK_MEASURE_BUSY"));

    const QString viewportId = params.value(QStringLiteral("viewportId")).toString();
    const QString dimension = params.value(QStringLiteral("dimension")).toString();
    const QString measurementType = params.value(QStringLiteral("measurementType")).toString();
    const QMap<QString, int> typeIndices{
        {QStringLiteral("distance_2_points"), 0}, {QStringLiteral("point_to_line"), 1},
        {QStringLiteral("segment_gap"), 2},       {QStringLiteral("angle_3_points"), 3},
        {QStringLiteral("circle_3_points"), 4},   {QStringLiteral("rectangle_area"), 5}};
    if (viewportId.trimmed().isEmpty() || (dimension != QLatin1String("2d") && dimension != QLatin1String("3d")) ||
        !typeIndices.contains(measurementType))
        return error(QStringLiteral("viewportId, dimension, or measurementType is invalid"),
                     QStringLiteral("INVALID_PARAM"));
    if (dimension == QLatin1String("3d") && measurementType == QLatin1String("rectangle_area"))
        return error(QStringLiteral("Rectangle area quick measurement is only supported in 2D"),
                     QStringLiteral("UNSUPPORTED_MEASUREMENT_DIMENSION"));
    ViewportWidget* viewport = m_displayManager ? m_displayManager->viewport(viewportId) : nullptr;
    if (!viewport)
        return error(QStringLiteral("The exact viewport was not found"), QStringLiteral("VIEWPORT_NOT_FOUND"));
    const QJsonObject snapshot = quickMeasurementConfirmationSnapshot(params);
    if (!snapshot.value(QStringLiteral("targetAvailable")).toBool())
        return error(QStringLiteral("The target viewport has no compatible visible measurement content"),
                     QStringLiteral("VIEWPORT_CONTENT_UNAVAILABLE"));

    QStringList steps;
    if (measurementType == QLatin1String("distance_2_points"))
        steps = QStringList{QStringLiteral("点击第一个端点"), QStringLiteral("点击第二个端点")};
    else if (measurementType == QLatin1String("point_to_line"))
        steps =
            QStringList{QStringLiteral("点击待测点"), QStringLiteral("点击直线起点"), QStringLiteral("点击直线终点")};
    else if (measurementType == QLatin1String("segment_gap"))
        steps = QStringList{QStringLiteral("点击第一条线段起点"), QStringLiteral("点击第一条线段终点"),
                            QStringLiteral("点击第二条线段起点"), QStringLiteral("点击第二条线段终点")};
    else if (measurementType == QLatin1String("angle_3_points"))
        steps = QStringList{QStringLiteral("点击角顶点"), QStringLiteral("点击第一条边上的点"),
                            QStringLiteral("点击第二条边上的点")};
    else if (measurementType == QLatin1String("circle_3_points"))
        steps = QStringList{QStringLiteral("点击圆周第一点"), QStringLiteral("点击圆周第二点"),
                            QStringLiteral("点击圆周第三点")};
    else
        steps = QStringList{QStringLiteral("在图像中拖拽矩形 ROI")};

    AgentQuickMeasurementRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.status = QStringLiteral("awaiting_user_input");
    request.viewportId = viewportId;
    request.dimension = dimension;
    request.measurementType = measurementType;
    request.coordinateSystem = dimension == QLatin1String("2d") ? QStringLiteral("image_pixels")
                                                                : QStringLiteral("point_cloud_world_coordinates");
    request.steps = steps;
    request.measurementTypeIndex = typeIndices.value(measurementType);
    request.viewportIdentity = reinterpret_cast<quintptr>(viewport);
    const Project* project = ProjectManager::instance().currentProject();
    request.projectRuntimeIdentity = project ? project->runtimeIdentity() : 0;
    request.viewportContentGeneration = viewport->contentGeneration();
    m_agentQuickMeasurement = request;
    QObject::disconnect(m_agentQuickMeasureRoiConnection);
    m_agentQuickMeasureRoiConnection = {};
    m_quickMeasureActive = true;
    m_quickMeasureType = request.measurementTypeIndex;
    m_quickMeasurePoints.clear();
    m_quickMeasurePoints3D.clear();
    viewport->setPickMode(dimension == QLatin1String("3d"));
    viewport->setCursor(Qt::CrossCursor);
    if (dimension == QLatin1String("2d") && request.measurementTypeIndex == 5 && viewport->imageWidget()) {
        viewport->imageWidget()->clearRois();
        viewport->imageWidget()->setRoiMode(RoiType::Rectangle1);
        m_agentQuickMeasureRoiConnection =
            connect(viewport->imageWidget(), &HImageWidget::roiCreated, this, [this, viewportId](const RoiData& roi) {
                if (m_agentQuickMeasurement.status != QLatin1String("awaiting_user_input") ||
                    m_agentQuickMeasurement.viewportId != viewportId ||
                    m_agentQuickMeasurement.dimension != QLatin1String("2d") ||
                    m_agentQuickMeasurement.measurementTypeIndex != 5)
                    return;
                QString reason;
                if (!agentQuickMeasurementTargetStillValid(&reason)) {
                    cancelAgentQuickMeasurement(QStringLiteral("expired"), reason);
                    return;
                }
                const QPointF a(roi.x1, roi.y1);
                const QPointF b(roi.x2, roi.y2);
                const QPointF tl(qMin(a.x(), b.x()), qMin(a.y(), b.y()));
                const QPointF br(qMax(a.x(), b.x()), qMax(a.y(), b.y()));
                const QPointF topRight(br.x(), tl.y());
                const QPointF bl(tl.x(), br.y());
                QJsonArray pickedCorners;
                if (ViewportWidget* target = m_displayManager->viewport(viewportId)) {
                    const ImageCoordinateProvenance provenance = target->imageCoordinateProvenance();
                    for (const QPointF& corner : {tl, topRight, br, bl}) {
                        const auto sourcePoint = provenance.mapLocalToSource(corner);
                        QJsonObject picked{{QStringLiteral("imageLocal"), QJsonArray{corner.x(), corner.y()}},
                                           {QStringLiteral("coordinateFrame"), QStringLiteral("ImageLocalPixel")},
                                           {QStringLiteral("sourceCoordinateAvailable"), sourcePoint.has_value()}};
                        if (sourcePoint) {
                            picked.insert(QStringLiteral("sourceImage"), provenance.sourceImageId);
                            picked.insert(QStringLiteral("sourceImageSize"),
                                          QJsonArray{provenance.sourceSize.width(), provenance.sourceSize.height()});
                            picked.insert(QStringLiteral("sourceImagePixel"),
                                          QJsonArray{sourcePoint->x(), sourcePoint->y()});
                        }
                        pickedCorners.append(picked);
                    }
                }
                m_agentQuickMeasurement.pickedCoordinates = pickedCorners;
                const double width = std::abs(br.x() - tl.x());
                const double height = std::abs(br.y() - tl.y());
                const QString label =
                    tr("W=%1, H=%2, A=%3").arg(width, 0, 'f', 2).arg(height, 0, 'f', 2).arg(width * height, 0, 'f', 2);
                if (ViewportWidget* target = m_displayManager->viewport(viewportId)) {
                    if (HImageWidget* image = target->imageWidget()) {
                        image->clearRois();
                        image->setMeasurementOverlay({{tl, QStringLiteral("TL")}, {br, QStringLiteral("BR")}},
                                                     {{tl, topRight, QString()},
                                                      {topRight, br, QString()},
                                                      {br, bl, QString()},
                                                      {bl, tl, label}});
                    }
                }
                Logger::instance().info(tr("快速测量: 矩形 宽=%1, 高=%2, 面积=%3")
                                            .arg(width, 0, 'f', 3)
                                            .arg(height, 0, 'f', 3)
                                            .arg(width * height, 0, 'f', 3),
                                        "Measurement");
                m_agentQuickMeasurement.completedSteps = 1;
                completeAgentQuickMeasurement();
            });
    }
    if (m_quickMeasureAction)
        m_quickMeasureAction->setChecked(true);
    return currentAgentQuickMeasurementStatus();
}

QJsonObject MainWindow::closeViewportById(const QString& viewportId) {
    auto error = [](const QString& message, const QString& code) {
        return QJsonObject{{QStringLiteral("error"), message},
                           {QStringLiteral("errorCode"), code},
                           {QStringLiteral("retryable"), false}};
    };
    if (!m_displayManager)
        return error(QStringLiteral("Viewport manager is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));

    ViewportWidget* target = m_displayManager->viewport(viewportId);
    if (!target)
        return error(QStringLiteral("Viewport was not found"), QStringLiteral("VIEWPORT_NOT_FOUND"));

    // Resolve the lazy primary ID before deciding whether this viewport may be destroyed.
    ViewportWidget* primary = primaryDataSourceViewport();
    if (!primary)
        return error(QStringLiteral("Primary viewport is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));

    for (auto it = m_dataSourceViewportIds.begin(); it != m_dataSourceViewportIds.end();) {
        if (it.value() == viewportId)
            it = m_dataSourceViewportIds.erase(it);
        else
            ++it;
    }

    const bool keepViewport = target == primary || m_displayManager->viewportCount() <= 1;
    if (keepViewport) {
        target->clearDisplay();
        target->setTitle(tr("主视图"));
        return {{QStringLiteral("status"), QStringLiteral("closed")},
                {QStringLiteral("viewportId"), viewportId},
                {QStringLiteral("disposition"), QStringLiteral("cleared")},
                {QStringLiteral("destroyed"), false}};
    }

    m_displayManager->destroyViewport(viewportId);
    return {{QStringLiteral("status"), QStringLiteral("closed")},
            {QStringLiteral("viewportId"), viewportId},
            {QStringLiteral("disposition"), QStringLiteral("destroyed")},
            {QStringLiteral("destroyed"), true}};
}

IModule* MainWindow::measurementInputForPicking(QString& instanceId) {
    if (!m_pendingMeasurementInputId.isEmpty()) {
        IModule* pendingModule = m_flowModules.value(m_pendingMeasurementInputId, nullptr);
        if (pendingModule && pendingModule->moduleId() == QStringLiteral("com.deeplux.plugin.measurementinput")) {
            instanceId = m_pendingMeasurementInputId;
            return pendingModule;
        }
        m_pendingMeasurementInputId.clear();
    }

    QTreeWidgetItem* item = m_processTree ? m_processTree->currentItem() : nullptr;
    if (item && item->data(0, Qt::UserRole).toString() == QStringLiteral("flow_item")) {
        const QString selectedId = item->data(0, Qt::UserRole + 1).toString();
        IModule* selectedModule = m_flowModules.value(selectedId, nullptr);
        if (selectedModule && selectedModule->moduleId() == QStringLiteral("com.deeplux.plugin.measurementinput")) {
            m_activeMeasurementInputId = selectedId;
            instanceId = selectedId;
            return selectedModule;
        }
    }

    IModule* module = m_flowModules.value(m_activeMeasurementInputId, nullptr);
    if (module && module->moduleId() == QStringLiteral("com.deeplux.plugin.measurementinput")) {
        instanceId = m_activeMeasurementInputId;
        return module;
    }

    m_activeMeasurementInputId.clear();
    return nullptr;
}

bool MainWindow::requestMeasurementInputForRun() {
    if (!m_pendingMeasurementInputId.isEmpty()) {
        return true;
    }

    bool hasMeasurementInput = false;
    for (int row = 0; row < m_processTreeController->topLevelItemCount(); ++row) {
        QTreeWidgetItem* item = m_processTreeController->topLevelItem(row);
        const QString instanceId = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
        IModule* module = m_flowModules.value(instanceId, nullptr);
        if (!module || module->moduleId() != QStringLiteral("com.deeplux.plugin.measurementinput")) {
            continue;
        }
        hasMeasurementInput = true;

        const QJsonObject params = module->currentParams();
        const int requiredPickCount = measurementPickCountForMode(params["mode"].toString());
        if (requiredPickCount == 0) {
            continue;
        }

        const bool explicitlyAwaitingPick = params["awaitUserPick"].toBool(false);
        const bool legacyActiveInput = !params.contains("awaitUserPick") && instanceId == m_activeMeasurementInputId;
        if (!explicitlyAwaitingPick &&
            (!legacyActiveInput || m_measurementPickCount.value(instanceId, 0) >= requiredPickCount)) {
            continue;
        }

        if (explicitlyAwaitingPick) {
            m_measurementPickCount[instanceId] = 0;
            m_measurementPickCursor[instanceId] = 0;
        }
        m_pendingMeasurementViewportId.clear();
        m_pendingMeasurementViewports.clear();
        if (m_displayManager) {
            for (ViewportWidget* viewport : m_displayManager->allViewports()) {
                if (viewport) {
                    m_pendingMeasurementViewports.insert(
                        viewport->viewportId(), MeasurementViewportSnapshot{viewport, viewport->contentGeneration()});
                }
            }
        }
        m_activeMeasurementInputId = instanceId;
        m_pendingMeasurementInputId = instanceId;
        selectModule(instanceId, true, true);
        setProcessItemStatus(
            item, QStringLiteral("running"),
            tr("等待拾取 %1/%2").arg(m_measurementPickCount.value(instanceId, 0) + 1).arg(requiredPickCount));
        Logger::instance().info(
            tr("流程等待测量输入：请在视图中拾取 %1").arg(measurementInputModeText(params["mode"].toString())),
            "Measurement");
        return true;
    }

    if (!hasMeasurementInput) {
        for (int row = 0; row < m_processTreeController->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = m_processTreeController->topLevelItem(row);
            const QString consumerInstanceId = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
            IModule* consumer = m_flowModules.value(consumerInstanceId, nullptr);
            const QString mode = consumer ? measurementInputModeForConsumer(consumer->moduleId()) : QString();
            if (mode.isEmpty()) {
                continue;
            }

            if (!ensureMeasurementInputForMode(mode, consumerInstanceId).isEmpty()) {
                Logger::instance().info(tr("已为测量步骤创建交互输入，等待在视图中拾取"), "Measurement");
                return requestMeasurementInputForRun();
            }
            break;
        }
    }

    return false;
}

bool MainWindow::bindMeasurementPickViewport(const QString& viewportId) {
    if (m_pendingMeasurementInputId.isEmpty())
        return true;

    const auto boundViewport = m_pendingMeasurementViewports.constFind(m_pendingMeasurementViewportId);
    if (!m_pendingMeasurementViewportId.isEmpty() &&
        (boundViewport == m_pendingMeasurementViewports.cend() || !boundViewport->viewport || !m_displayManager ||
         m_displayManager->viewport(m_pendingMeasurementViewportId) != boundViewport->viewport.data() ||
         boundViewport->viewport->contentGeneration() != boundViewport->contentGeneration)) {
        Logger::instance().warning(tr("等待测量输入期间视口内容已改变，已取消本轮运行"), "Measurement");
        onStop();
        return false;
    }

    const auto sourceViewport = m_pendingMeasurementViewports.constFind(viewportId);
    if (sourceViewport == m_pendingMeasurementViewports.cend()) {
        Logger::instance().warning(tr("该视口不属于当前测量输入等待上下文，已忽略采点"), "Measurement");
        return false;
    }
    if (!sourceViewport->viewport || !m_displayManager ||
        m_displayManager->viewport(viewportId) != sourceViewport->viewport.data() ||
        sourceViewport->viewport->contentGeneration() != sourceViewport->contentGeneration) {
        Logger::instance().warning(tr("等待测量输入期间视口内容已改变，已取消本轮运行"), "Measurement");
        onStop();
        return false;
    }
    if (!m_pendingMeasurementViewportId.isEmpty() && m_pendingMeasurementViewportId != viewportId) {
        Logger::instance().warning(tr("本轮测量输入已绑定到其他视口，已忽略该采点"), "Measurement");
        return false;
    }

    m_pendingMeasurementViewportId = viewportId;
    return true;
}

void MainWindow::clearPendingMeasurementPick() {
    const QString inputId =
        m_pendingMeasurementInputId.isEmpty() ? m_activeMeasurementInputId : m_pendingMeasurementInputId;
    if (!inputId.isEmpty()) {
        if (QTreeWidgetItem* item = m_processTreeController->instanceItem(inputId)) {
            clearExecutionHighlight(item);
            item->setText(1, QString());
        }
    }
    m_pendingMeasurementInputId.clear();
    m_pendingMeasurementViewportId.clear();
    m_pendingMeasurementViewports.clear();
    if (m_activeMeasurementInputId == inputId)
        m_activeMeasurementInputId.clear();
}

void MainWindow::finishMeasurementPick(const QString& instanceId, const QJsonObject& params, bool is3D) {
    const int count = m_measurementPickCount.value(instanceId, 0) + 1;
    m_measurementPickCount[instanceId] = count;
    if (is3D) {
        refreshMeasurementOverlay3D(params, count);
    } else {
        refreshMeasurementOverlay(params, count);
    }

    const int requiredPickCount = measurementPickCountForMode(params["mode"].toString());
    if (count < requiredPickCount) {
        if (instanceId == m_pendingMeasurementInputId) {
            if (QTreeWidgetItem* item = m_processTreeController->instanceItem(instanceId)) {
                setProcessItemStatus(item, QStringLiteral("running"),
                                     tr("等待拾取 %1/%2").arg(count + 1).arg(requiredPickCount));
            }
        }
        return;
    }

    if (IModule* module = m_flowModules.value(instanceId, nullptr)) {
        module->setParam("awaitUserPick", false);
    }
    if (Project* project = ProjectManager::instance().currentProject()) {
        project->setModuleParam(instanceId, "awaitUserPick", false);
    }
    if (instanceId != m_pendingMeasurementInputId) {
        return;
    }

    m_pendingMeasurementInputId.clear();
    if (QTreeWidgetItem* item = m_processTreeController->instanceItem(instanceId)) {
        setProcessItemStatus(item, QStringLiteral("success"), tr("拾取完成"));
    }
    Logger::instance().info(tr("测量输入完成，继续运行流程"), "Measurement");
    Project* completedProject = ProjectManager::instance().currentProject();
    const quint64 completedProjectIdentity = completedProject ? completedProject->runtimeIdentity() : 0;
    const ModuleInstance* completedTarget = completedProject ? completedProject->findModule(instanceId) : nullptr;
    const quint64 completedTargetGeneration =
        completedProject ? completedProject->parameterInstanceGeneration(instanceId) : 0;
    const QJsonObject completedTargetSnapshot = completedTarget ? completedTarget->toJson() : QJsonObject{};
    const quint64 continuationGeneration = m_measurementPickContinuationGeneration;
    const QString continuationViewportId = m_pendingMeasurementViewportId;
    const auto viewportSnapshot = m_pendingMeasurementViewports.constFind(continuationViewportId);
    const QPointer<ViewportWidget> continuationViewport = viewportSnapshot == m_pendingMeasurementViewports.cend()
                                                              ? QPointer<ViewportWidget>()
                                                              : viewportSnapshot->viewport;
    const quint64 continuationViewportGeneration =
        viewportSnapshot == m_pendingMeasurementViewports.cend() ? 0 : viewportSnapshot->contentGeneration;
    QTimer::singleShot(
        0, this,
        [this, completedProjectIdentity, completedTargetGeneration, completedTargetSnapshot, continuationGeneration,
         continuationViewport, continuationViewportId, continuationViewportGeneration, instanceId]() {
            if (continuationGeneration != m_measurementPickContinuationGeneration) {
                return;
            }
            if (!m_isRunning) {
                return;
            }
            if (!continuationViewportId.isEmpty() &&
                (!continuationViewport || !m_displayManager ||
                 m_displayManager->viewport(continuationViewportId) != continuationViewport.data() ||
                 continuationViewport->contentGeneration() != continuationViewportGeneration)) {
                onStop();
                return;
            }
            Project* currentProject = ProjectManager::instance().currentProject();
            const ModuleInstance* currentTarget = currentProject ? currentProject->findModule(instanceId) : nullptr;
            if (!currentProject || currentProject->runtimeIdentity() != completedProjectIdentity || !currentTarget ||
                currentProject->parameterInstanceGeneration(instanceId) != completedTargetGeneration ||
                currentTarget->toJson() != completedTargetSnapshot) {
                onStop();
                return;
            }
            m_pendingMeasurementViewportId.clear();
            m_pendingMeasurementViewports.clear();
            if (requestMeasurementInputForRun()) {
                return;
            }
            if (m_isCycleMode && !m_pendingMeasurementCycleSessionId.isEmpty()) {
                const QString sessionId = m_pendingMeasurementCycleSessionId;
                m_pendingMeasurementCycleSessionId.clear();
                RunEngine& engine = RunEngine::instance();
                if (!engine.refreshCycleProjectSnapshot(sessionId, ProjectManager::instance().currentProject())) {
                    Logger::instance().warning(tr("测量拾取后无法刷新循环工程快照，已停止循环"), "Run");
                    engine.stop();
                    return;
                }
                engine.continueDeferredCycle(sessionId);
                return;
            }
            executeFlowOnce();
        });
}

void MainWindow::onPoint2DPicked(const QPointF& point) {
    // 快速测量模式
    if (m_quickMeasureActive) {
        m_quickMeasurePoints.append(point);

        // 构建当前已放置点的标签
        auto buildOverlay = [&](const QList<MeasurementOverlayPoint>& pts, const QList<MeasurementOverlayLine>& lines) {
            for (ViewportWidget* vp : m_displayManager->allViewports()) {
                if (m_agentQuickMeasurement.status == QLatin1String("awaiting_user_input") && vp &&
                    vp->viewportId() != m_agentQuickMeasurement.viewportId)
                    continue;
                if (HImageWidget* iw = vp ? vp->imageWidget() : nullptr) {
                    if (iw->hasImage())
                        iw->setMeasurementOverlay(pts, lines);
                }
            }
        };

        auto ptLabel = [](int idx) { return QStringLiteral("P%1").arg(idx + 1); };

        // ---- 两点距离 (2 clicks) ----
        if (m_quickMeasureType == 0) {
            if (m_quickMeasurePoints.size() == 1) {
                buildOverlay({{m_quickMeasurePoints[0], ptLabel(0)}}, {});
            } else if (m_quickMeasurePoints.size() >= 2) {
                const QPointF& a = m_quickMeasurePoints[0];
                const QPointF& b = m_quickMeasurePoints[1];
                double dist = QLineF(a, b).length();
                QString label = tr("距离: %1").arg(dist, 0, 'f', 3);
                Logger::instance().info(tr("快速测量: %1 → %2 = %3").arg(pointText2D(a)).arg(pointText2D(b)).arg(label),
                                        "Measurement");
                buildOverlay({{a, QStringLiteral("P1")}, {b, QStringLiteral("P2")}}, {{a, b, label}});
                m_quickMeasurePoints.clear();
            }
        }
        // ---- 点到线距离 (3 clicks: point + 2 line endpoints) ----
        else if (m_quickMeasureType == 1) {
            int n = m_quickMeasurePoints.size();
            if (n < 3) {
                QList<MeasurementOverlayPoint> pts;
                for (int i = 0; i < n; ++i)
                    pts.append({m_quickMeasurePoints[i], ptLabel(i)});
                buildOverlay(pts, {});
            } else {
                const QPointF& p = m_quickMeasurePoints[0];
                const QPointF& a = m_quickMeasurePoints[1];
                const QPointF& b = m_quickMeasurePoints[2];
                QPointF ab = b - a;
                QPointF ap = p - a;
                qreal t = QPointF::dotProduct(ap, ab) / QPointF::dotProduct(ab, ab);
                t = qBound(0.0, t, 1.0);
                QPointF foot = a + t * ab;
                double dist = QLineF(p, foot).length();
                QString label = tr("距离: %1").arg(dist, 0, 'f', 3);
                Logger::instance().info(tr("快速测量: 点到线 = %1").arg(label), "Measurement");
                buildOverlay({{p, QStringLiteral("P")}, {a, QStringLiteral("L1")}, {b, QStringLiteral("L2")}},
                             {{a, b, QString()}, {p, foot, label}});
                m_quickMeasurePoints.clear();
            }
        }
        // ---- 两线间距 (4 clicks: line1 endpoints + line2 endpoints) ----
        else if (m_quickMeasureType == 2) {
            int n = m_quickMeasurePoints.size();
            if (n < 4) {
                QList<MeasurementOverlayPoint> pts;
                QList<MeasurementOverlayLine> lines;
                for (int i = 0; i < n; ++i) {
                    QString lbl = (i < 2) ? QStringLiteral("L1-%1").arg(i + 1) : QStringLiteral("L2-%1").arg(i - 1);
                    pts.append({m_quickMeasurePoints[i], lbl});
                }
                if (n >= 2)
                    lines.append({m_quickMeasurePoints[0], m_quickMeasurePoints[1], QString()});
                if (n >= 4)
                    lines.append({m_quickMeasurePoints[2], m_quickMeasurePoints[3], QString()});
                buildOverlay(pts, lines);
            } else {
                QLineF l1(m_quickMeasurePoints[0], m_quickMeasurePoints[1]);
                QLineF l2(m_quickMeasurePoints[2], m_quickMeasurePoints[3]);
                double len1 = l1.length();
                double len2 = l2.length();
                const MeasurementSegmentDistance3D closest = MeasurementData::closestPointsBetweenSegments(
                    {l1.p1().x(), l1.p1().y(), 0.0}, {l1.p2().x(), l1.p2().y(), 0.0}, {l2.p1().x(), l2.p1().y(), 0.0},
                    {l2.p2().x(), l2.p2().y(), 0.0});
                const QPointF closestOnLine1(closest.pointOnFirst.x, closest.pointOnFirst.y);
                const QPointF closestOnLine2(closest.pointOnSecond.x, closest.pointOnSecond.y);
                double dist = closest.distance;
                QString label = tr("间距: %1").arg(dist, 0, 'f', 3);
                Logger::instance().info(tr("快速测量: 两线段最近距离=%1 (线1长度=%2, 线2长度=%3)")
                                            .arg(dist, 0, 'f', 3)
                                            .arg(len1, 0, 'f', 3)
                                            .arg(len2, 0, 'f', 3),
                                        "Measurement");
                buildOverlay({{l1.p1(), QStringLiteral("L1-1")},
                              {l1.p2(), QStringLiteral("L1-2")},
                              {l2.p1(), QStringLiteral("L2-1")},
                              {l2.p2(), QStringLiteral("L2-2")}},
                             {{l1.p1(), l1.p2(), tr("线1: %1").arg(len1, 0, 'f', 3)},
                              {l2.p1(), l2.p2(), tr("线2: %1").arg(len2, 0, 'f', 3)},
                              {closestOnLine1, closestOnLine2, label}});
                m_quickMeasurePoints.clear();
            }
        }
        // ---- 三点角度 (3 clicks: vertex P1 + P2 + P3 → angle at P1) ----
        else if (m_quickMeasureType == 3) {
            int n = m_quickMeasurePoints.size();
            if (n < 3) {
                QList<MeasurementOverlayPoint> pts;
                for (int i = 0; i < n; ++i)
                    pts.append({m_quickMeasurePoints[i], ptLabel(i)});
                buildOverlay(pts, {});
            } else {
                const QPointF& v = m_quickMeasurePoints[0]; // vertex
                const QPointF& p2 = m_quickMeasurePoints[1];
                const QPointF& p3 = m_quickMeasurePoints[2];
                // Angle between v->p2 and v->p3
                QPointF d1 = p2 - v;
                QPointF d2 = p3 - v;
                double dot = QPointF::dotProduct(d1, d2);
                double cross = d1.x() * d2.y() - d1.y() * d2.x();
                double angleRad = atan2(std::abs(cross), dot);
                double angleDeg = angleRad * 180.0 / M_PI;
                QString label = tr("角度: %1°").arg(angleDeg, 0, 'f', 2);
                Logger::instance().info(tr("快速测量: 三点角度 = %1°").arg(angleDeg, 0, 'f', 2), "Measurement");
                buildOverlay({{v, QStringLiteral("V")}, {p2, QStringLiteral("P2")}, {p3, QStringLiteral("P3")}},
                             {{v, p2, QString()}, {v, p3, label}});
                m_quickMeasurePoints.clear();
            }
        }
        // ---- 三点拟合圆 (3 clicks → circle center, radius) ----
        else if (m_quickMeasureType == 4) {
            int n = m_quickMeasurePoints.size();
            if (n < 3) {
                QList<MeasurementOverlayPoint> pts;
                for (int i = 0; i < n; ++i)
                    pts.append({m_quickMeasurePoints[i], ptLabel(i)});
                buildOverlay(pts, {});
            } else {
                const QPointF& p1 = m_quickMeasurePoints[0];
                const QPointF& p2 = m_quickMeasurePoints[1];
                const QPointF& p3 = m_quickMeasurePoints[2];
                // 三点定圆: 圆心 = 外接圆心
                double ax = p1.x(), ay = p1.y();
                double bx = p2.x(), by = p2.y();
                double cx = p3.x(), cy = p3.y();
                double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
                double ux, uy;
                if (std::abs(d) < 1e-10) {
                    // 共线，无法拟合圆
                    Logger::instance().warning(tr("三点共线，无法拟合圆"), "Measurement");
                    buildOverlay({{p1, QStringLiteral("P1")}, {p2, QStringLiteral("P2")}, {p3, QStringLiteral("P3")}},
                                 {});
                    m_quickMeasurePoints.clear();
                } else {
                    ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) +
                          (cx * cx + cy * cy) * (ay - by)) /
                         d;
                    uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) +
                          (cx * cx + cy * cy) * (bx - ax)) /
                         d;
                    QPointF center(ux, uy);
                    double radius = QLineF(center, p1).length();
                    double circumference = 2.0 * M_PI * radius;
                    QString label = tr("R=%1, C=%2").arg(radius, 0, 'f', 2).arg(circumference, 0, 'f', 2);
                    Logger::instance().info(tr("快速测量: 拟合圆 半径=%1, 周长=%2, 圆心=(%3, %4)")
                                                .arg(radius, 0, 'f', 3)
                                                .arg(circumference, 0, 'f', 3)
                                                .arg(ux, 0, 'f', 1)
                                                .arg(uy, 0, 'f', 1),
                                            "Measurement");
                    // 用多段线近似圆
                    QList<MeasurementOverlayLine> circleLines;
                    const int segments = 64;
                    for (int i = 0; i < segments; ++i) {
                        double a1 = 2.0 * M_PI * i / segments;
                        double a2 = 2.0 * M_PI * (i + 1) / segments;
                        circleLines.append({{center.x() + radius * cos(a1), center.y() + radius * sin(a1)},
                                            {center.x() + radius * cos(a2), center.y() + radius * sin(a2)},
                                            (i == 0) ? label : QString()});
                    }
                    buildOverlay({{p1, QStringLiteral("P1")},
                                  {p2, QStringLiteral("P2")},
                                  {p3, QStringLiteral("P3")},
                                  {center, QStringLiteral("C")}},
                                 circleLines);
                    m_quickMeasurePoints.clear();
                }
            }
        }
        // type 5 (矩形面积) 不在此处理 — 使用 ROI 拖拽模式，见 onQuickMeasure
        return;
    }

    QString instanceId;
    IModule* mod = measurementInputForPicking(instanceId);
    if (!mod) {
        QGuiApplication::clipboard()->setText(pointText2D(point));
        Logger::instance().info(QString("2D pick: (%1, %2) — no active MeasurementInput")
                                    .arg(point.x(), 0, 'f', 2)
                                    .arg(point.y(), 0, 'f', 2),
                                "Picking");
        return;
    }
    if (RunEngine::instance().isBusy() && instanceId != m_pendingMeasurementInputId) {
        rejectBusyFlowEdit(tr("拾取测量输入"));
        return;
    }

    QJsonObject params = mod->currentParams();
    const QString mode = params["mode"].toString("point_pair");
    const int cursor = m_measurementPickCursor.value(instanceId, 0);
    Project* project = ProjectManager::instance().currentProject();

    const QJsonArray newPoint2D = pointArray2D(point);
    const QJsonArray newPoint3D = pointArray3D(point.x(), point.y(), 0.0);
    auto finishPick = [&]() { finishMeasurementPick(instanceId, mod->currentParams(), false); };

    if (mode == "point_set") {
        QJsonArray points = params["points"].toArray();
        points.append(newPoint2D);
        mod->setParam("points", points);
        if (project)
            project->setModuleParam(instanceId, "points", points);
        Logger::instance().info(QString("2D pick: point set p%1 set to (%2, %3)")
                                    .arg(points.size())
                                    .arg(point.x(), 0, 'f', 2)
                                    .arg(point.y(), 0, 'f', 2),
                                "Picking");
        m_measurementPickCursor[instanceId] = points.size();
        finishPick();
        return;
    }

    if (mode == "line_pair") {
        const int step = cursor % 4;
        const QString key = step < 2 ? QStringLiteral("line1") : QStringLiteral("line2");
        const bool firstPoint = (step % 2) == 0;
        QJsonArray line = updatedLineArray(params[key].toArray(), point, firstPoint);
        mod->setParam(key, line);
        if (project)
            project->setModuleParam(instanceId, key, line);
        Logger::instance().info(QString("2D pick: %1 p%2 set to (%3, %4)")
                                    .arg(key)
                                    .arg(firstPoint ? 1 : 2)
                                    .arg(point.x(), 0, 'f', 2)
                                    .arg(point.y(), 0, 'f', 2),
                                "Picking");
        mod->setParam("pick_coordinate_frame", QStringLiteral("ImageLocalPixel"));
        if (project)
            project->setModuleParam(instanceId, "pick_coordinate_frame", QStringLiteral("ImageLocalPixel"));
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    if (mode == "point_line") {
        const int step = cursor % 3;
        if (step == 0) {
            mod->setParam("point", newPoint2D);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint2D);
            Logger::instance().info(
                QString("2D pick: point set to (%1, %2)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2),
                "Picking");
        } else {
            QJsonArray line = updatedLineArray(params["line"].toArray(), point, step == 1);
            mod->setParam("line", line);
            if (project)
                project->setModuleParam(instanceId, "line", line);
            Logger::instance().info(QString("2D pick: line p%1 set to (%2, %3)")
                                        .arg(step)
                                        .arg(point.x(), 0, 'f', 2)
                                        .arg(point.y(), 0, 'f', 2),
                                    "Picking");
        }
        mod->setParam("pick_coordinate_frame", QStringLiteral("ImageLocalPixel"));
        if (project)
            project->setModuleParam(instanceId, "pick_coordinate_frame", QStringLiteral("ImageLocalPixel"));
        m_measurementPickCursor[instanceId] = (step + 1) % 3;
        finishPick();
        return;
    }

    if (mode == "point_plane") {
        const int step = cursor % 4;
        if (step == 0) {
            mod->setParam("point", newPoint3D);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint3D);
            Logger::instance().info(
                QString("2D pick: point set to (%1, %2, 0)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2),
                "Picking");
        } else {
            QJsonArray plane = updatedPlaneArray(params["plane"].toArray(), newPoint3D, step - 1);
            mod->setParam("plane", plane);
            if (project)
                project->setModuleParam(instanceId, "plane", plane);
            Logger::instance().info(QString("2D pick: plane p%1 set to (%2, %3, 0)")
                                        .arg(step)
                                        .arg(point.x(), 0, 'f', 2)
                                        .arg(point.y(), 0, 'f', 2),
                                    "Picking");
        }
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    const bool setFirst = (cursor % 2) == 0;
    if (setFirst) {
        mod->setParam("point1", newPoint2D);
        if (project)
            project->setModuleParam(instanceId, "point1", newPoint2D);
        Logger::instance().info(
            QString("2D pick: point1 set to (%1, %2)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2), "Picking");
    } else {
        mod->setParam("point2", newPoint2D);
        if (project)
            project->setModuleParam(instanceId, "point2", newPoint2D);
        Logger::instance().info(
            QString("2D pick: point2 set to (%1, %2)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2), "Picking");
    }
    m_measurementPickCursor[instanceId] = (cursor + 1) % 2;
    finishPick();
}

void MainWindow::refreshMeasurementOverlay(const QJsonObject& params, int visibleSteps) {
    if (!m_displayManager) {
        return;
    }

    QList<MeasurementOverlayPoint> points;
    QList<MeasurementOverlayLine> lines;
    const QString mode = params["mode"].toString("point_pair");

    auto addPoint = [&](const QJsonArray& arr, const QString& label, int minStep, int offset = 0) {
        if (visibleSteps < minStep || arr.size() < offset + 2) {
            return;
        }
        points.append(MeasurementOverlayPoint{pointFromArray2D(arr, offset), label});
    };

    auto addLine = [&](const QJsonArray& arr, const QString& label, int minStep) {
        if (visibleSteps < minStep || arr.size() < 4) {
            return;
        }
        lines.append(MeasurementOverlayLine{pointFromArray2D(arr, 0), pointFromArray2D(arr, 2), label});
    };

    if (mode == QStringLiteral("point_set")) {
        const QJsonArray pointSet = params["points"].toArray();
        for (int i = 0; i < pointSet.size() && i < visibleSteps; ++i) {
            addPoint(pointSet.at(i).toArray(), QStringLiteral("P%1").arg(i + 1), i + 1);
        }
    } else if (mode == QStringLiteral("point_line")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray line = params["line"].toArray();
        addPoint(point, QStringLiteral("P"), 1);
        addPoint(line, QStringLiteral("L1"), 2, 0);
        addPoint(line, QStringLiteral("L2"), 3, 2);
        addLine(line, QStringLiteral("线段"), 3);
    } else if (mode == QStringLiteral("line_pair")) {
        const QJsonArray line1 = params["line1"].toArray();
        const QJsonArray line2 = params["line2"].toArray();
        addPoint(line1, QStringLiteral("L1-1"), 1, 0);
        addPoint(line1, QStringLiteral("L1-2"), 2, 2);
        addLine(line1, QStringLiteral("线1"), 2);
        addPoint(line2, QStringLiteral("L2-1"), 3, 0);
        addPoint(line2, QStringLiteral("L2-2"), 4, 2);
        addLine(line2, QStringLiteral("线2"), 4);
    } else if (mode == QStringLiteral("point_plane")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray plane = params["plane"].toArray();
        addPoint(point, QStringLiteral("P"), 1);
        addPoint(plane, QStringLiteral("A"), 2, 0);
        addPoint(plane, QStringLiteral("B"), 3, 3);
        addPoint(plane, QStringLiteral("C"), 4, 6);
        if (visibleSteps >= 4 && plane.size() >= 9) {
            lines.append(MeasurementOverlayLine{pointFromArray2D(plane, 0), pointFromArray2D(plane, 3),
                                                QStringLiteral("平面边")});
            lines.append(MeasurementOverlayLine{pointFromArray2D(plane, 3), pointFromArray2D(plane, 6), QString()});
            lines.append(MeasurementOverlayLine{pointFromArray2D(plane, 6), pointFromArray2D(plane, 0), QString()});
        }
    } else {
        const QJsonArray point1 = params["point1"].toArray();
        const QJsonArray point2 = params["point2"].toArray();
        addPoint(point1, QStringLiteral("P1"), 1);
        addPoint(point2, QStringLiteral("P2"), 2);
        if (visibleSteps >= 2 && point1.size() >= 2 && point2.size() >= 2) {
            lines.append(
                MeasurementOverlayLine{pointFromArray2D(point1), pointFromArray2D(point2), QStringLiteral("P1-P2")});
        }
    }

    // 拾取阶段叠加画在正在拾取的图像上：仅当含图像的视口唯一（无歧义）时绘制，
    // 不使用"最近一次显示"的全局值，避免误画到无关视口。
    QList<HImageWidget*> imageWidgets;
    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        if (viewport && viewport->imageWidget() && viewport->imageWidget()->hasImage())
            imageWidgets.append(viewport->imageWidget());
    }
    if (imageWidgets.size() == 1)
        imageWidgets.first()->setMeasurementOverlay(points, lines);
}

void MainWindow::updateMeasurementResultOnOverlay() {
    if (!m_displayManager) {
        return;
    }

    // 后台执行（设计 §7.3）：测量叠加只消费当前 DisplayFrame.moduleDataSnapshot，
    // 禁止回读 RunEngine::moduleOutput() 的实时输出（避免 A 帧图像搭配 B 轮结果）。
    if (!m_hasDisplayFrame || !m_latestDisplayFrame.hasSnapshot()) {
        clearMeasurementOverlays();
        return;
    }
    const QMap<QString, QVariantMap>& snapshot = m_latestDisplayFrame.moduleDataSnapshot;

    // 以项目模型为实例身份权威来源，插件运行实例的名称字段可能为空。
    // 无法归属/无测量时统一清除叠加（含 3D 视口），避免旧支路叠加残留。
    Project* currentProject = ProjectManager::instance().currentProject();
    if (!currentProject) {
        clearMeasurementOverlays();
        return;
    }

    QList<ModuleInstance> measurementInputs;
    for (const ModuleInstance& instance : currentProject->modules()) {
        const PluginInfo info = PluginManager::instance().pluginInfo(instance.moduleId);
        if (instance.moduleId.compare(QStringLiteral("MeasurementInput"), Qt::CaseInsensitive) == 0 ||
            info.id.compare(QStringLiteral("com.deeplux.plugin.measurementinput"), Qt::CaseInsensitive) == 0) {
            measurementInputs.append(instance);
        }
    }
    if (measurementInputs.isEmpty()) {
        clearMeasurementOverlays();
        return;
    }

    // 某测量输入的全部下游模块（含直接/间接连接，不含自身）
    auto downstreamOf = [&currentProject](const QString& inputId) {
        QSet<QString> visited{inputId};
        QStringList queue{inputId};
        QStringList result;
        while (!queue.isEmpty()) {
            const QString current = queue.takeFirst();
            for (const ModuleConnection& connection : currentProject->connections()) {
                if (connection.fromModuleId != current || visited.contains(connection.toModuleId)) {
                    continue;
                }
                visited.insert(connection.toModuleId);
                queue.append(connection.toModuleId);
                result.append(connection.toModuleId);
            }
        }
        return result;
    };

    // 拥有目标节点（位于其支路内）的测量输入集合；>1 表示归属不唯一
    auto owningInputs = [&](const QString& moduleId) {
        QList<const ModuleInstance*> owners;
        if (moduleId.isEmpty())
            return owners;
        for (const ModuleInstance& input : measurementInputs) {
            if (input.id == moduleId || downstreamOf(input.id).contains(moduleId))
                owners.append(&input);
        }
        return owners;
    };

    // 多测量支路时不得固定取第一个输入，否则会把流程 A 的点与流程 B 的结果组合。
    // 归属必须唯一：当前选中节点所属支路 > 最近执行节点所属支路 > 唯一输入；
    // 归属不唯一（多个测量输入汇入同一节点）或选中节点不属于任何测量支路时，
    // 清除叠加并返回，避免错配或旧支路叠加残留。
    // preferredModule：确定支路来源的具体节点（选中节点 / 最近执行节点），
    // 其自身输出的结果优先于下游 BFS 命中（见下方结果选取规则）。
    const ModuleInstance* chosen = nullptr;
    QString preferredModuleId;
    if (!m_selectedModuleId.isEmpty()) {
        const QList<const ModuleInstance*> owners = owningInputs(m_selectedModuleId);
        if (owners.size() > 1) {
            clearMeasurementOverlays();
            return; // 归属不唯一，不绘制
        }
        if (owners.size() == 1) {
            chosen = owners.first();
            preferredModuleId = m_selectedModuleId;
        } else {
            clearMeasurementOverlays();
            return; // 选中节点不属于任何测量支路，清除叠加，不回退全局最近输出
        }
    }
    if (!chosen) {
        const QString lastOutputModule = m_latestDisplayFrame.moduleId;
        const QList<const ModuleInstance*> owners = owningInputs(lastOutputModule);
        if (owners.size() > 1) {
            clearMeasurementOverlays();
            return; // 归属不唯一，不绘制
        }
        if (owners.size() == 1) {
            chosen = owners.first();
            preferredModuleId = lastOutputModule;
        }
    }
    if (!chosen && measurementInputs.size() == 1) {
        chosen = &measurementInputs.first();
    }
    if (!chosen) {
        clearMeasurementOverlays();
        return;
    }

    const QJsonObject inputParams = chosen->params;

    const QString mode = inputParams["mode"].toString("point_pair");
    auto resultMatches = [&mode](const QVariantMap& candidate) {
        const bool matchesPointSet = mode == QStringLiteral("point_set") && candidate.contains("circle_radius");
        const bool matchesDistance = mode != QStringLiteral("point_set") &&
                                     (candidate.contains("distance") || candidate.contains("gap_distance"));
        return matchesPointSet || matchesDistance;
    };

    // 结果选取规则：
    // 1) 选中了测量输入以外的具体模块时，只显示该模块自身输出的测量值——
    //    点哪个结果模块就显示哪个，不得按 BFS 顺序命中同支路的其它结果模块；
    //    该模块自身无匹配结果时不绘制结果（不回退下游搜索）。
    // 2) 选中的是测量输入节点（或无选择而回退）时，优先采用参照模块
    //    （选中/最近执行节点）自身输出，其次沿所选支路整个下游链搜索；
    //    不回退全局 lastOutput()（避免混入另一支路结果）；
    //    共享下游（同时属于其它测量输入支路）的结果归属不唯一，跳过不采用。
    QVariantMap results;
    QString resultModuleId;

    auto tryAdoptOwnOutput = [&](const QString& moduleId) {
        const QVariantMap own = snapshot.value(moduleId);
        if (resultMatches(own)) {
            results = own;
            resultModuleId = moduleId;
            return true;
        }
        return false;
    };

    const bool selectedInputNode = m_selectedModuleId.isEmpty() || m_selectedModuleId == chosen->id;
    if (!selectedInputNode) {
        // 选中具体结果模块：只显示其自身输出。未运行/执行失败/无匹配输出时
        // 清除叠加并返回——不得落入下方按输入点几何推算的"伪测量值"。
        if (!tryAdoptOwnOutput(m_selectedModuleId)) {
            clearMeasurementOverlays();
            return;
        }
    } else if (preferredModuleId.isEmpty() || !tryAdoptOwnOutput(preferredModuleId)) {
        for (const QString& moduleId : downstreamOf(chosen->id)) {
            const QVariantMap candidate = snapshot.value(moduleId);
            if (!resultMatches(candidate))
                continue;
            if (owningInputs(moduleId).size() > 1)
                continue; // 结果节点同时属于其它测量支路，归属不唯一，跳过
            results = candidate;
            resultModuleId = moduleId;
            break;
        }
    }

    auto resultValue = [&](const QStringList& keys) -> double {
        for (const QString& key : keys) {
            if (results.contains(key)) {
                return results[key].toDouble();
            }
        }
        return -1.0;
    };

    auto fmtDist = [](double v) { return QString::fromUtf8("距离: %1").arg(v, 0, 'f', 3); };

    QList<MeasurementOverlayPoint> points;
    QList<MeasurementOverlayLine> lines;

    if (mode == QStringLiteral("point_pair")) {
        const QJsonArray p1 = inputParams["point1"].toArray();
        const QJsonArray p2 = inputParams["point2"].toArray();
        if (p1.size() >= 2)
            points.append({pointFromArray2D(p1), QStringLiteral("P1")});
        if (p2.size() >= 2)
            points.append({pointFromArray2D(p2), QStringLiteral("P2")});
        if (p1.size() >= 2 && p2.size() >= 2) {
            double dist = resultValue({"distance", "gap_distance"});
            if (dist < 0) {
                double dx = p2[0].toDouble() - p1[0].toDouble();
                double dy = p2[1].toDouble() - p1[1].toDouble();
                dist = sqrt(dx * dx + dy * dy);
            }
            lines.append({pointFromArray2D(p1), pointFromArray2D(p2), fmtDist(dist)});
        }
    } else if (mode == QStringLiteral("point_set")) {
        const QJsonArray pointSet = inputParams["points"].toArray();
        for (int i = 0; i < pointSet.size(); ++i) {
            const QJsonArray point = pointSet.at(i).toArray();
            if (point.size() >= 2) {
                points.append({pointFromArray2D(point), QStringLiteral("P%1").arg(i + 1)});
            }
        }

        const double centerX = resultValue({"circle_center_x"});
        const double centerY = resultValue({"circle_center_y"});
        const double radius = resultValue({"circle_radius"});
        if (radius > 0.0) {
            constexpr int segments = 64;
            for (int i = 0; i < segments; ++i) {
                const double angle1 = 2.0 * M_PI * i / segments;
                const double angle2 = 2.0 * M_PI * (i + 1) / segments;
                lines.append({QPointF(centerX + radius * std::cos(angle1), centerY + radius * std::sin(angle1)),
                              QPointF(centerX + radius * std::cos(angle2), centerY + radius * std::sin(angle2)),
                              i == segments * 7 / 8 ? QStringLiteral("R: %1 px").arg(radius, 0, 'f', 1) : QString()});
            }
        }
    } else if (mode == QStringLiteral("point_line")) {
        const QJsonArray pt = inputParams["point"].toArray();
        const QJsonArray ln = inputParams["line"].toArray();
        if (pt.size() >= 2)
            points.append({pointFromArray2D(pt), QStringLiteral("P")});
        if (ln.size() >= 4) {
            points.append({pointFromArray2D(ln), QStringLiteral("L1")});
            points.append({pointFromArray2D(ln, 2), QStringLiteral("L2")});
            double dist = resultValue({"distance"});
            if (dist < 0) {
                // 点到线段距离
                QPointF p = pointFromArray2D(pt);
                QPointF a = pointFromArray2D(ln);
                QPointF b = pointFromArray2D(ln, 2);
                QPointF ab = b - a;
                QPointF ap = p - a;
                double t = QPointF::dotProduct(ap, ab) / QPointF::dotProduct(ab, ab);
                t = qBound(0.0, t, 1.0);
                QPointF foot = a + t * ab;
                dist = QLineF(p, foot).length();
            }
            lines.append({pointFromArray2D(ln), pointFromArray2D(ln, 2), fmtDist(dist)});
        }
    } else if (mode == QStringLiteral("line_pair")) {
        const QJsonArray l1 = inputParams["line1"].toArray();
        const QJsonArray l2 = inputParams["line2"].toArray();
        if (l1.size() >= 4) {
            points.append({pointFromArray2D(l1), QStringLiteral("L1-1")});
            points.append({pointFromArray2D(l1, 2), QStringLiteral("L1-2")});
            double len = QLineF(pointFromArray2D(l1), pointFromArray2D(l1, 2)).length();
            lines.append(
                {pointFromArray2D(l1), pointFromArray2D(l1, 2), QString::fromUtf8("线1: %1").arg(len, 0, 'f', 3)});
        }
        if (l2.size() >= 4) {
            points.append({pointFromArray2D(l2), QStringLiteral("L2-1")});
            points.append({pointFromArray2D(l2, 2), QStringLiteral("L2-2")});
            double len = QLineF(pointFromArray2D(l2), pointFromArray2D(l2, 2)).length();
            lines.append(
                {pointFromArray2D(l2), pointFromArray2D(l2, 2), QString::fromUtf8("线2: %1").arg(len, 0, 'f', 3)});
        }
        if (l1.size() >= 4 && l2.size() >= 4) {
            double dist = resultValue({"distance"});
            if (dist >= 0) {
                const QVariant nearestSegmentValue = results.value(QStringLiteral("nearest_segment"));
                const QVariantList nearestSegment = nearestSegmentValue.toList();
                if (nearestSegmentValue.isValid()) {
                    if (nearestSegment.size() == 4 &&
                        std::all_of(nearestSegment.cbegin(), nearestSegment.cend(), [](const QVariant& value) {
                            return value.canConvert<double>() && std::isfinite(value.toDouble());
                        })) {
                        lines.append({QPointF(nearestSegment[0].toDouble(), nearestSegment[1].toDouble()),
                                      QPointF(nearestSegment[2].toDouble(), nearestSegment[3].toDouble()),
                                      fmtDist(dist)});
                    }
                } else {
                    lines.append({(pointFromArray2D(l1) + pointFromArray2D(l1, 2)) / 2.0,
                                  (pointFromArray2D(l2) + pointFromArray2D(l2, 2)) / 2.0, fmtDist(dist)});
                }
            }
        }
    } else if (mode == QStringLiteral("point_plane")) {
        const QJsonArray pt = inputParams["point"].toArray();
        const QJsonArray pl = inputParams["plane"].toArray();
        if (pt.size() >= 2)
            points.append({pointFromArray2D(pt), QStringLiteral("P")});
        if (pl.size() >= 9) {
            points.append({pointFromArray2D(pl), QStringLiteral("A")});
            points.append({pointFromArray2D(pl, 3), QStringLiteral("B")});
            points.append({pointFromArray2D(pl, 6), QStringLiteral("C")});
            lines.append({pointFromArray2D(pl), pointFromArray2D(pl, 3), QString()});
            lines.append({pointFromArray2D(pl, 3), pointFromArray2D(pl, 6), QString()});
            lines.append({pointFromArray2D(pl, 6), pointFromArray2D(pl), QString()});
        }
        if (pt.size() >= 2 && pl.size() >= 9) {
            double dist = resultValue({"distance"});
            lines.append({pointFromArray2D(pt), pointFromArray2D(pl), dist >= 0 ? fmtDist(dist) : QString()});
        }
    }

    // 视口按模块身份绑定：叠加画在"产生测量结果的模块"所显示的视口上，
    // 不使用"最近一次显示"的全局值，避免画到无关支路的视口。
    const QString bindModuleId = resultModuleId.isEmpty() ? chosen->id : resultModuleId;
    const QString boundViewportId = m_moduleViewportIds.value(bindModuleId);
    ViewportWidget* targetViewport = boundViewportId.isEmpty() ? nullptr : m_displayManager->viewport(boundViewportId);
    HImageWidget* targetImageWidget = targetViewport ? targetViewport->imageWidget() : nullptr;
    if (targetImageWidget && targetImageWidget->hasImage()) {
        targetImageWidget->setMeasurementOverlay(points, lines);
        return;
    }
    // 兜底：仅当含图像的视口唯一（无歧义）时绘制，避免误画到多支路的无关视口。
    QList<HImageWidget*> imageWidgets;
    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        HImageWidget* imageWidget = viewport ? viewport->imageWidget() : nullptr;
        if (imageWidget && imageWidget->hasImage())
            imageWidgets.append(imageWidget);
    }
    if (imageWidgets.size() == 1)
        imageWidgets.first()->setMeasurementOverlay(points, lines);
}

void MainWindow::refreshMeasurementOverlay3D(const QJsonObject& params, int visibleSteps) {
    if (!m_displayManager) {
        return;
    }

    QList<MeasurementOverlayPoint3D> points;
    QList<MeasurementOverlayLine3D> lines;
    const QString mode = params["mode"].toString("point_pair");

    auto addPoint3D = [&](const QJsonArray& arr, const QString& label, int minStep, int offset = 0) {
        if (visibleSteps < minStep || arr.size() < offset + 2) {
            return;
        }
        points.append(MeasurementOverlayPoint3D{pointFromArray3D(arr, offset), label});
    };

    auto addLine2D = [&](const QJsonArray& arr, const QString& label, int minStep) {
        if (visibleSteps < minStep || arr.size() < 4) {
            return;
        }
        lines.append(MeasurementOverlayLine3D{linePointFromArray3D(arr, 0), linePointFromArray3D(arr, 2), label});
    };

    if (mode == QStringLiteral("point_line")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray line = params["line"].toArray();
        addPoint3D(point, QStringLiteral("P"), 1);
        if (visibleSteps >= 2 && line.size() >= 2) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line, 0), QStringLiteral("L1")});
        }
        if (visibleSteps >= 3 && line.size() >= 4) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line, 2), QStringLiteral("L2")});
        }
        addLine2D(line, QStringLiteral("线段(z=0)"), 3);
    } else if (mode == QStringLiteral("line_pair")) {
        const QJsonArray line1 = params["line1"].toArray();
        const QJsonArray line2 = params["line2"].toArray();
        if (visibleSteps >= 1 && line1.size() >= 2) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line1, 0), QStringLiteral("L1-1")});
        }
        if (visibleSteps >= 2 && line1.size() >= 4) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line1, 2), QStringLiteral("L1-2")});
        }
        addLine2D(line1, QStringLiteral("线1(z=0)"), 2);
        if (visibleSteps >= 3 && line2.size() >= 2) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line2, 0), QStringLiteral("L2-1")});
        }
        if (visibleSteps >= 4 && line2.size() >= 4) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line2, 2), QStringLiteral("L2-2")});
        }
        addLine2D(line2, QStringLiteral("线2(z=0)"), 4);
    } else if (mode == QStringLiteral("point_plane")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray plane = params["plane"].toArray();
        addPoint3D(point, QStringLiteral("P"), 1);
        addPoint3D(plane, QStringLiteral("A"), 2, 0);
        addPoint3D(plane, QStringLiteral("B"), 3, 3);
        addPoint3D(plane, QStringLiteral("C"), 4, 6);
        if (visibleSteps >= 4 && plane.size() >= 9) {
            lines.append(MeasurementOverlayLine3D{pointFromArray3D(plane, 0), pointFromArray3D(plane, 3),
                                                  QStringLiteral("平面边")});
            lines.append(MeasurementOverlayLine3D{pointFromArray3D(plane, 3), pointFromArray3D(plane, 6), QString()});
            lines.append(MeasurementOverlayLine3D{pointFromArray3D(plane, 6), pointFromArray3D(plane, 0), QString()});
        }
    } else {
        const QJsonArray point1 = params["point1"].toArray();
        const QJsonArray point2 = params["point2"].toArray();
        addPoint3D(point1, QStringLiteral("P1"), 1);
        addPoint3D(point2, QStringLiteral("P2"), 2);
        if (visibleSteps >= 2 && point1.size() >= 2 && point2.size() >= 2) {
            lines.append(
                MeasurementOverlayLine3D{pointFromArray3D(point1), pointFromArray3D(point2), QStringLiteral("P1-P2")});
        }
    }

    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        Viewport3DContent* content = viewport ? viewport->viewport3D() : nullptr;
        if (content) {
            content->setMeasurementOverlay(points, lines);
        }
    }
}

void MainWindow::clearMeasurementOverlays() {
    if (!m_displayManager) {
        return;
    }

    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        if (!viewport) {
            continue;
        }
        if (HImageWidget* imageWidget = viewport->imageWidget()) {
            imageWidget->clearMeasurementOverlay();
        }
        if (Viewport3DContent* content = viewport->viewport3D()) {
            content->clearMeasurementOverlay();
        }
    }
}

void MainWindow::onPoint3DPicked(const QVector3D& point) {
    handlePoint3DPicked(point.x(), point.y(), point.z());
}

void MainWindow::handlePoint3DPicked(double x, double y, double z) {
    const Eigen::Vector3d point(x, y, z);
    // 3D 快速测量模式
    if (m_quickMeasureActive) {
        m_quickMeasurePoints3D.append(point);

        const auto overlayPoint = [](const Eigen::Vector3d& p, const QString& label) {
            return MeasurementOverlayPoint3D{p, label};
        };
        const auto overlayLine = [](const Eigen::Vector3d& p1, const Eigen::Vector3d& p2, const QString& label) {
            return MeasurementOverlayLine3D{p1, p2, label};
        };

        auto buildOverlay3D = [&](const QList<MeasurementOverlayPoint3D>& pts,
                                  const QList<MeasurementOverlayLine3D>& lines) {
            for (ViewportWidget* vp : m_displayManager->allViewports()) {
                if (m_agentQuickMeasurement.status == QLatin1String("awaiting_user_input") && vp &&
                    vp->viewportId() != m_agentQuickMeasurement.viewportId)
                    continue;
                if (Viewport3DContent* c = vp ? vp->viewport3D() : nullptr) {
                    c->setMeasurementOverlay(pts, lines);
                }
            }
        };

        auto ptLabel = [](int idx) { return QStringLiteral("P%1").arg(idx + 1); };

        if (m_quickMeasureType == 0) {
            // 两点距离 (3D)
            if (m_quickMeasurePoints3D.size() == 1) {
                buildOverlay3D({overlayPoint(m_quickMeasurePoints3D[0], ptLabel(0))}, {});
            } else if (m_quickMeasurePoints3D.size() >= 2) {
                const Eigen::Vector3d& a = m_quickMeasurePoints3D[0];
                const Eigen::Vector3d& b = m_quickMeasurePoints3D[1];
                const double dist = (b - a).norm();
                double dx = std::abs(b.x() - a.x());
                double dy = std::abs(b.y() - a.y());
                double dz = std::abs(b.z() - a.z());
                QString label = tr("距离: %1").arg(dist, 0, 'f', 3);
                Logger::instance().info(
                    tr("3D 快速测量: %1 → %2 = %3").arg(pointText3D(a)).arg(pointText3D(b)).arg(label), "Measurement");
                buildOverlay3D({overlayPoint(a, QStringLiteral("P1")), overlayPoint(b, QStringLiteral("P2"))},
                               {overlayLine(a, b, label)});
                // 方案 A: 视口右上角信息面板，显示坐标与 ΔX/ΔY/ΔZ 分解
                QStringList infoLines;
                infoLines << tr("3D 距离测量");
                infoLines << tr("P1 (%1, %2, %3)").arg(a.x(), 0, 'f', 2).arg(a.y(), 0, 'f', 2).arg(a.z(), 0, 'f', 2);
                infoLines << tr("P2 (%1, %2, %3)").arg(b.x(), 0, 'f', 2).arg(b.y(), 0, 'f', 2).arg(b.z(), 0, 'f', 2);
                infoLines << QStringLiteral("────────────");
                infoLines << tr("ΔX: %1").arg(dx, 0, 'f', 3);
                infoLines << tr("ΔY: %1").arg(dy, 0, 'f', 3);
                infoLines << tr("ΔZ: %1").arg(dz, 0, 'f', 3);
                infoLines << tr("距离: %1").arg(dist, 0, 'f', 3);
                for (ViewportWidget* vp : m_displayManager->allViewports()) {
                    if (m_agentQuickMeasurement.status == QLatin1String("awaiting_user_input") && vp &&
                        vp->viewportId() != m_agentQuickMeasurement.viewportId)
                        continue;
                    if (Viewport3DContent* c = vp ? vp->viewport3D() : nullptr) {
                        c->setMeasurementInfoPanel(infoLines);
                    }
                }
                m_quickMeasurePoints3D.clear();
            }
        } else if (m_quickMeasureType == 1) {
            // 点到线距离 (3D: 3 picks)
            int n = m_quickMeasurePoints3D.size();
            if (n < 3) {
                QList<MeasurementOverlayPoint3D> pts;
                for (int i = 0; i < n; ++i)
                    pts.append(overlayPoint(m_quickMeasurePoints3D[i], ptLabel(i)));
                buildOverlay3D(pts, {});
            } else {
                const Eigen::Vector3d& p = m_quickMeasurePoints3D[0];
                const Eigen::Vector3d& a = m_quickMeasurePoints3D[1];
                const Eigen::Vector3d& b = m_quickMeasurePoints3D[2];
                const Eigen::Vector3d ab = b - a;
                const double abSquaredNorm = ab.squaredNorm();
                const double t = abSquaredNorm > 0.0 ? std::clamp((p - a).dot(ab) / abSquaredNorm, 0.0, 1.0) : 0.0;
                const Eigen::Vector3d foot = a + t * ab;
                const double dist = (p - foot).norm();
                QString label = tr("距离: %1").arg(dist, 0, 'f', 3);
                Logger::instance().info(tr("3D 快速测量: 点到线 = %1").arg(label), "Measurement");
                buildOverlay3D({overlayPoint(p, QStringLiteral("P")), overlayPoint(a, QStringLiteral("L1")),
                                overlayPoint(b, QStringLiteral("L2"))},
                               {overlayLine(a, b, QString()), overlayLine(p, foot, label)});
                m_quickMeasurePoints3D.clear();
            }
        } else if (m_quickMeasureType == 2) {
            // 两线间距 (3D: 4 picks)
            int n = m_quickMeasurePoints3D.size();
            if (n < 4) {
                QList<MeasurementOverlayPoint3D> pts;
                QList<MeasurementOverlayLine3D> lines;
                for (int i = 0; i < n; ++i) {
                    QString lbl = (i < 2) ? QStringLiteral("L1-%1").arg(i + 1) : QStringLiteral("L2-%1").arg(i - 1);
                    pts.append(overlayPoint(m_quickMeasurePoints3D[i], lbl));
                }
                if (n >= 2)
                    lines.append(overlayLine(m_quickMeasurePoints3D[0], m_quickMeasurePoints3D[1], QString()));
                if (n >= 4)
                    lines.append(overlayLine(m_quickMeasurePoints3D[2], m_quickMeasurePoints3D[3], QString()));
                buildOverlay3D(pts, lines);
            } else {
                const Eigen::Vector3d& a1 = m_quickMeasurePoints3D[0];
                const Eigen::Vector3d& a2 = m_quickMeasurePoints3D[1];
                const Eigen::Vector3d& b1 = m_quickMeasurePoints3D[2];
                const Eigen::Vector3d& b2 = m_quickMeasurePoints3D[3];
                const double len1 = (a2 - a1).norm();
                const double len2 = (b2 - b1).norm();
                const MeasurementSegmentDistance3D closest =
                    MeasurementData::closestPointsBetweenSegments({a1.x(), a1.y(), a1.z()}, {a2.x(), a2.y(), a2.z()},
                                                                  {b1.x(), b1.y(), b1.z()}, {b2.x(), b2.y(), b2.z()});
                const Eigen::Vector3d closestOnLine1(closest.pointOnFirst.x, closest.pointOnFirst.y,
                                                     closest.pointOnFirst.z);
                const Eigen::Vector3d closestOnLine2(closest.pointOnSecond.x, closest.pointOnSecond.y,
                                                     closest.pointOnSecond.z);
                const double dist = closest.distance;
                QString label = tr("间距: %1").arg(dist, 0, 'f', 3);
                Logger::instance().info(tr("3D 快速测量: 两线段最近距离=%1 (线1=%2, 线2=%3)")
                                            .arg(dist, 0, 'f', 3)
                                            .arg(len1, 0, 'f', 3)
                                            .arg(len2, 0, 'f', 3),
                                        "Measurement");
                buildOverlay3D({overlayPoint(a1, QStringLiteral("L1-1")), overlayPoint(a2, QStringLiteral("L1-2")),
                                overlayPoint(b1, QStringLiteral("L2-1")), overlayPoint(b2, QStringLiteral("L2-2"))},
                               {overlayLine(a1, a2, tr("线1: %1").arg(len1, 0, 'f', 3)),
                                overlayLine(b1, b2, tr("线2: %1").arg(len2, 0, 'f', 3)),
                                overlayLine(closestOnLine1, closestOnLine2, label)});
                m_quickMeasurePoints3D.clear();
            }
        }
        // ---- 三点角度 (3D: 3 clicks: vertex + 2 points → angle) ----
        else if (m_quickMeasureType == 3) {
            int n = m_quickMeasurePoints3D.size();
            if (n < 3) {
                QList<MeasurementOverlayPoint3D> pts;
                for (int i = 0; i < n; ++i)
                    pts.append(overlayPoint(m_quickMeasurePoints3D[i], ptLabel(i)));
                buildOverlay3D(pts, {});
            } else {
                const Eigen::Vector3d& v = m_quickMeasurePoints3D[0];
                const Eigen::Vector3d& p2 = m_quickMeasurePoints3D[1];
                const Eigen::Vector3d& p3 = m_quickMeasurePoints3D[2];
                const Eigen::Vector3d d1 = p2 - v;
                const Eigen::Vector3d d2 = p3 - v;
                const double dot = d1.dot(d2);
                const double len1 = d1.norm();
                const double len2 = d2.norm();
                const double angleRad = acos(qBound(-1.0, dot / (len1 * len2 + 1e-12), 1.0));
                double angleDeg = angleRad * 180.0 / M_PI;
                QString label = tr("角度: %1°").arg(angleDeg, 0, 'f', 2);
                Logger::instance().info(tr("3D 快速测量: 三点角度 = %1°").arg(angleDeg, 0, 'f', 2), "Measurement");
                buildOverlay3D({overlayPoint(v, QStringLiteral("V")), overlayPoint(p2, QStringLiteral("P2")),
                                overlayPoint(p3, QStringLiteral("P3"))},
                               {overlayLine(v, p2, QString()), overlayLine(v, p3, label)});
                m_quickMeasurePoints3D.clear();
            }
        }
        // ---- 三点拟合圆 (3D: 3 clicks → project to plane, fit circle) ----
        else if (m_quickMeasureType == 4) {
            int n = m_quickMeasurePoints3D.size();
            if (n < 3) {
                QList<MeasurementOverlayPoint3D> pts;
                for (int i = 0; i < n; ++i)
                    pts.append(overlayPoint(m_quickMeasurePoints3D[i], ptLabel(i)));
                buildOverlay3D(pts, {});
            } else {
                const Eigen::Vector3d& p1 = m_quickMeasurePoints3D[0];
                const Eigen::Vector3d& p2 = m_quickMeasurePoints3D[1];
                const Eigen::Vector3d& p3 = m_quickMeasurePoints3D[2];
                // 构建局部坐标系: normal = (p2-p1) × (p3-p1)
                Eigen::Vector3d u = p2 - p1;
                const Eigen::Vector3d w = u.cross(p3 - p1);
                if (w.norm() < 1e-6) {
                    Logger::instance().warning(tr("三点共线，无法拟合圆"), "Measurement");
                    buildOverlay3D({overlayPoint(p1, QStringLiteral("P1")), overlayPoint(p2, QStringLiteral("P2")),
                                    overlayPoint(p3, QStringLiteral("P3"))},
                                   {});
                    m_quickMeasurePoints3D.clear();
                } else {
                    u.normalize();
                    const Eigen::Vector3d normal = w.normalized();
                    const Eigen::Vector3d vAxis = normal.cross(u).normalized();
                    // 投影到局部 2D 坐标
                    auto project = [&](const Eigen::Vector3d& p) {
                        const Eigen::Vector3d d = p - p1;
                        return QPointF(d.dot(u), d.dot(vAxis));
                    };
                    QPointF a2 = project(p1);
                    QPointF b2 = project(p2);
                    QPointF c2 = project(p3);
                    double ax = a2.x(), ay = a2.y();
                    double bx = b2.x(), by = b2.y();
                    double cx = c2.x(), cy = c2.y();
                    double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
                    double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) +
                                 (cx * cx + cy * cy) * (ay - by)) /
                                d;
                    double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) +
                                 (cx * cx + cy * cy) * (bx - ax)) /
                                d;
                    double radius = sqrt(ux * ux + uy * uy);
                    // 圆心在局部坐标系，转回世界坐标
                    const Eigen::Vector3d center = p1 + u * ux + vAxis * uy;
                    double circumference = 2.0 * M_PI * radius;
                    QString label = tr("R=%1, C=%2").arg(radius, 0, 'f', 2).arg(circumference, 0, 'f', 2);
                    Logger::instance().info(
                        tr("3D 快速测量: 拟合圆 半径=%1, 周长=%2").arg(radius, 0, 'f', 3).arg(circumference, 0, 'f', 3),
                        "Measurement");
                    // 用多段线近似圆（在三点所在平面上）
                    QList<MeasurementOverlayLine3D> circleLines3D;
                    const int segments3D = 64;
                    for (int i = 0; i < segments3D; ++i) {
                        double a1 = 2.0 * M_PI * i / segments3D;
                        double a2 = 2.0 * M_PI * (i + 1) / segments3D;
                        const Eigen::Vector3d c1 = center + u * (radius * cos(a1)) + vAxis * (radius * sin(a1));
                        const Eigen::Vector3d c2 = center + u * (radius * cos(a2)) + vAxis * (radius * sin(a2));
                        circleLines3D.append(overlayLine(c1, c2, (i == 0) ? label : QString()));
                    }
                    buildOverlay3D({overlayPoint(p1, QStringLiteral("P1")), overlayPoint(p2, QStringLiteral("P2")),
                                    overlayPoint(p3, QStringLiteral("P3")), overlayPoint(center, QStringLiteral("C"))},
                                   circleLines3D);
                    m_quickMeasurePoints3D.clear();
                }
            }
        }
        // ---- 矩形面积 (3D: 2 clicks → 宽/高/面积, 用 X/Y 分量) ----
        else if (m_quickMeasureType == 5) {
            if (m_quickMeasurePoints3D.size() == 1) {
                buildOverlay3D({overlayPoint(m_quickMeasurePoints3D[0], ptLabel(0))}, {});
            } else if (m_quickMeasurePoints3D.size() >= 2) {
                const Eigen::Vector3d& a = m_quickMeasurePoints3D[0];
                const Eigen::Vector3d& b = m_quickMeasurePoints3D[1];
                double w = std::abs(b.x() - a.x());
                double h = std::abs(b.y() - a.y());
                double area = w * h;
                QString label = tr("W=%1, H=%2, A=%3").arg(w, 0, 'f', 2).arg(h, 0, 'f', 2).arg(area, 0, 'f', 2);
                Logger::instance().info(tr("3D 快速测量: 矩形 宽=%1, 高=%2, 面积=%3")
                                            .arg(w, 0, 'f', 3)
                                            .arg(h, 0, 'f', 3)
                                            .arg(area, 0, 'f', 3),
                                        "Measurement");
                buildOverlay3D({overlayPoint(a, QStringLiteral("P1")), overlayPoint(b, QStringLiteral("P2"))},
                               {overlayLine(a, b, label)});
                m_quickMeasurePoints3D.clear();
            }
        }
        return;
    }

    QString instanceId;
    IModule* mod = measurementInputForPicking(instanceId);
    if (!mod) {
        QGuiApplication::clipboard()->setText(
            QString("%1,%2,%3").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2).arg(z, 0, 'f', 2));
        Logger::instance().info(QString("3D pick: (%1, %2, %3) — no active MeasurementInput")
                                    .arg(x, 0, 'f', 3)
                                    .arg(y, 0, 'f', 3)
                                    .arg(z, 0, 'f', 3),
                                "Picking");
        return;
    }
    if (RunEngine::instance().isBusy() && instanceId != m_pendingMeasurementInputId) {
        rejectBusyFlowEdit(tr("拾取测量输入"));
        return;
    }

    QJsonObject params = mod->currentParams();
    const QString mode = params["mode"].toString("point_pair");
    const int cursor = m_measurementPickCursor.value(instanceId, 0);
    Project* project = ProjectManager::instance().currentProject();
    const QJsonArray newPoint = pointArray3D(x, y, z);
    auto finishPick = [&]() { finishMeasurementPick(instanceId, mod->currentParams(), true); };

    if (mode == "point_plane") {
        const int step = cursor % 4;
        if (step == 0) {
            mod->setParam("point", newPoint);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint);
            Logger::instance().info(
                QString("3D pick: point set to (%1, %2, %3)").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3),
                "Picking");
        } else {
            QJsonArray plane = updatedPlaneArray(params["plane"].toArray(), newPoint, step - 1);
            mod->setParam("plane", plane);
            if (project)
                project->setModuleParam(instanceId, "plane", plane);
            Logger::instance().info(QString("3D pick: plane p%1 set to (%2, %3, %4)")
                                        .arg(step)
                                        .arg(x, 0, 'f', 3)
                                        .arg(y, 0, 'f', 3)
                                        .arg(z, 0, 'f', 3),
                                    "Picking");
        }
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    if (mode == "line_pair") {
        const int step = cursor % 4;
        const QString key = step < 2 ? QStringLiteral("line1") : QStringLiteral("line2");
        const bool firstPoint = (step % 2) == 0;
        QJsonArray line = updatedLineArray(params[key].toArray(), QPointF(x, y), firstPoint);
        mod->setParam(key, line);
        if (project)
            project->setModuleParam(instanceId, key, line);
        Logger::instance().info(QString("3D pick: %1 p%2 set to (%3, %4, z=0; input z %5 ignored)")
                                    .arg(key)
                                    .arg(firstPoint ? 1 : 2)
                                    .arg(x, 0, 'f', 3)
                                    .arg(y, 0, 'f', 3)
                                    .arg(z, 0, 'f', 3),
                                "Picking");
        mod->setParam("pick_coordinate_frame", QStringLiteral("Unspecified"));
        if (project)
            project->setModuleParam(instanceId, "pick_coordinate_frame", QStringLiteral("Unspecified"));
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    if (mode == "point_line") {
        const int step = cursor % 3;
        if (step == 0) {
            mod->setParam("point", newPoint);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint);
            Logger::instance().info(
                QString("3D pick: point set to (%1, %2, %3)").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3),
                "Picking");
        } else {
            QJsonArray line = updatedLineArray(params["line"].toArray(), QPointF(x, y), step == 1);
            mod->setParam("line", line);
            if (project)
                project->setModuleParam(instanceId, "line", line);
            Logger::instance().info(QString("3D pick: line p%1 set to (%2, %3, z=0; input z %4 ignored)")
                                        .arg(step)
                                        .arg(x, 0, 'f', 3)
                                        .arg(y, 0, 'f', 3)
                                        .arg(z, 0, 'f', 3),
                                    "Picking");
        }
        mod->setParam("pick_coordinate_frame", QStringLiteral("Unspecified"));
        if (project)
            project->setModuleParam(instanceId, "pick_coordinate_frame", QStringLiteral("Unspecified"));
        m_measurementPickCursor[instanceId] = (step + 1) % 3;
        finishPick();
        return;
    }

    const bool setFirst = (cursor % 2) == 0;
    if (setFirst) {
        mod->setParam("point1", newPoint);
        if (project)
            project->setModuleParam(instanceId, "point1", newPoint);
        Logger::instance().info(
            QString("3D pick: point1 set to (%1, %2, %3)").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3),
            "Picking");
    } else {
        mod->setParam("point2", newPoint);
        if (project)
            project->setModuleParam(instanceId, "point2", newPoint);
        Logger::instance().info(
            QString("3D pick: point2 set to (%1, %2, %3)").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(z, 0, 'f', 3),
            "Picking");
    }
    m_measurementPickCursor[instanceId] = (cursor + 1) % 2;
    finishPick();
}

void MainWindow::onDataSourceAdded(const DataSource& ds) {
    if (m_dataSourcePanel) {
        m_dataSourcePanel->addDataSource(ds);
    }
}

void MainWindow::onDataSourceRemoved(const QString& id) {
    if (m_dataSourcePanel) {
        m_dataSourcePanel->removeDataSource(id);
    }
    m_hiddenDataSourceIds.remove(id);
    m_dataSourceDisplayData.remove(id);
    m_dataSourcePreviewImages.remove(id);
    const QString viewportId = m_dataSourceViewportIds.take(id);
    if (m_displayManager && m_displayManager->viewport(viewportId)) {
        m_displayManager->destroyViewport(viewportId);
    }
    rebuildPrimaryDataSourceDisplay();
    RunEngine::instance().clearOutputs();
    m_dirtyModuleIds.clear();
}

void MainWindow::onDataSourceVisibilityChanged(const QString& dataSourceId, bool visible) {
    QString errorCode;
    if (!setDataSourceVisibilityById(dataSourceId, visible, &errorCode)) {
        Logger::instance().warning(tr("数据源可见性修改失败：%1 (%2)").arg(dataSourceId, errorCode), "Display");
        return;
    }

    Logger::instance().info(tr("数据源%1：%2").arg(visible ? tr("显示") : tr("隐藏"), dataSourceId), "Display");
}

void MainWindow::onDisplayDataSource(const QString& dataSourceId) {
    QString errorCode;
    if (!displayDataSourceById(dataSourceId, &errorCode))
        Logger::instance().warning(tr("数据源显示失败：%1 (%2)").arg(dataSourceId, errorCode), "Display");
}

bool MainWindow::displayDataSourceById(const QString& dataSourceId, QString* errorCode) {
    auto fail = [errorCode](const QString& code) {
        if (errorCode)
            *errorCode = code;
        return false;
    };
    if (dataSourceId.trimmed().isEmpty())
        return fail(QStringLiteral("INVALID_ARGUMENT"));

    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return fail(QStringLiteral("NO_PROJECT"));

    std::optional<DataSource> target;
    int matches = 0;
    for (const DataSource& source : project->dataSources()) {
        if (source.id == dataSourceId) {
            ++matches;
            if (matches == 1)
                target = source;
        }
    }
    if (matches == 0)
        return fail(QStringLiteral("DATA_SOURCE_NOT_FOUND"));
    if (matches != 1)
        return fail(QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
    if (!target->isValid())
        return fail(QStringLiteral("DATA_SOURCE_LOAD_FAILED"));
    if (m_hiddenDataSourceIds.contains(dataSourceId))
        return fail(QStringLiteral("DATA_SOURCE_HIDDEN"));

    if (!m_dataSourceDisplayData.contains(dataSourceId)) {
        const QFileInfo fileInfo(target->filePath);
        if (!fileInfo.exists() || !fileInfo.isFile())
            return fail(QStringLiteral("DATA_SOURCE_LOAD_FAILED"));

        if (target->isImage() && QImage(target->filePath).isNull())
            return fail(QStringLiteral("DATA_SOURCE_LOAD_FAILED"));

        bool imported = false;
        if (target->isPointCloud())
            imported = importPointCloudFile(target->filePath, dataSourceId);
        else if (target->isImage())
            imported = importImageFile(target->filePath, dataSourceId);
        if (!imported || !m_dataSourceDisplayData.contains(dataSourceId))
            return fail(QStringLiteral("DATA_SOURCE_LOAD_FAILED"));
    }

    rebuildPrimaryDataSourceDisplay();
    if (!m_dataSourceDisplayData.contains(dataSourceId) || !primaryDataSourceViewport())
        return fail(QStringLiteral("DATA_SOURCE_LOAD_FAILED"));
    if (errorCode)
        errorCode->clear();
    return true;
}

bool MainWindow::setDataSourceVisibilityById(const QString& dataSourceId, bool visible, QString* errorCode) {
    auto fail = [errorCode](const QString& code) {
        if (errorCode)
            *errorCode = code;
        return false;
    };
    if (dataSourceId.trimmed().isEmpty())
        return fail(QStringLiteral("INVALID_ARGUMENT"));

    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return fail(QStringLiteral("NO_PROJECT"));
    int matches = 0;
    for (const DataSource& source : project->dataSources())
        matches += source.id == dataSourceId ? 1 : 0;
    if (matches == 0)
        return fail(QStringLiteral("DATA_SOURCE_NOT_FOUND"));
    if (matches != 1)
        return fail(QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));

    if (!visible) {
        m_hiddenDataSourceIds.insert(dataSourceId);
        if (m_dataSourcePanel)
            m_dataSourcePanel->setDataSourceVisibility(dataSourceId, false);
        if (ViewportWidget* viewport =
                m_displayManager ? m_displayManager->viewport(m_dataSourceViewportIds.value(dataSourceId)) : nullptr)
            viewport->setVisible(false);
        rebuildPrimaryDataSourceDisplay();
    } else {
        const bool wasHidden = m_hiddenDataSourceIds.contains(dataSourceId);
        m_hiddenDataSourceIds.remove(dataSourceId);
        if (!m_dataSourceDisplayData.contains(dataSourceId)) {
            QString displayError;
            if (!displayDataSourceById(dataSourceId, &displayError)) {
                if (wasHidden)
                    m_hiddenDataSourceIds.insert(dataSourceId);
                if (ViewportWidget* viewport =
                        m_displayManager ? m_displayManager->viewport(m_dataSourceViewportIds.value(dataSourceId))
                                         : nullptr)
                    viewport->setVisible(!m_hiddenDataSourceIds.contains(dataSourceId));
                rebuildPrimaryDataSourceDisplay();
                return fail(displayError.isEmpty() ? QStringLiteral("DATA_SOURCE_LOAD_FAILED") : displayError);
            }
        }
        if (ViewportWidget* viewport =
                m_displayManager ? m_displayManager->viewport(m_dataSourceViewportIds.value(dataSourceId)) : nullptr)
            viewport->setVisible(true);
        if (m_dataSourcePanel)
            m_dataSourcePanel->setDataSourceVisibility(dataSourceId, true);
        rebuildPrimaryDataSourceDisplay();
    }

    if (errorCode)
        errorCode->clear();
    return true;
}

QJsonObject MainWindow::executeAgentDataSourceViewAction(const QString& toolName, const QJsonObject& params) {
    auto error = [](const QString& message, const QString& code) {
        return QJsonObject{{QStringLiteral("error"), message},
                           {QStringLiteral("errorCode"), code},
                           {QStringLiteral("retryable"), false}};
    };
    if (QThread::currentThread() != thread())
        return error(QStringLiteral("Data source GUI action must run on the main window thread"),
                     QStringLiteral("THREAD_AFFINITY"));

    if (toolName == QLatin1String("save_viewport_snapshot_for_user")) {
        if (params.size() != 1 || !params.value(QStringLiteral("viewportId")).isString() ||
            params.value(QStringLiteral("viewportId")).toString().trimmed().isEmpty())
            return error(QStringLiteral("A single non-empty viewportId is required"), QStringLiteral("INVALID_PARAM"));
        const QString viewportId = params.value(QStringLiteral("viewportId")).toString();
        ViewportWidget* viewport = m_displayManager ? m_displayManager->viewport(viewportId) : nullptr;
        if (!viewport)
            return error(QStringLiteral("The requested viewport is not available"),
                         QStringLiteral("VIEWPORT_NOT_FOUND"));

        const QJsonObject result = viewport->saveSnapshotForUser();
        if (result.value(QStringLiteral("status")) == QLatin1String("saved"))
            Logger::instance().info(QStringLiteral("Agent saved a viewport snapshot"), "Agent");
        else if (result.value(QStringLiteral("status")) == QLatin1String("cancelled"))
            Logger::instance().info(QStringLiteral("Agent viewport snapshot save was cancelled"), "Agent");
        else if (result.contains(QStringLiteral("errorCode")))
            Logger::instance().warning(QStringLiteral("Agent viewport snapshot save failed: %1")
                                           .arg(result.value(QStringLiteral("errorCode")).toString()),
                                       "Agent");
        return result;
    }

    if (toolName == QLatin1String("open_data_source_folder_for_user")) {
        const QJsonValue idValue = params.value(QStringLiteral("dataSourceId"));
        if (params.size() != 1 || !idValue.isString() || idValue.toString().trimmed().isEmpty())
            return error(QStringLiteral("A single non-empty dataSourceId is required"),
                         QStringLiteral("INVALID_PARAM"));
        Project* project = ProjectManager::instance().currentProject();
        if (!project)
            return error(QStringLiteral("No project is open"), QStringLiteral("NO_PROJECT"));
        const QString dataSourceId = idValue.toString();
        const QList<DataSource> sources = project->dataSources();
        const DataSource* match = nullptr;
        int matches = 0;
        for (const DataSource& source : sources) {
            if (source.id != dataSourceId)
                continue;
            ++matches;
            if (matches == 1)
                match = &source;
        }
        if (matches == 0)
            return error(QStringLiteral("Data source was not found"), QStringLiteral("DATA_SOURCE_NOT_FOUND"));
        if (matches != 1)
            return error(QStringLiteral("Data source ID is ambiguous"), QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
        if (match->filePath.trimmed().isEmpty())
            return error(QStringLiteral("Data source path is unavailable"),
                         QStringLiteral("DATA_SOURCE_PATH_UNAVAILABLE"));

        const QUrl folderUrl = QUrl::fromLocalFile(QFileInfo(match->filePath).absolutePath());
        if (!openDesktopUrl(folderUrl))
            return error(QStringLiteral("The desktop file manager rejected the open request"),
                         QStringLiteral("DESKTOP_OPEN_FAILED"));
        Logger::instance().info(QStringLiteral("Agent requested the data source folder in the desktop file manager"),
                                "Agent");
        return {{QStringLiteral("status"), QStringLiteral("open_requested")}};
    }

    if (toolName == QLatin1String("open_user_knowledge_folder_for_user")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This knowledge folder action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));
        const QString knowledgeDir = AgentActor::userKnowledgeDir();
        if (!QFileInfo(knowledgeDir).isDir())
            return error(QStringLiteral("The user knowledge folder is unavailable"),
                         QStringLiteral("KNOWLEDGE_FOLDER_UNAVAILABLE"));
        if (!openDesktopUrl(QUrl::fromLocalFile(knowledgeDir)))
            return error(QStringLiteral("The desktop file manager rejected the open request"),
                         QStringLiteral("DESKTOP_OPEN_FAILED"));
        Logger::instance().info(QStringLiteral("Agent requested the user knowledge folder in the desktop file manager"),
                                "Agent");
        return {{QStringLiteral("status"), QStringLiteral("open_requested")}};
    }

    if (toolName == QLatin1String("open_sam_image_picker_for_user")) {
        if (!params.isEmpty())
            return error(QStringLiteral("The SAM image picker action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));

        SamAnnotatorDialog* dialog = m_samAnnotatorDialog.data();
        const bool created = !dialog;
        if (created) {
            dialog = new SamAnnotatorDialog(this, true);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            m_samAnnotatorDialog = dialog;
            dialog->applyTheme(m_isDarkTheme);
            connect(dialog, &QDialog::finished, this, [this, dialog](int) {
                if (m_samAnnotatorDialog == dialog)
                    m_samAnnotatorDialog.clear();
            });
            connect(dialog, &QObject::destroyed, this, [this, dialog]() {
                if (m_samAnnotatorDialog == dialog)
                    m_samAnnotatorDialog.clear();
            });
        }

        dialog->showImagePickerEntryForUser();
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        const bool imageAvailable = !dialog->imageSnapshot().isNull();
        Logger::instance().info(QStringLiteral("Agent opened SAM image picker entry (%1)")
                                    .arg(created ? QStringLiteral("opened") : QStringLiteral("reused")),
                                "Agent");
        return {{QStringLiteral("status"), QStringLiteral("opened")},
                {QStringLiteral("dialogAction"), created ? QStringLiteral("opened") : QStringLiteral("reused")},
                {QStringLiteral("userActionRequired"), true},
                {QStringLiteral("imageAvailable"), imageAvailable}};
    }

    if (toolName == QLatin1String("open_sam_annotator_dialog")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This SAM annotation action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));

        SamAnnotatorDialog* dialog = m_samAnnotatorDialog.data();
        const bool hasExistingImage = dialog && !dialog->imageSnapshot().isNull();
        ViewportWidget* targetViewport = nullptr;
        if (!hasExistingImage) {
            ViewportWidget* fallbackViewport = nullptr;
            const QVector<ViewportWidget*> viewports =
                m_displayManager ? m_displayManager->allViewports() : QVector<ViewportWidget*>();
            for (ViewportWidget* viewport : viewports) {
                if (!viewport || !viewport->imageWidget() || !viewport->imageWidget()->hasImage())
                    continue;
                if (!fallbackViewport)
                    fallbackViewport = viewport;
                if (viewport->hasFocus() || viewport->imageWidget()->hasFocus()) {
                    targetViewport = viewport;
                    break;
                }
            }
            if (!targetViewport)
                targetViewport = fallbackViewport;
            if (!targetViewport)
                return error(tr("No SAM annotation image is available"), QStringLiteral("NO_IMAGE"));
        }

        const bool created = !dialog;
        if (created) {
            dialog = new SamAnnotatorDialog(this, true);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            m_samAnnotatorDialog = dialog;
            dialog->applyTheme(m_isDarkTheme);
            connect(dialog, &QDialog::finished, this, [this, dialog](int) {
                if (m_samAnnotatorDialog == dialog)
                    m_samAnnotatorDialog.clear();
            });
            connect(dialog, &QObject::destroyed, this, [this, dialog]() {
                if (m_samAnnotatorDialog == dialog)
                    m_samAnnotatorDialog.clear();
            });
        }
        if (targetViewport && dialog->imageSnapshot().isNull()) {
            // Safe attach updates the visible image/session binding without preparing the SAM backend.
            dialog->attachToImageWidgetForEnvironmentSetup(targetViewport->imageWidget());
        }

        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        const bool imageAvailable = !dialog->imageSnapshot().isNull();
        Logger::instance().info(QStringLiteral("Agent opened SAM annotation dialog (%1)")
                                    .arg(created ? QStringLiteral("opened") : QStringLiteral("reused")),
                                "Agent");
        return {{QStringLiteral("status"), QStringLiteral("opened")},
                {QStringLiteral("dialogAction"), created ? QStringLiteral("opened") : QStringLiteral("reused")},
                {QStringLiteral("userActionRequired"), true},
                {QStringLiteral("imageAvailable"), imageAvailable}};
    }

    if (toolName == QLatin1String("open_sam_environment_setup")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This SAM environment action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));
        const bool created = m_samAnnotatorDialog.isNull();
        if (created) {
            m_samAnnotatorDialog = new SamAnnotatorDialog(this, true);
            m_samAnnotatorDialog->setAttribute(Qt::WA_DeleteOnClose);
            SamAnnotatorDialog* createdDialog = m_samAnnotatorDialog;
            connect(createdDialog, &QDialog::finished, this, [this, createdDialog](int) {
                if (m_samAnnotatorDialog == createdDialog)
                    m_samAnnotatorDialog.clear();
            });
            connect(createdDialog, &QObject::destroyed, this, [this, createdDialog]() {
                if (m_samAnnotatorDialog == createdDialog)
                    m_samAnnotatorDialog.clear();
            });
            m_samAnnotatorDialog->applyTheme(m_isDarkTheme);

            ViewportWidget* targetViewport = nullptr;
            const QVector<ViewportWidget*> viewports =
                m_displayManager ? m_displayManager->allViewports() : QVector<ViewportWidget*>();
            for (ViewportWidget* viewport : viewports) {
                if (!viewport || !viewport->imageWidget() || !viewport->imageWidget()->hasImage())
                    continue;
                if (!targetViewport)
                    targetViewport = viewport;
                if (viewport->hasFocus() || viewport->imageWidget()->hasFocus()) {
                    targetViewport = viewport;
                    break;
                }
            }
            if (targetViewport)
                m_samAnnotatorDialog->attachToImageWidgetForEnvironmentSetup(targetViewport->imageWidget());
        }

        m_samAnnotatorDialog->show();
        m_samAnnotatorDialog->raise();
        m_samAnnotatorDialog->activateWindow();
        QJsonObject status = m_samAnnotatorDialog->environmentHandoffStatus();
        status.insert(QStringLiteral("dialogOpened"), true);
        status.insert(QStringLiteral("dialogAction"), created ? QStringLiteral("opened") : QStringLiteral("reused"));
        Logger::instance().info(QStringLiteral("Agent opened SAM environment setup"), "Agent");
        return status;
    }

    if (toolName == QLatin1String("open_user_login_dialog")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This login dialog action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));

        LoginDialog* dialog = nullptr;
        const QList<LoginDialog*> existingDialogs = findChildren<LoginDialog*>();
        for (LoginDialog* existing : existingDialogs) {
            if (existing && existing->isVisible()) {
                dialog = existing;
                break;
            }
        }
        const bool reused = dialog != nullptr || !m_agentLoginDialog.isNull();
        if (!dialog && m_agentLoginDialog)
            dialog = m_agentLoginDialog;
        if (!dialog) {
            dialog = new LoginDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowModality(Qt::NonModal);
            m_agentLoginDialog = dialog;
            connect(dialog, &QDialog::finished, this, [this, dialog](int) {
                if (m_agentLoginDialog == dialog)
                    m_agentLoginDialog.clear();
            });
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        Logger::instance().info(QStringLiteral("Agent opened user login dialog"), "Agent");
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("opened")},
            {QStringLiteral("dialogAction"), reused ? QStringLiteral("reused") : QStringLiteral("opened")},
            {QStringLiteral("userActionRequired"), true}};
    }

    if (toolName == QLatin1String("open_agent_settings_dialog")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This Agent settings action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));

        AgentSettingsDialog* dialog = nullptr;
        for (AgentSettingsDialog* existing : findChildren<AgentSettingsDialog*>()) {
            if (!existing || !existing->isVisible())
                continue;
            if (existing->isModal()) {
                dialog = existing;
                break;
            }
            if (!dialog)
                dialog = existing;
        }
        const bool reused = dialog != nullptr || !m_agentPreferencesDialog.isNull();
        if (!dialog && m_agentPreferencesDialog)
            dialog = m_agentPreferencesDialog;
        if (!dialog) {
            dialog = new AgentSettingsDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowModality(Qt::NonModal);
            m_agentPreferencesDialog = dialog;
            connect(dialog, &QDialog::finished, this, [this, dialog](int) {
                if (m_agentPreferencesDialog == dialog)
                    m_agentPreferencesDialog.clear();
            });
            connect(dialog, &QDialog::accepted, this, [this, dialog]() { applyAgentSettings(*dialog); });
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        Logger::instance().info(QStringLiteral("Agent opened Agent settings dialog"), "Agent");
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("opened")},
            {QStringLiteral("dialogAction"), reused ? QStringLiteral("reused") : QStringLiteral("opened")},
            {QStringLiteral("userActionRequired"), true}};
    }

    if (toolName == QLatin1String("open_about_dialog")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This About action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));

        const QString title = tr("关于 DeepLux");
        const QString message = tr("<h2>DeepLux Vision 1.0.0</h2>"
                                   "<p>工业视觉检测软件</p>"
                                   "<p>&copy; 2024 DeepLux. All rights reserved.</p>");
        QMessageBox* dialog = nullptr;
        for (QMessageBox* existing : findChildren<QMessageBox*>()) {
            if (!existing || !existing->isVisible() || existing->windowTitle() != title || existing->text() != message)
                continue;
            if (existing->isModal()) {
                dialog = existing;
                break;
            }
            if (!dialog)
                dialog = existing;
        }
        const bool reused = dialog != nullptr || !m_agentAboutDialog.isNull();
        if (!dialog && m_agentAboutDialog)
            dialog = m_agentAboutDialog;
        if (!dialog) {
            dialog = new QMessageBox(QMessageBox::Information, title, message, QMessageBox::Ok, this);
            dialog->setObjectName(QStringLiteral("AgentAboutDialog"));
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowModality(Qt::NonModal);
            m_agentAboutDialog = dialog;
            connect(dialog, &QMessageBox::finished, this, [this, dialog](int) {
                if (m_agentAboutDialog == dialog)
                    m_agentAboutDialog.clear();
            });
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        Logger::instance().info(QStringLiteral("Agent opened About dialog"), "Agent");
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("opened")},
            {QStringLiteral("dialogAction"), reused ? QStringLiteral("reused") : QStringLiteral("opened")},
            {QStringLiteral("userActionRequired"), true}};
    }

    if (toolName == QLatin1String("open_knowledge_base_dialog")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This knowledge base action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));

        KnowledgeBaseDialog* dialog = nullptr;
        for (KnowledgeBaseDialog* existing : findChildren<KnowledgeBaseDialog*>()) {
            if (!existing || !existing->isVisible())
                continue;
            if (existing->isModal()) {
                dialog = existing;
                break;
            }
            if (!dialog)
                dialog = existing;
        }
        const bool reused = dialog != nullptr || !m_agentKnowledgeBaseDialog.isNull();
        if (!dialog && m_agentKnowledgeBaseDialog)
            dialog = m_agentKnowledgeBaseDialog;
        if (!dialog) {
            dialog = new KnowledgeBaseDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowModality(Qt::NonModal);
            m_agentKnowledgeBaseDialog = dialog;
            connect(dialog, &QDialog::finished, this, [this, dialog](int) {
                if (m_agentKnowledgeBaseDialog == dialog)
                    m_agentKnowledgeBaseDialog.clear();
            });
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        Logger::instance().info(QStringLiteral("Agent opened knowledge base dialog"), "Agent");
        return QJsonObject{
            {QStringLiteral("status"), QStringLiteral("opened")},
            {QStringLiteral("dialogAction"), reused ? QStringLiteral("reused") : QStringLiteral("opened")},
            {QStringLiteral("userActionRequired"), true}};
    }

    if (toolName.startsWith(QLatin1String("__quick_measurement_")) || toolName == QLatin1String("list_viewports") ||
        toolName == QLatin1String("begin_quick_measurement") ||
        toolName == QLatin1String("get_quick_measurement_status") ||
        toolName == QLatin1String("cancel_quick_measurement"))
        return executeAgentQuickMeasurementAction(toolName, params);
    if (toolName == QLatin1String("__sam_mask_edit_confirmation_snapshot") ||
        toolName == QLatin1String("__sam_category_edit_confirmation_snapshot") ||
        toolName == QLatin1String("__sam_prompt_cancel_confirmation_snapshot") ||
        toolName == QLatin1String("__sam_mask_edit_preflight") ||
        toolName == QLatin1String("__sam_category_edit_preflight") ||
        toolName == QLatin1String("__sam_prompt_cancel_preflight") ||
        toolName == QLatin1String("list_active_annotation_objects") ||
        toolName == QLatin1String("edit_active_annotation_masks") ||
        toolName == QLatin1String("list_active_annotation_categories") ||
        toolName == QLatin1String("edit_active_annotation_categories") ||
        toolName == QLatin1String("get_active_annotation_prompt_status") ||
        toolName == QLatin1String("cancel_active_annotation_prompt") ||
        toolName == QLatin1String("save_active_annotation_session") ||
        toolName == QLatin1String("open_active_annotation_session") ||
        toolName.startsWith(QLatin1String("__sam_session_open_")) ||
        toolName.startsWith(QLatin1String("__sam_session_save_")))
        return executeAgentActiveAnnotationAction(toolName, params);

    if (toolName == QLatin1String("list_recent_projects")) {
        QJsonArray projects;
        const QStringList recent = ProjectManager::instance().recentProjects();
        for (int index = 0; index < recent.size(); ++index) {
            const QString& path = recent.at(index);
            const QFileInfo info(path);
            projects.append(QJsonObject{{QStringLiteral("index"), index},
                                        {QStringLiteral("path"), path},
                                        {QStringLiteral("fileName"), info.fileName()},
                                        {QStringLiteral("exists"), info.exists()},
                                        {QStringLiteral("isFile"), info.isFile()}});
        }
        return {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("projects"), projects}};
    }

    if (toolName == QLatin1String("open_global_variables_dialog")) {
        if (!params.isEmpty())
            return error(QStringLiteral("This global variables action does not accept parameters"),
                         QStringLiteral("INVALID_PARAM"));

        GlobalVarView* dialog = nullptr;
        for (GlobalVarView* existing : findChildren<GlobalVarView*>()) {
            if (existing && existing->isVisible()) {
                dialog = existing;
                break;
            }
        }
        if (!dialog && m_agentGlobalVarDialog)
            dialog = m_agentGlobalVarDialog;
        const bool reused = dialog != nullptr;
        if (reused && !dialog->isVisible() && !dialog->refreshVariablesIfAuthorized())
            return error(QStringLiteral("Global variable access requires a logged-in user with GlobalVar permission"),
                         QStringLiteral("PERMISSION_DENIED"));
        if (!dialog) {
            dialog = new GlobalVarView(this);
            dialog->setObjectName(QStringLiteral("AgentGlobalVarDialog"));
            dialog->setWindowTitle(tr("全局变量"));
            dialog->setWindowModality(Qt::NonModal);
            m_agentGlobalVarDialog = dialog;
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        Logger::instance().info(QStringLiteral("Agent opened the global variables window for the user"), "Agent");
        return {{QStringLiteral("status"), QStringLiteral("opened")},
                {QStringLiteral("dialogAction"), reused ? QStringLiteral("reused") : QStringLiteral("opened")},
                {QStringLiteral("userActionRequired"), true}};
    }

    if (toolName == QLatin1String("open_system_settings")) {
        if (!m_agentSystemSettingsDialog) {
            auto* dialog = new SystemParamView(this);
            dialog->setObjectName(QStringLiteral("AgentSystemSettingsDialog"));
            dialog->setWindowTitle(tr("系统设置"));
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            m_agentSystemSettingsDialog = dialog;
            connect(dialog, &QDialog::finished, this, [this](int) {
                ConfigManager& config = ConfigManager::instance();
                const bool darkTheme = config.isInitialized()
                                           ? config.groupBool("appearance", "darkTheme", false)
                                           : SystemConfig::instance().theme() == QLatin1String("dark");
                if (m_isDarkTheme != darkTheme) {
                    m_isDarkTheme = darkTheme;
                    applyTheme();
                }
            });
        }
        m_agentSystemSettingsDialog->show();
        m_agentSystemSettingsDialog->raise();
        m_agentSystemSettingsDialog->activateWindow();
        Logger::instance().info(QStringLiteral("Agent requested system settings window"), "Agent");
        return {{QStringLiteral("status"), QStringLiteral("opened")},
                {QStringLiteral("page"), QStringLiteral("system_settings")},
                {QStringLiteral("userActionRequired"), true}};
    }

    if (toolName == QLatin1String("set_theme")) {
        const QString theme = params.value(QStringLiteral("theme")).toString();
        if (theme != QLatin1String("light") && theme != QLatin1String("dark"))
            return error(QStringLiteral("Theme must be light or dark"), QStringLiteral("INVALID_PARAM"));

        const bool darkTheme = theme == QLatin1String("dark");
        m_isDarkTheme = darkTheme;
        applyTheme();

        ConfigManager& config = ConfigManager::instance();
        if (!config.isInitialized())
            return {{QStringLiteral("status"), QStringLiteral("applied")},
                    {QStringLiteral("theme"), theme},
                    {QStringLiteral("persisted"), false}};

        config.setGroupValue(QStringLiteral("appearance"), QStringLiteral("darkTheme"), darkTheme);
        if (!config.save()) {
            return {{QStringLiteral("error"), QStringLiteral("Theme was applied but configuration could not be saved")},
                    {QStringLiteral("errorCode"), QStringLiteral("CONFIG_SAVE_FAILED")},
                    {QStringLiteral("retryable"), false},
                    {QStringLiteral("details"), QJsonObject{{QStringLiteral("applied"), true},
                                                            {QStringLiteral("theme"), theme},
                                                            {QStringLiteral("persisted"), false}}}};
        }
        return {{QStringLiteral("status"), QStringLiteral("applied")},
                {QStringLiteral("theme"), theme},
                {QStringLiteral("persisted"), true}};
    }

    if (toolName == QLatin1String("close_viewport")) {
        if (!params.contains(QStringLiteral("viewportId")))
            return error(QStringLiteral("A viewport ID is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue viewportId = params.value(QStringLiteral("viewportId"));
        if (!viewportId.isString() || viewportId.toString().trimmed().isEmpty())
            return error(QStringLiteral("Viewport ID must be a non-empty string"), QStringLiteral("INVALID_PARAM"));
        return closeViewportById(viewportId.toString());
    }

    if (toolName == QLatin1String("copy_data_source_path_to_clipboard")) {
        if (!params.contains(QStringLiteral("dataSourceId")))
            return error(QStringLiteral("A data source ID is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue dataSourceId = params.value(QStringLiteral("dataSourceId"));
        if (!dataSourceId.isString())
            return error(QStringLiteral("Data source ID must be a string"), QStringLiteral("INVALID_PARAM"));
        return copyDataSourcePathById(dataSourceId.toString());
    }

    if (toolName == QLatin1String("set_inspector_pinned")) {
        if (!params.contains(QStringLiteral("pinned")))
            return error(QStringLiteral("Inspector pin state is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue pinnedValue = params.value(QStringLiteral("pinned"));
        if (!pinnedValue.isBool())
            return error(QStringLiteral("Inspector pin state must be a boolean"), QStringLiteral("INVALID_PARAM"));
        if (!m_inspectorPanel)
            return error(QStringLiteral("Module inspector is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));

        m_inspectorPanel->setPinned(pinnedValue.toBool());
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("pinned"), m_inspectorPanel->isPinned()}};
    }

    if (toolName == QLatin1String("set_inspector_collapsed")) {
        if (!params.contains(QStringLiteral("collapsed")))
            return error(QStringLiteral("Inspector collapsed state is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue collapsed = params.value(QStringLiteral("collapsed"));
        if (!collapsed.isBool())
            return error(QStringLiteral("Inspector collapsed state must be boolean"), QStringLiteral("INVALID_PARAM"));
        if (!m_inspectorPanel)
            return error(QStringLiteral("Module inspector is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));
        const bool changed = m_inspectorPanel->isCollapsed() != collapsed.toBool();
        m_inspectorPanel->setCollapsed(collapsed.toBool());
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("collapsed"), m_inspectorPanel->isCollapsed()},
                {QStringLiteral("changed"), changed}};
    }

    if (toolName == QLatin1String("set_inspector_visibility")) {
        if (!params.contains(QStringLiteral("visible")))
            return error(QStringLiteral("Inspector visibility is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue visible = params.value(QStringLiteral("visible"));
        if (!visible.isBool())
            return error(QStringLiteral("Inspector visibility must be boolean"), QStringLiteral("INVALID_PARAM"));
        if (!m_inspectorPanel)
            return error(QStringLiteral("Module inspector is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));
        const bool changed =
            m_inspectorClosed != !visible.toBool() || m_inspectorPanel->isVisible() != visible.toBool();
        setInspectorVisibility(visible.toBool());
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("visible"), m_inspectorPanel->isVisible()},
                {QStringLiteral("changed"), changed}};
    }

    if (toolName == QLatin1String("set_primary_viewport_zoom")) {
        if (!params.contains(QStringLiteral("action")))
            return error(QStringLiteral("A zoom action is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue actionValue = params.value(QStringLiteral("action"));
        if (!actionValue.isString())
            return error(QStringLiteral("Zoom action must be a string"), QStringLiteral("INVALID_PARAM"));
        const QString action = actionValue.toString();
        if (action != QLatin1String("zoom_in") && action != QLatin1String("zoom_out") &&
            action != QLatin1String("fit") && action != QLatin1String("actual"))
            return error(QStringLiteral("Unsupported zoom action"), QStringLiteral("INVALID_PARAM"));

        ViewportWidget* viewport = primaryDataSourceViewport();
        if (!viewport)
            return error(QStringLiteral("Primary image viewport is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));
        HImageWidget* imageWidget = viewport->imageWidget();
        if (viewport->displayMode() != ViewportWidget::DisplayMode::Auto2D || !imageWidget || !imageWidget->hasImage())
            return error(QStringLiteral("Primary viewport has no active 2D image"), QStringLiteral("NO_IMAGE"));

        if (action == QLatin1String("zoom_in"))
            viewport->zoomIn();
        else if (action == QLatin1String("zoom_out"))
            viewport->zoomOut();
        else if (action == QLatin1String("fit"))
            viewport->fitToWindow();
        else
            viewport->actualSize();
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("action"), action},
                {QStringLiteral("zoomFactor"), imageWidget->zoom()}};
    }

    if (toolName == QLatin1String("set_primary_viewport_point_size")) {
        if (!params.contains(QStringLiteral("size")))
            return error(QStringLiteral("A point size is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue sizeValue = params.value(QStringLiteral("size"));
        if (!sizeValue.isDouble())
            return error(QStringLiteral("Point size must be a JSON number"), QStringLiteral("INVALID_PARAM"));
        const double size = sizeValue.toDouble();
        if (!std::isfinite(size) || size < 1.0 || size > 8.0 || std::abs(size * 10.0 - std::round(size * 10.0)) > 1e-8)
            return error(QStringLiteral("Point size must be 1.0–8.0 with at most one decimal place"),
                         QStringLiteral("INVALID_PARAM"));

        ViewportWidget* viewport = primaryDataSourceViewport();
        if (!viewport)
            return error(QStringLiteral("Primary point cloud viewport is unavailable"),
                         QStringLiteral("GUI_UNAVAILABLE"));
        if (viewport->displayMode() != ViewportWidget::DisplayMode::Auto3D)
            return error(QStringLiteral("Primary viewport is not displaying a point cloud"),
                         QStringLiteral("NO_POINT_CLOUD_VIEW"));
        auto* pointSize = viewport->findChild<QDoubleSpinBox*>(QStringLiteral("ViewportPointSizeSpinBox"));
        if (!viewport->viewport3D() || !pointSize)
            return error(QStringLiteral("Primary point size control is unavailable"),
                         QStringLiteral("GUI_UNAVAILABLE"));

        pointSize->setValue(size);
        return {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("size"), pointSize->value()}};
    }

    if (toolName == QLatin1String("set_primary_viewport_display_mode")) {
        if (!params.contains(QStringLiteral("mode")))
            return error(QStringLiteral("A display mode is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue modeValue = params.value(QStringLiteral("mode"));
        if (!modeValue.isString())
            return error(QStringLiteral("Display mode must be a string"), QStringLiteral("INVALID_PARAM"));
        const QString mode = modeValue.toString();
        if (mode != QLatin1String("2d") && mode != QLatin1String("3d"))
            return error(QStringLiteral("Display mode must be 2d or 3d"), QStringLiteral("INVALID_PARAM"));

        ViewportWidget* viewport = primaryDataSourceViewport();
        if (!viewport)
            return error(QStringLiteral("Primary viewport is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));
        const ViewportWidget::DisplayMode targetMode =
            mode == QLatin1String("2d") ? ViewportWidget::DisplayMode::Auto2D : ViewportWidget::DisplayMode::Auto3D;
        const bool changed = viewport->displayMode() != targetMode;
        if (changed) {
            if (!viewport->hasCachedImage())
                return error(QStringLiteral("Primary viewport has no cached image"), QStringLiteral("NO_IMAGE"));
            if (!viewport->hasCachedPointCloud())
                return error(QStringLiteral("Primary viewport has no cached point cloud"),
                             QStringLiteral("NO_POINT_CLOUD"));
        } else if (targetMode == ViewportWidget::DisplayMode::Auto2D && !viewport->hasCachedImage()) {
            return error(QStringLiteral("Primary viewport has no cached image"), QStringLiteral("NO_IMAGE"));
        } else if (targetMode == ViewportWidget::DisplayMode::Auto3D && !viewport->hasCachedPointCloud()) {
            return error(QStringLiteral("Primary viewport has no cached point cloud"),
                         QStringLiteral("NO_POINT_CLOUD"));
        }
        if (!viewport->setDisplayMode(targetMode))
            return error(QStringLiteral("Requested display content is unavailable"),
                         targetMode == ViewportWidget::DisplayMode::Auto2D ? QStringLiteral("NO_IMAGE")
                                                                           : QStringLiteral("NO_POINT_CLOUD"));
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("mode"), mode},
                {QStringLiteral("changed"), changed}};
    }

    if (toolName == QLatin1String("set_point_cloud_render_mode")) {
        if (!params.contains(QStringLiteral("mode")))
            return error(QStringLiteral("A render mode is required"), QStringLiteral("MISSING_PARAM"));
        const QJsonValue modeValue = params.value(QStringLiteral("mode"));
        if (!modeValue.isString())
            return error(QStringLiteral("Render mode must be a string"), QStringLiteral("INVALID_PARAM"));
        static const QStringList modes{QStringLiteral("uniform"), QStringLiteral("rgb"),
                                       QStringLiteral("height"),  QStringLiteral("intensity"),
                                       QStringLiteral("normal"),  QStringLiteral("blinn_phong")};
        const int index = modes.indexOf(modeValue.toString());
        if (index < 0)
            return error(QStringLiteral("Unsupported render mode"), QStringLiteral("INVALID_PARAM"));

        const bool anyPointCloudModeVisible =
            std::any_of(std::begin(m_renderActions), std::end(m_renderActions),
                        [](const QAction* action) { return action && action->isVisible(); });
        if (!anyPointCloudModeVisible)
            return error(QStringLiteral("No visible point cloud is available"), QStringLiteral("NO_POINT_CLOUD_VIEW"));
        QAction* action = m_renderActions[index];
        if (!action || !action->isVisible() || !action->isEnabled())
            return error(QStringLiteral("The current point cloud does not support this render mode"),
                         QStringLiteral("RENDER_MODE_UNAVAILABLE"));
        const QVector<ViewportWidget*> viewports =
            m_displayManager ? m_displayManager->allViewports() : QVector<ViewportWidget*>{};
        const bool viewportMismatch =
            std::any_of(viewports.cbegin(), viewports.cend(), [index](const ViewportWidget* viewport) {
                return viewport && viewport->renderMode() != index;
            });
        if (!action->isChecked() || viewportMismatch)
            action->trigger();
        return {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("mode"), modeValue.toString()}};
    }

    if (toolName == QLatin1String("set_log_level_filter")) {
        const QString minimumLevel = params.value(QStringLiteral("minimumLevel")).toString();
        const QStringList levels{QStringLiteral("all"), QStringLiteral("debug"), QStringLiteral("info"),
                                 QStringLiteral("warning"), QStringLiteral("error")};
        const int index = levels.indexOf(minimumLevel);
        if (index < 0)
            return error(QStringLiteral("A supported minimum log level is required"), QStringLiteral("INVALID_PARAM"));
        if (!m_logTable)
            return error(QStringLiteral("Runtime log table is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));

        onLogFilterChanged(index);
        int visibleRows = 0;
        for (int row = 0; row < m_logTable->rowCount(); ++row) {
            if (!m_logTable->isRowHidden(row))
                ++visibleRows;
        }
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("minimumLevel"), minimumLevel},
                {QStringLiteral("visibleRows"), visibleRows}};
    }

    if (toolName == QLatin1String("select_log_panel_tab")) {
        const QString tab = params.value(QStringLiteral("tab")).toString();
        if (tab != QLatin1String("runtime") && tab != QLatin1String("agent") && tab != QLatin1String("terminal"))
            return error(QStringLiteral("A supported log panel tab is required"), QStringLiteral("INVALID_PARAM"));
        if (!m_logTerminalTabs)
            return error(QStringLiteral("Log panel tabs are unavailable"), QStringLiteral("GUI_UNAVAILABLE"));

        QWidget* target = nullptr;
        if (tab == QLatin1String("runtime"))
            target = m_logTable ? m_logTable->parentWidget() : nullptr;
        else if (tab == QLatin1String("agent"))
            target = m_agentActionLogWidget;
        else
            target = m_terminalWidget;
        const int targetIndex = target ? m_logTerminalTabs->indexOf(target) : -1;
        if (targetIndex < 0)
            return error(QStringLiteral("Requested log panel tab is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));

        m_logTerminalTabs->setCurrentWidget(target);
        if (m_logTerminalTabs->currentWidget() != target)
            return error(QStringLiteral("Requested log panel tab did not become current"),
                         QStringLiteral("TAB_SELECTION_FAILED"));
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("tab"), tab},
                {QStringLiteral("tabIndex"), targetIndex},
                {QStringLiteral("dockVisible"), m_logDock && m_logDock->isVisible()}};
    }

    if (toolName == QLatin1String("scroll_agent_chat_to_latest")) {
        if (!m_agentChatPanel || !m_agentChatPanel->canScrollToLatest())
            return error(QStringLiteral("Agent conversation panel is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));
        int scrollValue = 0;
        int scrollMaximum = 0;
        if (!m_agentChatPanel->scrollToLatest(scrollValue, scrollMaximum))
            return error(QStringLiteral("Agent conversation did not reach its latest message"),
                         QStringLiteral("SCROLL_FAILED"));
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("scrollValue"), scrollValue},
                {QStringLiteral("scrollMaximum"), scrollMaximum},
                {QStringLiteral("atLatest"), true}};
    }

    if (toolName == QLatin1String("clear_runtime_logs")) {
        if (!m_logTable)
            return error(QStringLiteral("Runtime log table is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));

        const int clearedRuntimeRows = m_logTable->rowCount();
        const int clearedLoggerEntries = Logger::instance().clearLogsAndGetCount();
        return {{QStringLiteral("status"), QStringLiteral("completed")},
                {QStringLiteral("clearedLoggerEntries"), clearedLoggerEntries},
                {QStringLiteral("clearedRuntimeRows"), clearedRuntimeRows}};
    }

    if (toolName == QLatin1String("set_workspace_panel_visibility")) {
        const QString panel = params.value(QStringLiteral("panel")).toString();
        if (!params.value(QStringLiteral("visible")).isBool() ||
            (panel != QLatin1String("tools") && panel != QLatin1String("process") && panel != QLatin1String("bottom")))
            return error(QStringLiteral("A supported panel and boolean visible value are required"),
                         QStringLiteral("INVALID_PARAM"));

        QAction* action = nullptr;
        QWidget* panelWidget = nullptr;
        if (panel == QLatin1String("tools")) {
            action = m_viewToolPanelAction;
            panelWidget = m_toolBoxDock;
        } else if (panel == QLatin1String("process")) {
            action = m_viewProcessPanelAction;
            panelWidget = m_processTabWidget;
        } else {
            action = m_viewBottomPanelAction;
            panelWidget = m_logDock;
        }
        if (!action || !panelWidget)
            return {{QStringLiteral("error"), QStringLiteral("Workspace panel is unavailable")},
                    {QStringLiteral("errorCode"), QStringLiteral("VISIBILITY_UPDATE_FAILED")},
                    {QStringLiteral("retryable"), false},
                    {QStringLiteral("panel"), panel},
                    {QStringLiteral("visible"), QJsonValue::Null},
                    {QStringLiteral("effectivelyVisible"), QJsonValue::Null},
                    {QStringLiteral("suppressedByFocusMode"), false}};

        const bool requestedVisible = params.value(QStringLiteral("visible")).toBool();
        if (panel == QLatin1String("tools"))
            m_toolPanelUserClosed = !requestedVisible;
        action->setChecked(requestedVisible);
        if (m_focusMode && requestedVisible) {
            if (panel == QLatin1String("tools")) {
                m_toolPanelSuppressSignal = true;
                m_toolBoxDock->setVisible(false);
                m_toolPanelSuppressSignal = false;
            } else if (panel == QLatin1String("bottom")) {
                m_focusSavedLogVisible = true;
                m_logDock->setVisible(false);
            }
        } else if (m_focusMode && panel == QLatin1String("bottom")) {
            m_focusSavedLogVisible = false;
        }

        const bool actionVisible = action->isChecked();
        const bool effectivelyVisible = panelWidget->isVisible();
        const bool suppressedByFocusMode = m_focusMode && actionVisible && !effectivelyVisible;
        if (actionVisible != requestedVisible)
            return {
                {QStringLiteral("error"), QStringLiteral("Panel visibility action did not accept the requested state")},
                {QStringLiteral("errorCode"), QStringLiteral("VISIBILITY_UPDATE_FAILED")},
                {QStringLiteral("retryable"), false},
                {QStringLiteral("panel"), panel},
                {QStringLiteral("visible"), actionVisible},
                {QStringLiteral("effectivelyVisible"), effectivelyVisible},
                {QStringLiteral("suppressedByFocusMode"), suppressedByFocusMode}};
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("panel"), panel},
                {QStringLiteral("visible"), actionVisible},
                {QStringLiteral("effectivelyVisible"), effectivelyVisible},
                {QStringLiteral("suppressedByFocusMode"), suppressedByFocusMode}};
    }

    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return error(QStringLiteral("No project is open"), QStringLiteral("NO_PROJECT"));

    if (toolName == QLatin1String("select_flow_module")) {
        const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
        if (instanceId.trimmed().isEmpty())
            return error(QStringLiteral("A non-empty module instance ID is required"), QStringLiteral("INVALID_PARAM"));
        int matches = 0;
        for (const ModuleInstance& instance : project->modules()) {
            if (instance.id == instanceId)
                ++matches;
        }
        if (matches == 0)
            return error(QStringLiteral("Module instance was not found"), QStringLiteral("MODULE_NOT_FOUND"));
        if (matches > 1)
            return error(QStringLiteral("Module instance ID is ambiguous"), QStringLiteral("INSTANCE_ID_AMBIGUOUS"));
        if (!m_inspectorPanel)
            return error(QStringLiteral("Module selection requires an available inspector"),
                         QStringLiteral("GUI_UNAVAILABLE"));
        if (m_inspectorPanel->isPinned() && !m_selectedModuleId.isEmpty() && m_selectedModuleId != instanceId)
            return error(QStringLiteral("Inspector is pinned to another module"), QStringLiteral("INSPECTOR_PINNED"));
        selectModule(instanceId, true, false);
        if (m_selectedModuleId != instanceId)
            return error(QStringLiteral("Module selection did not complete"), QStringLiteral("SELECTION_FAILED"));
        m_inspectorPanel->showParamsTab();
        return {{QStringLiteral("status"), QStringLiteral("selected")},
                {QStringLiteral("instanceId"), instanceId},
                {QStringLiteral("inspectorVisible"), !m_inspectorClosed},
                {QStringLiteral("paramsTabVisible"), true}};
    }

    if (toolName == QLatin1String("get_data_source_view_state")) {
        QJsonArray states;
        const QList<DataSource> sources = project->dataSources();
        QSet<QString> ids;
        for (const DataSource& source : sources) {
            if (ids.contains(source.id))
                return error(QStringLiteral("Project contains duplicate data source IDs"),
                             QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
            ids.insert(source.id);
        }
        for (const DataSource& source : sources) {
            const QString viewportId = m_dataSourceViewportIds.value(source.id);
            const bool dedicated = !viewportId.isEmpty() && m_displayManager && m_displayManager->viewport(viewportId);
            states.append(QJsonObject{{QStringLiteral("id"), source.id},
                                      {QStringLiteral("displayed"), m_dataSourceDisplayData.contains(source.id)},
                                      {QStringLiteral("visible"), !m_hiddenDataSourceIds.contains(source.id)},
                                      {QStringLiteral("inDedicatedViewport"), dedicated}});
        }
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("dataSources"), states},
                {QStringLiteral("total"), states.size()}};
    }

    const QString dataSourceId = params.value(QStringLiteral("dataSourceId")).toString();
    QString errorCode;
    if (toolName == QLatin1String("display_data_source_in_dedicated_viewport")) {
        QString viewportId;
        bool created = false;
        if (!displayDataSourceInDedicatedViewportById(dataSourceId, &errorCode, &viewportId, &created))
            return error(tr("Could not display data source %1 in its dedicated viewport").arg(dataSourceId), errorCode);
        return {{QStringLiteral("status"), QStringLiteral("displayed")},
                {QStringLiteral("dataSourceId"), dataSourceId},
                {QStringLiteral("viewportId"), viewportId},
                {QStringLiteral("created"), created}};
    }
    if (toolName == QLatin1String("display_data_source")) {
        if (!displayDataSourceById(dataSourceId, &errorCode))
            return error(tr("Could not display data source %1").arg(dataSourceId), errorCode);
        ViewportWidget* viewport = primaryDataSourceViewport();
        return {{QStringLiteral("status"), QStringLiteral("displayed")},
                {QStringLiteral("dataSourceId"), dataSourceId},
                {QStringLiteral("viewportId"), viewport ? viewport->viewportId() : QString()}};
    }
    if (toolName == QLatin1String("set_data_source_visibility")) {
        const bool visible = params.value(QStringLiteral("visible")).toBool();
        if (!setDataSourceVisibilityById(dataSourceId, visible, &errorCode))
            return error(tr("Could not change visibility for data source %1").arg(dataSourceId), errorCode);
        const bool displayed = m_dataSourceDisplayData.contains(dataSourceId);
        return {{QStringLiteral("status"), visible ? QStringLiteral("visible") : QStringLiteral("hidden")},
                {QStringLiteral("dataSourceId"), dataSourceId},
                {QStringLiteral("displayed"), displayed}};
    }
    return error(QStringLiteral("Unsupported data source GUI action"), QStringLiteral("UNSUPPORTED_TOOL"));
}

void MainWindow::onDisplayDataSourceInNewViewport(const QString& dataSourceId) {
    QString errorCode;
    if (!displayDataSourceInDedicatedViewportById(dataSourceId, &errorCode))
        Logger::instance().warning(tr("专用视口显示数据源失败：%1 (%2)").arg(dataSourceId, errorCode), "Display");
}

bool MainWindow::displayDataSourceInDedicatedViewportById(const QString& dataSourceId, QString* errorCode,
                                                          QString* viewportId, bool* created) {
    auto fail = [errorCode](const QString& code) {
        if (errorCode)
            *errorCode = code;
        return false;
    };
    if (dataSourceId.trimmed().isEmpty())
        return fail(QStringLiteral("INVALID_PARAM"));
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("THREAD_AFFINITY"));
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return fail(QStringLiteral("NO_PROJECT"));

    std::optional<DataSource> source;
    int matches = 0;
    for (const DataSource& candidate : project->dataSources()) {
        if (candidate.id == dataSourceId) {
            ++matches;
            if (matches == 1)
                source = candidate;
        }
    }
    if (matches == 0)
        return fail(QStringLiteral("DATA_SOURCE_NOT_FOUND"));
    if (matches != 1)
        return fail(QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
    if (!source->isValid() || (!source->isImage() && !source->isPointCloud()))
        return fail(QStringLiteral("DATA_SOURCE_LOAD_FAILED"));
    if (m_hiddenDataSourceIds.contains(dataSourceId))
        return fail(QStringLiteral("DATA_SOURCE_HIDDEN"));

    if (!m_dataSourceDisplayData.contains(dataSourceId)) {
        bool loaded = false;
        if (source->isPointCloud())
            loaded = importPointCloudFile(source->filePath, dataSourceId, false);
        else
            loaded = importImageFile(source->filePath, dataSourceId, false);
        if (!loaded || !m_dataSourceDisplayData.contains(dataSourceId))
            return fail(QStringLiteral("DATA_SOURCE_LOAD_FAILED"));
    }

    const QString previousViewportId = m_dataSourceViewportIds.value(dataSourceId);
    const bool didCreate =
        previousViewportId.isEmpty() || !m_displayManager || !m_displayManager->viewport(previousViewportId);
    const QString id = ensureDataSourceViewport(dataSourceId, source->name);
    ViewportWidget* viewport = m_displayManager ? m_displayManager->viewport(id) : nullptr;
    if (!viewport) {
        if (didCreate && !id.isEmpty()) {
            m_dataSourceViewportIds.remove(dataSourceId);
            m_displayManager->destroyViewport(id);
        }
        return fail(QStringLiteral("GUI_UNAVAILABLE"));
    }

    const auto data = m_dataSourceDisplayData.constFind(dataSourceId);
    if (data->pointCloudData() && m_dataSourcePreviewImages.contains(dataSourceId))
        viewport->setDualData(m_dataSourcePreviewImages.value(dataSourceId), *data);
    else
        viewport->displayData(*data);
    viewport->setTitle(source->name);
    viewport->setVisible(true);
    if (viewportId)
        *viewportId = id;
    if (created)
        *created = didCreate;
    if (errorCode)
        errorCode->clear();
    return true;
}

void MainWindow::onRemoveDataSource(const QString& dataSourceId) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return;
    project->removeDataSource(dataSourceId);
}

void MainWindow::onShowDataSourceInFolder(const QString& dataSourceId) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return;
    auto ds = project->findDataSource(dataSourceId);
    if (!ds || ds->filePath.isEmpty())
        return;
    openDesktopUrl(QUrl::fromLocalFile(QFileInfo(ds->filePath).absolutePath()));
}

bool MainWindow::openDesktopUrl(const QUrl& url) const {
    return QDesktopServices::openUrl(url);
}

void MainWindow::onCopyDataSourcePath(const QString& dataSourceId) {
    copyDataSourcePathById(dataSourceId);
}

QJsonObject MainWindow::copyDataSourcePathById(const QString& dataSourceId) {
    auto error = [](const QString& message, const QString& code) {
        return QJsonObject{{QStringLiteral("error"), message},
                           {QStringLiteral("errorCode"), code},
                           {QStringLiteral("retryable"), false}};
    };
    if (QThread::currentThread() != thread())
        return error(QStringLiteral("Clipboard update must run on the main window thread"),
                     QStringLiteral("THREAD_AFFINITY"));
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return error(QStringLiteral("No project is open"), QStringLiteral("NO_PROJECT"));
    if (dataSourceId.trimmed().isEmpty())
        return error(QStringLiteral("A non-empty data source ID is required"), QStringLiteral("INVALID_PARAM"));

    const QList<DataSource> sources = project->dataSources();
    const DataSource* match = nullptr;
    for (const DataSource& source : sources) {
        if (source.id != dataSourceId)
            continue;
        if (match)
            return error(QStringLiteral("Data source ID is ambiguous"), QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
        match = &source;
    }
    if (!match)
        return error(QStringLiteral("Data source was not found"), QStringLiteral("DATA_SOURCE_NOT_FOUND"));
    if (match->filePath.isEmpty())
        return error(QStringLiteral("Data source has no file path"), QStringLiteral("DATA_SOURCE_PATH_UNAVAILABLE"));

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return error(QStringLiteral("System clipboard is unavailable"), QStringLiteral("GUI_UNAVAILABLE"));
    clipboard->setText(match->filePath);
    return {{QStringLiteral("status"), QStringLiteral("copied")},
            {QStringLiteral("dataSourceId"), dataSourceId},
            {QStringLiteral("path"), match->filePath}};
}

void MainWindow::addMeasurementConfigAction(QVBoxLayout* layout, const QString& consumerModuleId,
                                            const QString& consumerInstanceId, QDialog* dialog) {
    const QString mode = measurementInputModeForConsumer(consumerModuleId);
    if (mode.isEmpty() || !layout || !dialog) {
        return;
    }

    QGroupBox* group = new QGroupBox(tr("测量元素"));
    group->setObjectName("MeasurementInputActionGroup");
    QVBoxLayout* groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(10, 8, 10, 10);
    groupLayout->setSpacing(8);

    QLabel* label = new QLabel(tr("需要输入：%1").arg(measurementInputModeText(mode)), group);
    label->setWordWrap(true);
    groupLayout->addWidget(label);

    QPushButton* button = new QPushButton(tr("添加测量输入并开始拾取"), group);
    button->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#2563EB")));
    button->setObjectName("MeasurementInputSetupButton");
    button->setProperty("measurementInputMode", mode);
    button->setMinimumHeight(qMax(30, button->fontMetrics().height() + 10));
    groupLayout->addWidget(button);

    connect(button, &QPushButton::clicked, this, [this, mode, consumerInstanceId, dialog]() {
        const QString inputId = ensureMeasurementInputForMode(mode, consumerInstanceId);
        if (inputId.isEmpty()) {
            QMessageBox::warning(this, tr("测量输入不可用"), tr("无法创建测量输入插件，请先同步并重启插件。"));
            return;
        }
        Logger::instance().info(tr("已创建测量输入：%1，开始在视图中拾取元素").arg(inputId), "Config");
        dialog->accept();
    });

    layout->addWidget(group);
}

QString MainWindow::ensureMeasurementInputForMode(const QString& mode, const QString& consumerInstanceId) {
    if (mode.isEmpty() || rejectBusyFlowEdit(tr("创建测量输入"))) {
        return QString();
    }

    PluginManager& pm = PluginManager::instance();
    if (!pm.isPluginLoaded(QStringLiteral("MeasurementInput")) && !pm.loadPlugin(QStringLiteral("MeasurementInput"))) {
        return QString();
    }

    Project* project = ProjectManager::instance().currentProject();
    if (!project) {
        return QString();
    }

    const QString baseId = consumerInstanceId.isEmpty() ? QStringLiteral("measurement_input")
                                                        : QStringLiteral("%1_input").arg(consumerInstanceId);
    QString instanceId = baseId;
    int counter = 1;
    while (project->findModule(instanceId) || m_processTreeController->containsInstance(instanceId)) {
        instanceId = QStringLiteral("%1_%2").arg(baseId).arg(counter++);
    }

    ModuleInstance inst;
    inst.id = instanceId;
    inst.moduleId = QStringLiteral("MeasurementInput");
    inst.name = toolDisplayName(QStringLiteral("MeasurementInput"), tr("测量输入"));
    inst.params["mode"] = mode;
    inst.params["awaitUserPick"] = true;

    int insertRow = m_processTreeController->topLevelItemCount();
    if (m_processTreeController->containsInstance(consumerInstanceId)) {
        const int consumerRow =
            m_processTreeController->indexOfTopLevelItem(m_processTreeController->instanceItem(consumerInstanceId));
        if (consumerRow >= 0) {
            insertRow = consumerRow;
        }
    }

    project->addModule(inst);
    project->moveModule(instanceId, insertRow);

    const ModuleInstance* consumerInstance = project->findModule(consumerInstanceId);
    IModule* consumerModule = m_flowModules.value(consumerInstanceId, nullptr);
    const QList<PortSpec> consumerPorts =
        consumerModule ? consumerModule->inputPorts()
                       : (consumerInstance ? pm.pluginInfo(consumerInstance->moduleId).inputPorts : QList<PortSpec>{});
    const QList<PortSpec> inputPorts = pm.pluginInfo(QStringLiteral("MeasurementInput")).outputPorts;
    for (const PortSpec& consumerPort : consumerPorts) {
        if (!consumerPort.required)
            continue;
        bool provided = false;
        for (const PortSpec& inputPort : inputPorts) {
            if (inputPort.id == consumerPort.id) {
                provided = true;
                break;
            }
        }
        if (!provided)
            continue;
        ModuleConnection connection;
        connection.fromModuleId = instanceId;
        connection.toModuleId = consumerInstanceId;
        connection.fromPort = consumerPort.id;
        connection.toPort = consumerPort.id;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }

    QTreeWidgetItem* inputItem = m_processTreeController->instanceItem(instanceId);
    if (inputItem) {
        const int currentRow = m_processTreeController->indexOfTopLevelItem(inputItem);
        if (currentRow >= 0 && currentRow != insertRow) {
            m_processTreeController->takeTopLevelItem(currentRow);
            m_processTreeController->insertTopLevelItem(insertRow, inputItem);
        }
        m_processTreeController->setCurrentItem(inputItem);
    }
    selectModule(instanceId, true, true);

    m_activeMeasurementInputId = instanceId;
    m_measurementPickCursor[instanceId] = 0;
    m_measurementPickCount[instanceId] = 0;
    m_modulesNeedSync = true;
    return instanceId;
}

void MainWindow::applyTheme() {
    // 首先更新 ConfigWidgetHelper 的全局主题状态
    ConfigWidgetHelper::setGlobalDarkTheme(m_isDarkTheme);

    // 更新所有子控件的 ConfigWidgetHelper 样式（包括对话框中的控件）
    ConfigWidgetHelper::updateAllWidgetsStyle(this, m_isDarkTheme);

    setStyleSheet(ThemeManager::styleSheet(m_isDarkTheme));

    // 更新自定义标题栏样式
    const ThemePalette pal = ThemeManager::palette(m_isDarkTheme);

    if (m_toolBoxDock && m_toolBoxDock->titleBarWidget()) {
        m_toolBoxDock->titleBarWidget()->setStyleSheet(QString());
    }
    if (m_logDock && m_logDock->titleBarWidget()) {
        m_logDock->titleBarWidget()->setStyleSheet(
            QString("background-color: %1; border: none; border-bottom: 1px solid %2;")
                .arg(pal.bgColor, pal.borderColor));
        QLabel* label = m_logDock->titleBarWidget()->findChild<QLabel*>();
        if (label)
            label->setStyleSheet(
                QString("QLabel { color: %1; font-weight: 600; font-size: 14px; }").arg(pal.textColor));
    }

    // Tree widget 样式由 ThemeManager 统一管理，不再局部覆盖

    if (QScrollArea* toolCategoryScroll = findChild<QScrollArea*>("ToolCategoryScroll")) {
        toolCategoryScroll->setStyleSheet(QString());
    }

    if (m_processStatusWidget) {
        m_processStatusWidget->setStyleSheet(QString());
    }

    // Process tool button 样式由 ThemeManager 统一管理，不再局部覆盖

    QWidget* procToolBar = findChild<QWidget*>("ProcessToolBar");
    if (procToolBar) {
        procToolBar->setStyleSheet(QString());
    }

    QWidget* viewToggleWidget = findChild<QWidget*>("ViewToggleWidget");
    if (viewToggleWidget) {
        viewToggleWidget->setStyleSheet(QString());
    }

    QWidget* toolPanelWidget = findChild<QWidget*>("ToolPanelWidget");
    if (toolPanelWidget) {
        toolPanelWidget->setStyleSheet(QString());
    }
    QWidget* processPanelWidget = findChild<QWidget*>("ProcessPanelWidget");
    if (processPanelWidget) {
        processPanelWidget->setStyleSheet(QString());
    }
    QWidget* imageDisplayWidget = findChild<QWidget*>("ImageDisplayWidget");
    if (imageDisplayWidget) {
        imageDisplayWidget->setStyleSheet(QString());
    }
    if (m_logDock) {
        m_logDock->setStyleSheet(QString());
    }
    if (m_logTable) {
        m_logTable->setFrameShape(QFrame::NoFrame);
        m_logTable->setStyleSheet(
            QString("QTableWidget#LogTable { background-color: %1; color: %2; border: none; outline: none; "
                    "gridline-color: %3; font-size: 13px; }"
                    "QTableWidget#LogTable::item { border-bottom: 1px solid %3; }"
                    "QTableWidget#LogTable::item:selected { background-color: #0078d7; color: #ffffff; }"
                    "QHeaderView::section { background-color: %4; color: %2; padding: 5px; border: none; "
                    "border-bottom: 1px solid %3; font-size: 13px; }"
                    "QTableCornerButton::section { background-color: %4; border: none; }")
                .arg(pal.logTableBg, pal.logTextColor, pal.logLineColor, pal.logHeaderBg));
    }

    if (m_processTabWidget) {
        m_processTabWidget->setStyleSheet(QString());
    }
    if (m_logTerminalTabs) {
        m_logTerminalTabs->setStyleSheet(
            QString("QTabWidget::pane { border: none; background-color: %1; padding: 0px; margin: 0px; }"
                    "QTabBar::tab { background-color: %2; color: %3; font-size: 13px; font-weight: 500;"
                    "  min-height: 26px; padding: 3px 10px; border: none; margin-right: 1px; }"
                    "QTabBar::tab:selected { background-color: %1; color: %4; }"
                    "QTabBar::tab:hover:!selected { background-color: %5; }")
                .arg(pal.logTabPaneBg, pal.logTabBg, pal.logTabFg, pal.logTabSelFg, pal.logTabHoverBg));
    }

    // 更新 DisplayManager 中的 Viewport 样式
    if (m_displayManager) {
        m_displayManager->applyTheme(m_isDarkTheme);
    }

    // 问题 3: 画布主题跟随
    if (m_flowCanvas) {
        m_flowCanvas->applyTheme(m_isDarkTheme);
    }

    // 检查器面板主题
    if (m_inspectorPanel) {
        m_inspectorPanel->applyTheme(m_isDarkTheme);
    }

    // 更新 Terminal 主题颜色
    if (m_terminalWidget) {
        QColor termFg = m_isDarkTheme ? QColor("#d4d4d4") : QColor("#212121");
        QColor termBg = m_isDarkTheme ? QColor("#1e1e1e") : QColor("#ffffff");
        QColor termSel = m_isDarkTheme ? QColor("#264f78") : QColor("#b4d7ff");
        m_terminalWidget->setThemeColors(termFg, termBg, termSel);
    }

    // 更新 Agent Chat 面板主题
    if (m_agentChatPanel) {
        m_agentChatPanel->applyTheme(m_isDarkTheme);
    }

    // 更新 Agent Action Log 主题
    if (m_agentActionLogWidget) {
        m_agentActionLogWidget->applyTheme(m_isDarkTheme);
    }

    if (m_samAnnotatorDialog) {
        m_samAnnotatorDialog->applyTheme(m_isDarkTheme);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // 处理流程树的双击事件 — 普通双击打开检查器（切换到参数页）
    if (m_processTree && event->type() == QEvent::MouseButtonDblClick) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        QTreeWidgetItem* item = nullptr;

        if (watched == m_processTree) {
            item = m_processTree->itemAt(mouseEvent->pos());
        } else if (m_processTree->viewport() && watched == m_processTree->viewport()) {
            item = m_processTree->itemAt(mouseEvent->pos());
        }

        if (item && item->data(0, Qt::UserRole).toString() == "flow_item") {
            QString instanceName = item->data(0, Qt::UserRole + 1).toString();
            if (!instanceName.isEmpty()) {
                // 普通双击打开检查器（切换到参数页）
                selectModule(instanceName, true, true);
                if (m_inspectorPanel) {
                    m_inspectorPanel->showParamsTab();
                }
            }
            return true;
        }
    }

    // 主视图：双击进入/退出聚焦模式，拖放处理文件
    QWidget* imgWidget = m_displayManager ? m_displayManager->centralDisplay() : nullptr;
    if (imgWidget) {
        // 高: 双击事件可能由子视口接收，检查 watched 是否是 imgWidget 或其后代
        bool isMainView = (watched == imgWidget);
        if (!isMainView) {
            QWidget* w = qobject_cast<QWidget*>(watched);
            while (w) {
                if (w == imgWidget) {
                    isMainView = true;
                    break;
                }
                w = w->parentWidget();
            }
        }
        if (isMainView && event->type() == QEvent::MouseButtonDblClick) {
            toggleFocusMode();
            return true;
        }
        if (isMainView && event->type() == QEvent::DragEnter) {
            QDragEnterEvent* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                QList<QUrl> urls = de->mimeData()->urls();
                if (!urls.isEmpty()) {
                    QString ext = QFileInfo(urls.first().toLocalFile()).suffix().toLower();
                    QStringList supported = {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "ply"};
                    if (supported.contains(ext)) {
                        de->acceptProposedAction();
                        return true;
                    }
                }
            }
        }
        if (event->type() == QEvent::Drop) {
            QDropEvent* de = static_cast<QDropEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                QList<QUrl> urls = de->mimeData()->urls();
                if (!urls.isEmpty()) {
                    QString filePath = urls.first().toLocalFile();
                    importFile(filePath);
                    de->acceptProposedAction();
                    return true;
                }
            }
        }
    }

    // 处理流程树的键盘删除
    if (m_processTreeController && (watched == m_processTree || watched == m_processTree->viewport()) &&
        event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete) {
            QList<QTreeWidgetItem*> selected = m_processTreeController->selectedItems();
            if (!selected.isEmpty()) {
                if (rejectBusyFlowEdit(tr("删除模块")))
                    return true;
                QTreeWidgetItem* item = selected.first();
                QString instanceName = item->data(0, Qt::UserRole + 1).toString();
                m_processTreeController->removeFlowModule(instanceName);
            }
            return true;
        }
    }

    // 处理拖放到流程树
    if (m_processTreeController) {
        QTreeWidget* tree = m_processTreeController->tree();
        QWidget* processViewport = tree->viewport();
        if (processViewport && watched == processViewport &&
            (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
             event->type() == QEvent::Drop)) {
            QDropEvent* dropEvent = static_cast<QDropEvent*>(event);
            if (rejectBusyFlowEdit(tr("编辑流程"))) {
                dropEvent->ignore();
                return true;
            }

            // 获取拖放的数据
            const QMimeData* mimeData = dropEvent->mimeData();
            if (!mimeData) {
                return QMainWindow::eventFilter(watched, event);
            }

            auto insertRowForDrop = [this, tree, dropEvent]() {
                QTreeWidgetItem* hoverItem = tree->itemAt(dropEvent->pos());
                if (!hoverItem) {
                    return tree->topLevelItemCount();
                }

                const QRect itemRect = tree->visualItemRect(hoverItem);
                const int itemMiddle = itemRect.top() + itemRect.height() / 2;
                const int hoverRow = tree->indexOfTopLevelItem(hoverItem);
                return dropEvent->pos().y() < itemMiddle ? hoverRow : hoverRow + 1;
            };

            const QString pluginName = PluginDragPayload::pluginName(mimeData);
            const QString draggedFlowInstanceId = PluginDragPayload::flowInstanceId(mimeData);
            const bool hasPluginPayload = !pluginName.isEmpty() && draggedFlowInstanceId.isEmpty();
            const bool isToolBoxDrop =
                (dropEvent->source() == m_toolBoxTree || dropEvent->source() == m_toolBoxTree->viewport())
                    ? hasPluginPayload
                    : (!dropEvent->source() && hasPluginPayload);
            QTreeWidgetItem* selectedFlowItem = m_processTreeController->currentItem();
            const bool hasSelectedFlowPayload =
                pluginName.isEmpty() && !draggedFlowInstanceId.isEmpty() && selectedFlowItem &&
                selectedFlowItem->data(0, Qt::UserRole).toString() == "flow_item" &&
                selectedFlowItem->data(0, Qt::UserRole + 1).toString() == draggedFlowInstanceId;
            const bool isProcessTreeSource = dropEvent->source() == tree || dropEvent->source() == tree->viewport();
            const bool isProcessTreeDrop = (isProcessTreeSource || !dropEvent->source()) && hasSelectedFlowPayload;
            if (!isToolBoxDrop && !isProcessTreeDrop) {
                dropEvent->ignore();
                return true;
            }

            if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
                if (mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
                    dropEvent->setDropAction(isProcessTreeDrop ? Qt::MoveAction : Qt::CopyAction);
                    dropEvent->accept();
                    return true;
                }
                dropEvent->ignore();
                return true;
            }

            if (isProcessTreeDrop && mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
                QTreeWidgetItem* sourceItem = m_processTreeController->currentItem();
                if (!sourceItem || sourceItem->data(0, Qt::UserRole).toString() != "flow_item") {
                    dropEvent->ignore();
                    return true;
                }

                const int sourceRow = m_processTreeController->indexOfTopLevelItem(sourceItem);
                int insertRow = insertRowForDrop();
                if (sourceRow < 0 || insertRow == sourceRow || insertRow == sourceRow + 1) {
                    dropEvent->setDropAction(Qt::MoveAction);
                    dropEvent->accept();
                    return true;
                }

                QTreeWidgetItem* movedItem = m_processTreeController->takeTopLevelItem(sourceRow);
                if (insertRow > sourceRow) {
                    --insertRow;
                }
                insertRow = qMax(0, qMin(insertRow, m_processTreeController->topLevelItemCount()));
                m_processTreeController->insertTopLevelItem(insertRow, movedItem);
                m_processTreeController->setCurrentItem(movedItem);

                const QString instanceId = movedItem->data(0, Qt::UserRole + 1).toString();
                if (Project* project = ProjectManager::instance().currentProject()) {
                    project->moveModule(instanceId, insertRow);
                }
                m_modulesNeedSync = true;

                dropEvent->setDropAction(Qt::MoveAction);
                dropEvent->accept();
                return true;
            }

            // 检查是否有来自工具箱的拖放
            if (isToolBoxDrop && mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
                const QString pluginName = PluginDragPayload::pluginName(mimeData);
                if (!pluginName.isEmpty()) {
                    if (!ProjectManager::instance().currentProject() && !ProjectManager::instance().newProject()) {
                        Logger::instance().error(tr("无法创建工程，不能添加插件：%1").arg(pluginName), "Flow");
                        dropEvent->ignore();
                        return true;
                    }

                    // 隐藏提示标签
                    m_processTreeController->hideHintLabel();

                    const int insertRow = insertRowForDrop();

                    // 先添加树节点（即时视觉反馈），再异步创建模块实例
                    QTreeWidgetItem* newItem = new QTreeWidgetItem();
                    newItem->setFlags((newItem->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
                    m_processTreeController->insertTopLevelItem(insertRow, newItem);

                    QString instanceName = pluginName;
                    int counter = 1;
                    while (m_processTreeController->isUsedName(instanceName)) {
                        instanceName = QString("%1_%2").arg(pluginName).arg(counter++);
                    }
                    m_processTreeController->insertUsedName(instanceName);
                    newItem->setData(0, Qt::UserRole, "flow_item");
                    newItem->setData(0, Qt::UserRole + 1, instanceName);
                    newItem->setData(0, Qt::UserRole + 2, pluginName);
                    const QString placeholderToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    newItem->setData(0, Qt::UserRole + 3, placeholderToken);
                    m_processTreeController->insertPendingInsertion(instanceName, newItem, placeholderToken);

                    // 推迟模块创建到事件循环 — 避免在拖放嵌套循环中阻塞
                    schedulePendingToolboxDrop(pluginName, instanceName, newItem, placeholderToken,
                                               ProjectManager::instance().currentProject());

                    dropEvent->setDropAction(Qt::CopyAction);
                    dropEvent->accept();
                    return true;
                }
                // MIME 载荷不包含有效插件 ID，拒绝处理。
                return true;
            }
            // Not a plugin drop, let default handling occur
            return QMainWindow::eventFilter(watched, event);
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::schedulePendingToolboxDrop(const QString& pluginName, const QString& instanceName,
                                            QTreeWidgetItem* item, const QString& token, Project* targetProject) {
    QPointer<Project> guardedProject(targetProject);
    QTimer::singleShot(0, this, [this, pluginName, instanceName, item, token, guardedProject]() {
        commitPendingToolboxDrop(pluginName, instanceName, item, token, guardedProject.data());
    });
}

void MainWindow::commitPendingToolboxDrop(const QString& pluginName, const QString& instanceName, QTreeWidgetItem* item,
                                          const QString& token, Project* targetProject) {
    auto discardPlaceholder = [this, &instanceName, item, &token]() {
        if (m_processTreeController)
            m_processTreeController->discardPendingInsertion(instanceName, item, token);
    };
    auto validPendingDrop = [this, instanceName, item, token, targetProject]() {
        return !RunEngine::instance().isBusy() && targetProject &&
               ProjectManager::instance().currentProject() == targetProject && m_processTreeController &&
               m_processTreeController->isPendingInsertion(instanceName, item, token);
    };

    if (!validPendingDrop()) {
        statusBar()->showMessage(tr("运行状态或工程已变化，未添加插件"), 5000);
        discardPlaceholder();
        return;
    }

    IModule* module = PluginManager::instance().createModule(pluginName);
    if (!module) {
        Logger::instance().error(tr("无法创建插件：%1").arg(pluginName), "Flow");
        discardPlaceholder();
        return;
    }
    if (!module->initialize()) {
        Logger::instance().error(tr("插件初始化失败：%1").arg(pluginName), "Flow");
        PluginManager::instance().destroyModule(module);
        discardPlaceholder();
        return;
    }
    if (!validPendingDrop()) {
        module->shutdown();
        PluginManager::instance().destroyModule(module);
        statusBar()->showMessage(tr("运行状态或工程已变化，未添加插件"), 5000);
        discardPlaceholder();
        return;
    }

    m_processTreeController->completePendingInsertion(instanceName, token);
    const QString displayName = toolDisplayName(pluginName, module->name());
    item->setText(0, displayName);
    item->setIcon(0, module->icon());
    m_flowModules.insert(instanceName, module);
    m_modulesNeedSync = true;
    ModuleInstance inst;
    inst.id = instanceName;
    inst.moduleId = pluginName;
    inst.name = displayName;
    targetProject->addModule(inst);
    targetProject->moveModule(instanceName, m_processTreeController->indexOfTopLevelItem(item));
    Logger::instance().info(tr("已添加插件到流程：%1 (%2)").arg(displayName).arg(instanceName), "Flow");
    selectModule(instanceName, true);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    // 只接受文件拖入
    if (event->mimeData()->hasUrls()) {
        QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            QString filePath = urls.first().toLocalFile();
            QFileInfo fileInfo(filePath);
            QString ext = fileInfo.suffix().toLower();
            QStringList supportedExts = {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "ply"};
            if (supportedExts.contains(ext)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QList<QUrl> urls = mimeData->urls();
        if (!urls.isEmpty()) {
            QString filePath = urls.first().toLocalFile();
            if (importFile(filePath)) {
                Logger::instance().info(tr("拖放导入文件：%1").arg(QFileInfo(filePath).fileName()), "System");
            }
        }
    }
    event->acceptProposedAction();
}

// TIFF 大文件阈值与跳采样步长（避免 UI 卡顿）
static constexpr qint64 LARGE_TIFF_THRESHOLD = 100 * 1024 * 1024; // 100MB
static constexpr int TIFF_STEP_LARGE = 2;
static constexpr int TIFF_STEP_NORMAL = 1;

bool MainWindow::importFile(const QString& filePath) {
    if (filePath.isEmpty())
        return false;

    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        Logger::instance().error(tr("文件不存在：%1").arg(filePath), "System");
        return false;
    }

    const QString ext = fileInfo.suffix().toLower();
    if (ext != "ply" && ext != "tif" && ext != "tiff" && rejectBusyFlowEdit(tr("导入图像并配置流程")))
        return false;

    if (!ProjectManager::instance().hasProject()) {
        ProjectManager::instance().newProject();
        Logger::instance().info(tr("自动创建项目"), "System");
    }

    m_lastImportedImagePath = filePath;
    Logger::instance().info(QString("[importFile] %1 ext=%2").arg(fileInfo.fileName()).arg(ext), "3D");

    if (ext == "ply" || ext == "tif" || ext == "tiff") {
        Logger::instance().info("[importFile] → importPointCloudFile", "3D");
        return importPointCloudFile(filePath);
    }
    Logger::instance().info("[importFile] → importImageFile", "3D");
    return importImageFile(filePath);
}

bool MainWindow::importPointCloudFile(const QString& filePath, const QString& existingDataSourceId,
                                      bool rebuildPrimaryDisplay) {
    QFileInfo fileInfo(filePath);
    QString ext = fileInfo.suffix().toLower();
    Logger::instance().info(
        QString("[imp3D] file=%1 size=%2 ext=%3").arg(fileInfo.fileName()).arg(fileInfo.size()).arg(ext), "3D");
    PointCloudData pc;
    QString error;
    bool ok = false;

    if (ext == "ply") {
        ok = PlyLoader::load(filePath, pc, error);
    } else if (ext == "tif" || ext == "tiff") {
        TiffLoader::Config cfg;
        cfg.step = (fileInfo.size() > LARGE_TIFF_THRESHOLD) ? TIFF_STEP_LARGE : TIFF_STEP_NORMAL;
        ok = TiffLoader::load(filePath, pc, error, cfg);
    } else {
        Logger::instance().error(tr("不支持的点云格式：%1").arg(ext), "System");
        return false;
    }

    if (!ok) {
        QString reason = error.isEmpty() ? tr("未知错误") : error;
        Logger::instance().error(tr("3D 文件加载失败：%1 (%2)").arg(fileInfo.fileName()).arg(reason), "3D");
        return false;
    }

    Project* project = ProjectManager::instance().currentProject();
    if (!project) {
        return false;
    }

    DataSource ds;
    if (!existingDataSourceId.isEmpty()) {
        const auto existing = project->findDataSource(existingDataSourceId);
        if (!existing) {
            return false;
        }
        ds = *existing;
    } else {
        for (const DataSource& source : project->dataSources()) {
            if (source.filePath == fileInfo.absoluteFilePath()) {
                ds = source;
                break;
            }
        }
        if (ds.id.isEmpty()) {
            ds.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            ds.name = fileInfo.fileName();
            ds.filePath = fileInfo.absoluteFilePath();
            ds.type = "pointcloud";
            ds.metadata["pointCount"] = static_cast<qint64>(pc.size());
            ds.metadata["fileSize"] = fileInfo.size();
            ds.importTime = QDateTime::currentMSecsSinceEpoch();
            project->addDataSource(ds);
        }
    }

    Logger::instance().info(QString("[imp3D] loaded %1 points").arg(pc.size()), "3D");

    size_t pointCount = pc.size();
    DisplayData data;
    data.variant() = std::move(pc);
    data.setTimestamp(QDateTime::currentMSecsSinceEpoch());
    data.setMetadata(QVariantMap{{"dataSourceId", ds.id}, {"dataSourceName", ds.name}});
    QImage previewImage;
    if (ext == "tif" || ext == "tiff") {
#ifdef DEEPLUX_HAS_OPENCV
        cv::Mat mat = cv::imread(filePath.toStdString(), cv::IMREAD_UNCHANGED);
        Logger::instance().info(QString("[imp3D] cv::imread: empty=%1, type=%2, channels=%3")
                                    .arg(mat.empty() ? "true" : "false")
                                    .arg(mat.type())
                                    .arg(mat.channels()),
                                "3D");
        if (!mat.empty()) {
            if (mat.channels() == 1) {
                cv::Mat mat8u;
                if (mat.type() != CV_8U) {
                    cv::normalize(mat, mat8u, 0, 255, cv::NORM_MINMAX, CV_8U);
                } else {
                    mat8u = mat;
                }
                cv::Mat colored;
                cv::applyColorMap(mat8u, colored, cv::COLORMAP_JET);
                previewImage = QImage(colored.data, colored.cols, colored.rows, static_cast<int>(colored.step),
                                      QImage::Format_RGB888)
                                   .copy();
            } else {
                cv::Mat rgb;
                cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
                previewImage =
                    QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
            }
        } else {
            Logger::instance().warning("[imp3D] cv::imread returned empty, falling back to 3D only", "3D");
        }
#endif
    }
    m_dataSourceDisplayData.insert(ds.id, std::move(data));
    if (previewImage.isNull()) {
        m_dataSourcePreviewImages.remove(ds.id);
    } else {
        m_dataSourcePreviewImages.insert(ds.id, previewImage);
    }
    if (rebuildPrimaryDisplay)
        rebuildPrimaryDataSourceDisplay();
    Logger::instance().info(tr("导入 3D 点云：%1 (%2 点)").arg(fileInfo.fileName()).arg(pointCount), "System");

    if (rebuildPrimaryDisplay && m_processTabWidget && m_dataSourcePanel) {
        m_processTabWidget->setCurrentWidget(m_dataSourcePanel);
    }

    return true;
}

bool MainWindow::importImageFile(const QString& filePath, const QString& existingDataSourceId,
                                 bool rebuildPrimaryDisplay) {
    Logger::instance().info(tr("导入图像：%1").arg(filePath), "System");

    if (existingDataSourceId.isEmpty() && rejectBusyFlowEdit(tr("导入图像并配置流程")))
        return false;

    QFileInfo fileInfo(filePath);
    Project* project = ProjectManager::instance().currentProject();
    if (!project) {
        return false;
    }

    DataSource ds;
    if (!existingDataSourceId.isEmpty()) {
        const auto existing = project->findDataSource(existingDataSourceId);
        if (!existing) {
            return false;
        }
        ds = *existing;
    } else {
        for (const DataSource& source : project->dataSources()) {
            if (source.filePath == fileInfo.absoluteFilePath()) {
                ds = source;
                break;
            }
        }
        if (ds.id.isEmpty()) {
            ds.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            ds.name = fileInfo.fileName();
            ds.filePath = fileInfo.absoluteFilePath();
            ds.type = "image";
            ds.metadata["fileSize"] = fileInfo.size();
            ds.importTime = QDateTime::currentMSecsSinceEpoch();
            project->addDataSource(ds);
        }
    }

    if (existingDataSourceId.isEmpty()) {
        autoConfigureGrabImage(filePath);
    }

    QImage image(filePath);
    if (!image.isNull()) {
        DisplayData data;
        data.variant() = ImageData(image);
        data.setMetadata(QVariantMap{{"dataSourceId", ds.id}, {"dataSourceName", ds.name}});
        m_dataSourceDisplayData.insert(ds.id, std::move(data));
        if (rebuildPrimaryDisplay)
            rebuildPrimaryDataSourceDisplay();
    }

    if (rebuildPrimaryDisplay && m_processTabWidget && m_dataSourcePanel) {
        m_processTabWidget->setCurrentWidget(m_dataSourcePanel);
    }

    return true;
}

void MainWindow::onRenderModeChanged(int index) {
    auto viewports = m_displayManager->allViewports();
    for (auto* vp : viewports) {
        if (vp)
            vp->setRenderMode(index);
    }
}

void MainWindow::updateRenderModeCombo() {
    if (m_renderSeparator) {
        m_renderSeparator->setVisible(false);
    }
    for (QAction* action : m_renderActions) {
        if (action) {
            action->setVisible(false);
        }
    }
}

void MainWindow::updateRenderModeComboForData(const PointCloudData& pc) {
    const bool supports[] = {
        true, pc.hasColors(), true, pc.intensities.size() == pc.size(), pc.hasNormals(), pc.hasNormals(),
    };

    if (m_renderSeparator) {
        m_renderSeparator->setVisible(true);
    }

    bool selectedModeSupported = false;
    for (int i = 0; i < 6; ++i) {
        QAction* action = m_renderActions[i];
        if (!action) {
            continue;
        }
        action->setVisible(supports[i]);
        action->setEnabled(supports[i]);
        selectedModeSupported = selectedModeSupported || (supports[i] && action->isChecked());
    }

    if (!selectedModeSupported) {
        m_renderActions[0]->setChecked(true);
        onRenderModeChanged(0);
    }
}

void MainWindow::autoConfigureGrabImage(const QString& filePath) {
    // 查找流程中第一个 GrabImage 模块
    for (auto it = m_flowModules.begin(); it != m_flowModules.end(); ++it) {
        IModule* module = it.value();
        if (module && module->moduleId().contains("GrabImage", Qt::CaseInsensitive)) {
            module->setParam("filePath", filePath);
            module->setParam("grabSource", "Path");
            if (Project* project = ProjectManager::instance().currentProject()) {
                project->setModuleParam(it.key(), QStringLiteral("filePath"), filePath);
                project->setModuleParam(it.key(), QStringLiteral("grabSource"), QStringLiteral("Path"));
            } else {
                markModuleDirty(it.key());
            }
            m_modulesNeedSync = true;
            Logger::instance().info(tr("已自动配置 GrabImage 模块使用文件：%1").arg(filePath), "System");
            return;
        }
    }
}

void MainWindow::clearCentralDisplay() {
    if (m_displayManager) {
        m_displayManager->clearAll();
    }
    updateRenderModeCombo();
}

ViewportWidget* MainWindow::primaryDataSourceViewport() {
    if (!m_displayManager) {
        return nullptr;
    }

    if (!m_primaryDataSourceViewportId.isEmpty()) {
        if (ViewportWidget* viewport = m_displayManager->viewport(m_primaryDataSourceViewportId)) {
            return viewport;
        }
    }

    const QList<QString> dedicatedViewportIds = m_dataSourceViewportIds.values();
    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        if (viewport && !dedicatedViewportIds.contains(viewport->viewportId())) {
            m_primaryDataSourceViewportId = viewport->viewportId();
            return viewport;
        }
    }
    return nullptr;
}

void MainWindow::rebuildPrimaryDataSourceDisplay() {
    ViewportWidget* primaryViewport = primaryDataSourceViewport();
    Project* project = ProjectManager::instance().currentProject();
    if (!primaryViewport || !project) {
        return;
    }

    QList<QImage> images;
    bool hasImageData = false;
    QVector<const PointCloudData*> pointClouds;
    for (const DataSource& source : project->dataSources()) {
        if (m_hiddenDataSourceIds.contains(source.id)) {
            continue;
        }
        const auto data = m_dataSourceDisplayData.constFind(source.id);
        if (data == m_dataSourceDisplayData.cend()) {
            continue;
        }
        if (const ImageData* image = data->imageData()) {
            const QImage value = image->toQImage();
            if (!value.isNull()) {
                images.append(value);
                hasImageData = true;
            }
        }
        if (const QImage preview = m_dataSourcePreviewImages.value(source.id); !preview.isNull()) {
            images.append(preview);
        }
        if (const PointCloudData* cloud = data->pointCloudData(); cloud && !cloud->isEmpty()) {
            pointClouds.append(cloud);
        }
    }

    if (images.isEmpty() && pointClouds.isEmpty()) {
        primaryViewport->clearDisplay();
        primaryViewport->setTitle(tr("主视图"));
        updateRenderModeCombo();
        return;
    }

    QImage compositeImage;
    if (images.size() == 1) {
        compositeImage = images.constFirst();
    } else if (!images.isEmpty()) {
        int width = 0;
        int height = 0;
        for (const QImage& image : images) {
            width = qMax(width, image.width());
            height = qMax(height, image.height());
        }
        compositeImage = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
        compositeImage.fill(Qt::black);
        QPainter painter(&compositeImage);
        painter.setOpacity(1.0 / images.size());
        for (const QImage& image : images) {
            painter.drawImage(0, 0, image);
        }
    }

    if (!pointClouds.isEmpty()) {
        // ponytail: visibility changes rebuild one merged GPU payload; add per-source GPU buffers only if large-cloud
        // toggles become a bottleneck.
        PointCloudData merged;
        size_t pointCount = 0;
        bool keepColors = true;
        bool keepNormals = true;
        bool keepIntensities = true;
        bool keepLabels = true;
        for (const PointCloudData* cloud : pointClouds) {
            pointCount += cloud->size();
            keepColors = keepColors && cloud->hasColors();
            keepNormals = keepNormals && cloud->hasNormals();
            keepIntensities = keepIntensities && cloud->intensities.size() == cloud->size();
            keepLabels = keepLabels && cloud->hasLabels();
        }
        merged.points.reserve(pointCount);
        if (keepColors)
            merged.colors.reserve(pointCount);
        if (keepNormals)
            merged.normals.reserve(pointCount);
        if (keepIntensities)
            merged.intensities.reserve(pointCount);
        if (keepLabels)
            merged.labels.reserve(pointCount);
        for (const PointCloudData* cloud : pointClouds) {
            merged.points.insert(merged.points.end(), cloud->points.begin(), cloud->points.end());
            if (keepColors)
                merged.colors.insert(merged.colors.end(), cloud->colors.begin(), cloud->colors.end());
            if (keepNormals)
                merged.normals.insert(merged.normals.end(), cloud->normals.begin(), cloud->normals.end());
            if (keepIntensities)
                merged.intensities.insert(merged.intensities.end(), cloud->intensities.begin(),
                                          cloud->intensities.end());
            if (keepLabels)
                merged.labels.insert(merged.labels.end(), cloud->labels.begin(), cloud->labels.end());
        }

        DisplayData mergedData;
        mergedData.variant() = std::move(merged);
        mergedData.setMetadata(QVariantMap{{"dataSourceName", tr("主视图")}});
        updateRenderModeComboForData(*mergedData.pointCloudData());
        if (compositeImage.isNull()) {
            primaryViewport->displayData(mergedData);
        } else {
            primaryViewport->setDualData(compositeImage, mergedData,
                                         hasImageData ? ViewportWidget::DisplayMode::Auto2D
                                                      : ViewportWidget::DisplayMode::Auto3D);
        }
    } else {
        primaryViewport->displayImage(compositeImage);
        updateRenderModeCombo();
    }
    primaryViewport->setTitle(tr("主视图"));
    primaryViewport->setVisible(true);
}

QString MainWindow::ensureDataSourceViewport(const QString& dataSourceId, const QString& title) {
    if (!m_displayManager || dataSourceId.isEmpty()) {
        return QString();
    }

    const QString existingId = m_dataSourceViewportIds.value(dataSourceId);
    if (!existingId.isEmpty() && m_displayManager->viewport(existingId)) {
        return existingId;
    }

    const QString viewportId = m_displayManager->createViewport(title);

    if (ViewportWidget* viewport = m_displayManager->viewport(viewportId)) {
        viewport->setTitle(title);
        viewport->setVisible(true);
    }
    m_dataSourceViewportIds.insert(dataSourceId, viewportId);
    return viewportId;
}

void MainWindow::clearDataSourceViewports() {
    ViewportWidget* primaryViewport = primaryDataSourceViewport();
    if (!m_displayManager) {
        m_dataSourceViewportIds.clear();
        m_dataSourceDisplayData.clear();
        m_dataSourcePreviewImages.clear();
        m_hiddenDataSourceIds.clear();
        m_primaryDataSourceViewportId.clear();
        updateRenderModeCombo();
        return;
    }

    const QList<QString> viewportIds = m_dataSourceViewportIds.values();
    for (const QString& viewportId : viewportIds) {
        if (m_displayManager->viewport(viewportId)) {
            m_displayManager->destroyViewport(viewportId);
        }
    }
    if (primaryViewport) {
        primaryViewport->clearDisplay();
        primaryViewport->setTitle(tr("主视图"));
        primaryViewport->setVisible(true);
    }
    m_dataSourceViewportIds.clear();
    m_dataSourceDisplayData.clear();
    m_dataSourcePreviewImages.clear();
    m_hiddenDataSourceIds.clear();
    m_primaryDataSourceViewportId = primaryViewport ? primaryViewport->viewportId() : QString();
    updateRenderModeCombo();
}

// ========== 阶段 7: 工具面板搜索 ==========

QString MainWindow::stripCategoryPrefix(const QString& text) {
    // 去除 "00 - " 之类的数字前缀，但保持原排序
    // 匹配格式: "DD - " 或 "DD-DD - "
    QString result = text;
    static const QRegularExpression prefixRe(QStringLiteral("^\\d{2}\\s*-\\s*"));
    result.remove(prefixRe);
    return result;
}

void MainWindow::filterToolBox(const QString& text) {
    if (!m_toolBoxTree) {
        return;
    }

    const QString lower = text.trimmed().toLower();

    for (int i = 0; i < m_toolBoxTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* category = m_toolBoxTree->topLevelItem(i);
        if (category->data(0, Qt::UserRole).toString() != "category") {
            continue;
        }

        bool anyVisible = false;
        for (int j = 0; j < category->childCount(); ++j) {
            QTreeWidgetItem* child = category->child(j);
            bool match = lower.isEmpty() || child->text(0).toLower().contains(lower) ||
                         child->data(0, Qt::UserRole + 1).toString().toLower().contains(lower);
            child->setHidden(!match);
            if (match) {
                anyVisible = true;
            }
        }

        // 递归隐藏空分类
        category->setHidden(!anyVisible && !lower.isEmpty());

        // 搜索时自动展开匹配的分类
        if (anyVisible && !lower.isEmpty()) {
            category->setExpanded(true);
        }
    }
}

// ========== 阶段 8: 高级配置弹窗 ==========

void MainWindow::openAdvancedPluginConfig(const QString& instanceId) {
    if (instanceId.isEmpty() || !m_flowModules.contains(instanceId)) {
        return;
    }

    IModule* module = m_flowModules.value(instanceId);
    if (!module) {
        return;
    }

    // Keep the runtime-effective values for draft comparison and the sparse
    // project values for exact Undo (missing keys must not become JSON null).
    const QJsonObject effectiveParamsBefore = module->currentParams();
    Project* projectAtOpen = ProjectManager::instance().currentProject();
    const std::optional<ModuleInstance> instanceAtOpen =
        projectAtOpen ? projectAtOpen->moduleById(instanceId) : std::nullopt;
    const QJsonObject projectParamsBefore = instanceAtOpen ? instanceAtOpen->params : QJsonObject{};
    QPointer<Project> projectPtrAtOpen(projectAtOpen);

    // 阶段 1: 创建草稿模块（clone），配置控件绑定草稿，不直接修改运行模块
    IModule* draft = module->clone();
    if (!draft) {
        Logger::instance().warning(tr("该模块不支持克隆，无法打开配置"), "Config");
        return;
    }

    QWidget* configWidget = draft->createConfigWidget();
    if (!configWidget) {
        delete draft;
        Logger::instance().warning(tr("该模块没有配置选项"), "Config");
        return;
    }

    QString displayName = instanceId;
    QTreeWidgetItem* item = m_processTreeController ? m_processTreeController->instanceItem(instanceId) : nullptr;
    if (item) {
        displayName = item->text(0);
    }

    QDialog* dialog = new QDialog(this);
    dialog->setObjectName("PluginConfigDialog");
    dialog->setWindowTitle(tr("配置 - %1").arg(displayName));
    dialog->setMinimumSize(460, 480);
    dialog->setStyleSheet(pluginConfigDialogStyle(m_isDarkTheme));
    QVBoxLayout* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);
    addMeasurementConfigAction(layout, module->moduleId(), instanceId, dialog);
    configWidget->setObjectName("PluginConfigContent");
    applyPluginConfigTheme(configWidget, m_isDarkTheme);
    QScrollArea* scrollArea = new QScrollArea(dialog);
    scrollArea->setObjectName("PluginConfigScrollArea");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(configWidget);
    layout->addWidget(scrollArea, 1);

    // 阶段 1: 校验错误提示标签
    QLabel* errorLabel = new QLabel(dialog);
    errorLabel->setObjectName("PluginConfigErrorLabel");
    errorLabel->setStyleSheet(
        QString("color: %1; font-weight: bold; padding: 4px;").arg(m_isDarkTheme ? "#F87171" : "#DC2626"));
    errorLabel->setWordWrap(true);
    errorLabel->hide();
    layout->addWidget(errorLabel);

    QHBoxLayout* btnLayout = new QHBoxLayout();
    QPushButton* okBtn = new QPushButton(tr("确定"));
    okBtn->setObjectName("PluginConfigOkButton");
    okBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Confirm, 16, QColor("#16A34A")));
    QPushButton* cancelBtn = new QPushButton(tr("取消"));
    cancelBtn->setObjectName("PluginConfigCancelButton");
    cancelBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Cancel, 16, QColor("#DC2626")));
    btnLayout->addStretch();
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addLayout(btnLayout);

    // 校验、忙态复验和实际提交均在 dialog 仍打开时完成；拒绝时保留草稿。
    QPointer<IModule> modulePtr(module);
    QPointer<IModule> draftPtr(draft);
    connect(okBtn, &QPushButton::clicked, this,
            [this, modulePtr, draftPtr, errorLabel, dialog, effectiveParamsBefore, projectParamsBefore,
             projectPtrAtOpen, displayName, instanceId]() {
                if (!modulePtr || !draftPtr)
                    return;
                if (rejectBusyFlowEdit(tr("提交模块配置"))) {
                    errorLabel->setText(tr("运行或维护期间不能提交配置；草稿已保留"));
                    errorLabel->show();
                    return;
                }
                const QJsonObject draftParams = draftPtr->currentParams();
                QString error;
                if (!modulePtr->validateParams(draftParams, error)) {
                    errorLabel->setText(error);
                    errorLabel->show();
                    return;
                }

                Project* project = ProjectManager::instance().currentProject();
                const std::optional<ModuleInstance> current = project ? project->moduleById(instanceId) : std::nullopt;
                if (!project || project != projectPtrAtOpen.data() || !current || !m_paramUndoStack) {
                    errorLabel->setText(tr("工程或模块已关闭，配置未提交"));
                    errorLabel->show();
                    return;
                }
                QJsonObject projectParamsAfter = projectParamsBefore;
                QSet<QString> candidateKeys;
                for (auto it = effectiveParamsBefore.constBegin(); it != effectiveParamsBefore.constEnd(); ++it)
                    candidateKeys.insert(it.key());
                for (auto it = draftParams.constBegin(); it != draftParams.constEnd(); ++it)
                    candidateKeys.insert(it.key());
                for (const QString& key : candidateKeys) {
                    if (key.startsWith('_'))
                        continue;
                    const bool beforePresent = effectiveParamsBefore.contains(key);
                    const bool afterPresent = draftParams.contains(key);
                    if (beforePresent == afterPresent &&
                        (!beforePresent || effectiveParamsBefore.value(key) == draftParams.value(key)))
                        continue;
                    if (afterPresent)
                        projectParamsAfter.insert(key, draftParams.value(key));
                    else
                        projectParamsAfter.remove(key);
                }
                if (projectParamsAfter != projectParamsBefore) {
                    auto applied = std::make_shared<bool>(false);
                    m_paramUndoStack->push(new SetModuleParamCommand(
                        modulePtr, project, instanceId, projectParamsBefore, projectParamsAfter, m_inspectorPanel,
                        [this, displayName, applied](bool succeeded) {
                            *applied = succeeded;
                            if (!succeeded)
                                statusBar()->showMessage(tr("配置 %1 已过期，未应用任何更改").arg(displayName), 5000);
                        }));
                    if (!*applied) {
                        errorLabel->setText(tr("配置期间参数已被其他入口修改；草稿仍保留，请重新确认"));
                        errorLabel->show();
                        return;
                    }
                }

                if (draftParams.value("_clearPointsRequested").toBool())
                    QMetaObject::invokeMethod(modulePtr, "clearPointsPublic", Qt::DirectConnection);

                if (m_inspectorPanel && m_selectedModuleId == instanceId) {
                    QPointer<ModuleInspectorPanel> inspector = m_inspectorPanel;
                    QMetaObject::invokeMethod(
                        this,
                        [inspector]() {
                            if (inspector)
                                inspector->refreshFromModule();
                        },
                        Qt::QueuedConnection);
                }
                m_modulesNeedSync = true;
                Logger::instance().info(tr("模块参数已更新：%1").arg(displayName), "Config");
                dialog->accept();
            });
    connect(cancelBtn, &QPushButton::clicked, dialog, &QDialog::reject);

    // exec() 返回后 delete dialog，级联销毁所有子对象（含 configWidget、scrollArea 等）。
    const int result = dialog->exec();
    delete dialog;
    delete draft;

    if (result != QDialog::Accepted) {
        Logger::instance().info(tr("已取消配置修改：%1").arg(displayName), "Config");
    }
}

// ========== 阶段 8: 聚焦模式 ==========

void MainWindow::toggleFocusMode() {
    m_focusMode = !m_focusMode;

    // 高: 抑制 visibilityChanged 在 hide/show 时设 userClosed
    m_toolPanelSuppressSignal = true;

    if (m_focusMode) {
        m_focusSavedMainSizes = m_mainSplitter->sizes();
        m_focusSavedRightTopSizes = m_rightTopSplitter->sizes();
        m_focusSavedRightSplitterSizes = m_rightSplitter->sizes();
        m_focusSavedLogVisible = m_logDock ? m_logDock->isVisible() : false;

        if (m_toolBoxDock)
            m_toolBoxDock->setVisible(false);
        if (m_processTabWidget)
            m_processTabWidget->parentWidget()->setVisible(false);
        if (m_inspectorPanel)
            m_inspectorPanel->setVisible(false);
        if (m_logDock)
            m_logDock->setVisible(false);
    } else {
        if (m_toolBoxDock && !m_toolPanelUserClosed)
            m_toolBoxDock->setVisible(true);
        if (m_processTabWidget)
            m_processTabWidget->parentWidget()->setVisible(true);
        if (m_inspectorPanel && !m_inspectorClosed)
            m_inspectorPanel->setVisible(true);
        // 高: 恢复日志面板到进入焦点前状态
        if (m_logDock)
            m_logDock->setVisible(m_focusSavedLogVisible);

        if (!m_focusSavedMainSizes.isEmpty())
            m_mainSplitter->setSizes(m_focusSavedMainSizes);
        if (!m_focusSavedRightTopSizes.isEmpty())
            m_rightTopSplitter->setSizes(m_focusSavedRightTopSizes);
        if (!m_focusSavedRightSplitterSizes.isEmpty())
            m_rightSplitter->setSizes(m_focusSavedRightSplitterSizes);
    }

    m_toolPanelSuppressSignal = false;
}

void MainWindow::_phase8_openAdvancedPluginConfig(const QString& instanceId) {
    openAdvancedPluginConfig(instanceId);
}

void MainWindow::_phase8_toggleFocusMode() {
    toggleFocusMode();
}

void MainWindow::clearExecutionHighlight(QTreeWidgetItem* item) {
    if (!item) {
        return;
    }
    // 使用 data role 替代 setBackground — 清除状态
    item->setData(0, Qt::UserRole + 5, QString());
    item->setIcon(1, QIcon());
    if (m_flowCanvas) {
        m_flowCanvas->setNodeExecutionState(item->data(0, Qt::UserRole + 1).toString(), QString(), QString());
    }
}

void MainWindow::setProcessItemStatus(QTreeWidgetItem* item, const QString& status, const QString& timeText) {
    if (!item) {
        return;
    }
    // P1: 用 DecorationRole 渲染状态点，而非只存 UserRole
    item->setData(0, Qt::UserRole + 5, status);
    if (m_flowCanvas) {
        m_flowCanvas->setNodeExecutionState(item->data(0, Qt::UserRole + 1).toString(), status, timeText);
    }
    // 创建彩色状态点图标
    QColor dotColor;
    if (status == "running")
        dotColor = QColor("#3B82F6"); // 蓝色
    else if (status == "success")
        dotColor = QColor("#22C55E"); // 绿色
    else if (status == "failure")
        dotColor = QColor("#EF4444"); // 红色
    else if (status == "dirty")
        dotColor = QColor("#F59E0B"); // 琥珀色
    else if (status == "paused")
        dotColor = QColor("#D97706"); // 深琥珀色（断点暂停）
    else if (status == "skipped")
        dotColor = QColor("#9CA3AF"); // 灰色（已跳过/已禁用）
    if (dotColor.isValid()) {
        QPixmap pm(10, 10);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(dotColor);
        p.setPen(Qt::NoPen);
        p.drawEllipse(0, 0, 10, 10);
        item->setIcon(1, QIcon(pm));
    } else {
        item->setIcon(1, QIcon());
    }
    if (!timeText.isEmpty()) {
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(1, timeText);
    }
}

void MainWindow::showProcessModuleOutput(QTreeWidgetItem* item, bool userInitiated) {
    if (!item || item->data(0, Qt::UserRole).toString() != QStringLiteral("flow_item")) {
        return;
    }

    const QString instanceId = item->data(0, Qt::UserRole + 1).toString();
    if (instanceId.isEmpty()) {
        return;
    }

    // 统一走 selectModule 入口
    selectModule(instanceId, userInitiated, userInitiated);
}

void MainWindow::pushParamCommand(const QString& instanceId, const QString& key, const QVariant& value) {
    if (!m_paramUndoStack)
        return;

    Project* project = ProjectManager::instance().currentProject();
    IModule* module = m_flowModules.value(instanceId, nullptr);

    const std::optional<ModuleInstance> instance = project ? project->moduleById(instanceId) : std::nullopt;
    if (!module || !instance)
        return;
    const QJsonObject before = instance->params;
    QJsonObject after = before;
    after.insert(key, QJsonValue::fromVariant(value));
    auto* cmd = new SetModuleParamCommand(
        module, project, instanceId, before, after, m_inspectorPanel, [this, key](bool succeeded) {
            if (!succeeded)
                statusBar()->showMessage(tr("参数 %1 的撤销已失效，工程未修改").arg(key), 5000);
        });
    m_paramUndoStack->push(cmd);
}

bool MainWindow::rejectBusyFlowEdit(const QString& action) {
    if (!RunEngine::instance().isBusy())
        return false;
    statusBar()->showMessage(tr("运行或维护期间不能%1").arg(action), 5000);
    return true;
}

void MainWindow::markModuleDirty(const QString& instanceId) {
    if (instanceId.isEmpty()) {
        return;
    }

    QSet<QString> affected{instanceId};
    if (Project* project = ProjectManager::instance().currentProject()) {
        const QList<ModuleConnection> connections = project->connections();
        if (connections.isEmpty()) {
            bool downstream = false;
            for (const ModuleInstance& module : project->modules()) {
                downstream = downstream || module.id == instanceId;
                if (downstream) {
                    affected.insert(module.id);
                }
            }
        } else {
            QStringList pending{instanceId};
            while (!pending.isEmpty()) {
                const QString source = pending.takeFirst();
                for (const ModuleConnection& connection : connections) {
                    if (connection.fromModuleId == source && !affected.contains(connection.toModuleId)) {
                        affected.insert(connection.toModuleId);
                        pending.append(connection.toModuleId);
                    }
                }
            }
        }
    }

    RunEngine& engine = RunEngine::instance();
    for (const QString& id : affected) {
        m_dirtyModuleIds.insert(id);
        m_moduleExecutionTimes.remove(id);
        engine.invalidateModuleOutput(id);
        if (m_processTreeController) {
            setProcessItemStatus(m_processTreeController->instanceItem(id), QStringLiteral("dirty"), tr("待运行"));
        }
    }
    if (m_inspectorPanel && affected.contains(m_selectedModuleId)) {
        m_inspectorPanel->setDirty(true);
    }
}

void MainWindow::registerFlowModule(const QString& instanceId, IModule* module) {
    // Test/support integrations may register an already-created ModuleBase directly rather than through
    // PluginManager::createModule. Preserve the project-declared type identity in that case.
    if (ModuleBase* moduleBase = qobject_cast<ModuleBase*>(module)) {
        if (!moduleBase->property("deeplux.pluginId").isValid()) {
            const auto instance = ProjectManager::instance().currentProject()
                                      ? ProjectManager::instance().currentProject()->moduleById(instanceId)
                                      : std::nullopt;
            if (instance)
                moduleBase->setProperty("deeplux.pluginId", instance->moduleId);
        }
    }
    m_flowModules.insert(instanceId, module);
}

void MainWindow::setInspectorVisibility(bool visible) {
    if (!m_inspectorPanel)
        return;

    if (!visible) {
        if (m_inspectorClosed && !m_inspectorPanel->isVisible())
            return;
        m_inspectorClosed = true;
        m_inspectorPanel->setVisible(false);
        m_inspectorPanel->clear();
        return;
    }

    const bool wasClosed = m_inspectorClosed;
    m_inspectorClosed = false;
    m_inspectorPanel->setVisible(true);
    if (!wasClosed)
        return;

    Project* project = ProjectManager::instance().currentProject();
    if (project && !m_selectedModuleId.isEmpty() && project->findModule(m_selectedModuleId))
        selectModule(m_selectedModuleId, true);
    else {
        m_inspectorPanel->clear();
        adaptInspectorLayout();
    }
}

void MainWindow::resetInspectorClosed() {
    m_inspectorClosed = false;
    if (m_inspectorPanel) {
        m_inspectorPanel->setVisible(true);
    }
}

void MainWindow::selectModule(const QString& instanceId, bool revealInspector, bool force) {
    // 如果检查器已固定且 instanceId 不同，不切换
    if (!force && m_inspectorPanel && m_inspectorPanel->isPinned() && !m_selectedModuleId.isEmpty() &&
        instanceId != m_selectedModuleId) {
        const QString pinnedId = m_selectedModuleId;
        QTimer::singleShot(0, this, [this, pinnedId]() {
            if (!m_inspectorPanel || !m_inspectorPanel->isPinned() || m_selectedModuleId != pinnedId) {
                return;
            }
            if (m_processTreeController) {
                if (QTreeWidgetItem* item = m_processTreeController->instanceItem(pinnedId)) {
                    QSignalBlocker blocker(m_processTree);
                    m_processTreeController->setCurrentItem(item);
                }
            }
            if (m_flowCanvas) {
                QSignalBlocker blocker(m_flowCanvas);
                m_flowCanvas->selectNode(pinnedId);
            }
        });
        return;
    }

    // 如果检查器已关闭且用户显式重新打开（revealInspector=true）
    if (m_inspectorClosed && revealInspector) {
        m_inspectorClosed = false;
        if (m_inspectorPanel) {
            m_inspectorPanel->setVisible(true);
        }
        adaptInspectorLayout();
    }

    // 如果检查器已关闭，不自动展开
    if (m_inspectorClosed && !revealInspector) {
        m_selectedModuleId = instanceId;
        if (m_flowCanvas) {
            m_flowCanvas->scene()->clearSelection();
            FlowNodeItem* nodeItem = m_flowCanvas->nodeItem(instanceId);
            if (nodeItem) {
                QSignalBlocker blocker(m_flowCanvas);
                nodeItem->setSelected(true);
            }
        }
        return;
    }

    m_selectedModuleId = instanceId;

    if (IModule* module = m_flowModules.value(instanceId, nullptr);
        module && module->moduleId() == QStringLiteral("com.deeplux.plugin.measurementinput")) {
        m_activeMeasurementInputId = instanceId;
    }

    if (instanceId.isEmpty()) {
        // 清空选择（含关闭检查器路径）：同步清除测量叠加，避免旧结果残留
        if (m_inspectorPanel) {
            m_inspectorPanel->clear();
        }
        if (m_flowCanvas) {
            m_flowCanvas->scene()->clearSelection();
        }
        clearMeasurementOverlays();
        return;
    }

    QTreeWidgetItem* selectedTreeItem = nullptr;

    // 同步流程树选中（用 QSignalBlocker 防止递归选择）
    if (m_processTreeController) {
        selectedTreeItem = m_processTreeController->instanceItem(instanceId);
        if (selectedTreeItem) {
            QSignalBlocker blocker(m_processTree);
            m_processTreeController->setCurrentItem(selectedTreeItem);
        }
    }

    // 同步画布选中（用 QSignalBlocker 防止递归选择）
    if (m_flowCanvas) {
        FlowNodeItem* nodeItem = m_flowCanvas->nodeItem(instanceId);
        if (nodeItem) {
            QSignalBlocker blocker(m_flowCanvas);
            m_flowCanvas->scene()->clearSelection();
            nodeItem->setSelected(true);
        }
    }

    // 更新检查器
    if (m_inspectorPanel) {
        IModule* module = m_flowModules.value(instanceId, nullptr);
        if (module) {
            // P1-6: 直接用 moduleId() 查询，pluginInfo() 内部支持 id/name/大小写多级匹配
            const QString moduleId = module->moduleId();
            PluginInfo info = PluginManager::instance().pluginInfo(moduleId);
            m_inspectorPanel->setModule(module, instanceId, info);
            const bool dirty = m_dirtyModuleIds.contains(instanceId);
            m_inspectorPanel->setDirty(dirty);

            // 阶段 B 复核: 反映启用/断点/备注到检查器
            if (Project* project = ProjectManager::instance().currentProject()) {
                if (auto inst = project->moduleById(instanceId)) {
                    m_inspectorPanel->setRuntimeState(inst->enabled, inst->breakpoint, inst->note);
                }
            }

            // 恢复该节点最近一次的真实执行状态；失败节点可能没有有效图像输出。
            const ImageData output = RunEngine::instance().moduleOutput(instanceId);
            const QString executionState =
                selectedTreeItem ? selectedTreeItem->data(0, Qt::UserRole + 5).toString() : QString();
            const bool hasCompletedState =
                executionState == QStringLiteral("success") || executionState == QStringLiteral("failure");
            if (!dirty && (hasCompletedState || output.isValid())) {
                int elapsed = m_moduleExecutionTimes.value(instanceId, 0);
                m_inspectorPanel->setOutput(output, executionState != QStringLiteral("failure"), elapsed);
            }

            if (revealInspector) {
                // 确保检查器可见（如果被折叠则展开）
                m_inspectorPanel->setVisible(true);
                adaptInspectorLayout();
            }
        } else {
            m_inspectorPanel->clear();
        }
    }

    // 同时显示模块输出到主视图（传入模块身份，记录模块→视口归属）
    const ImageData output = RunEngine::instance().moduleOutput(instanceId);
    if (!m_dirtyModuleIds.contains(instanceId) && output.isValid()) {
        displayImage(output, QString(), instanceId);
    }
    // 切换模块后按当前选中支路刷新测量叠加，避免旧支路叠加残留
    // （选中节点不属于任何测量支路时会清除叠加）。
    updateMeasurementResultOnOverlay();
}

// ===== 阶段 9: 自适应布局与状态保存 =====

void MainWindow::saveSettings() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "DeepLux", "DeepLuxVision");

    // 主窗口尺寸和位置
    settings.beginGroup("MainWindow");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("state", saveState());
    settings.endGroup();

    // Splitter 状态
    settings.beginGroup("Splitters");
    if (m_mainSplitter) {
        settings.setValue("mainSplitter", m_mainSplitter->saveState());
    }
    if (m_rightSplitter) {
        settings.setValue("rightSplitter", m_rightSplitter->saveState());
    }
    if (m_rightTopSplitter) {
        settings.setValue("rightTopSplitter", m_rightTopSplitter->saveState());
    }
    settings.endGroup();

    // 检查器状态
    settings.beginGroup("Inspector");
    if (m_inspectorPanel) {
        settings.setValue("mode", static_cast<int>(m_inspectorPanel->layoutMode()));
        settings.setValue("pinned", m_inspectorPanel->isPinned());
        settings.setValue("userOverride", m_inspectorPanel->userOverrideMode());
    }
    settings.setValue("closed", m_inspectorClosed);
    // 检查器宽度
    if (m_rightTopSplitter) {
        const auto sizes = m_rightTopSplitter->sizes();
        if (sizes.size() >= 3) {
            settings.setValue("width", sizes.at(2));
        }
    }
    settings.endGroup();

    // 底部面板高度
    settings.beginGroup("BottomPanel");
    if (m_rightSplitter) {
        const auto sizes = m_rightSplitter->sizes();
        if (sizes.size() >= 2) {
            settings.setValue("height", sizes.at(1));
        }
    }
    settings.endGroup();
}

void MainWindow::loadSettings() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "DeepLux", "DeepLuxVision");

    // 主窗口尺寸和位置
    settings.beginGroup("MainWindow");
    const QByteArray geometry = settings.value("geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const QByteArray state = settings.value("state").toByteArray();
    if (!state.isEmpty()) {
        restoreState(state);
    }
    settings.endGroup();

    // Splitter 状态
    settings.beginGroup("Splitters");
    if (m_mainSplitter) {
        const QByteArray data = settings.value("mainSplitter").toByteArray();
        if (!data.isEmpty()) {
            m_mainSplitter->restoreState(data);
        }
    }
    if (m_rightSplitter) {
        const QByteArray data = settings.value("rightSplitter").toByteArray();
        if (!data.isEmpty()) {
            m_rightSplitter->restoreState(data);
        }
    }
    if (m_rightTopSplitter) {
        const QByteArray data = settings.value("rightTopSplitter").toByteArray();
        if (!data.isEmpty()) {
            m_rightTopSplitter->restoreState(data);
        }
    }
    settings.endGroup();

    // 检查器状态
    settings.beginGroup("Inspector");
    m_inspectorClosed = settings.value("closed", false).toBool();
    if (m_inspectorPanel) {
        const int modeInt = settings.value("mode", static_cast<int>(ModuleInspectorPanel::LayoutMode::Docked)).toInt();
        m_inspectorPanel->setLayoutMode(static_cast<ModuleInspectorPanel::LayoutMode>(modeInt));
        m_inspectorPanel->setPinned(settings.value("pinned", false).toBool());
        m_inspectorPanel->setUserOverrideMode(settings.value("userOverride", false).toBool());
    }
    settings.endGroup();

    // 如果检查器上次被关闭，则隐藏
    if (m_inspectorClosed && m_inspectorPanel) {
        m_inspectorPanel->setVisible(false);
    }

    // 自适应布局
    adaptInspectorLayout();
}

void MainWindow::adaptInspectorLayout() {
    if (!m_inspectorPanel) {
        return;
    }

    const int windowWidth = width();
    const int minMainView = 520; // 主视图最小宽度
    const LayoutMetrics metrics = ThemeManager::layoutMetrics();

    // ===== 窄屏自适应：在判断布局前先压缩流程面板 / 隐藏工具面板 =====
    // P1/P2: 阈值从 900 提到 1100，确保 1024px 窗口也能隐藏工具面板
    // P2: 如果用户显式关闭工具面板，不自动重新显示
    if (m_toolBoxDock) {
        const bool toolShouldBeVisible = windowWidth >= 1100 && !m_toolPanelUserClosed;
        if (m_toolBoxDock->isVisible() != toolShouldBeVisible) {
            // P2: 抑制 visibilityChanged 设 userClosed（这是自适应隐藏，不是用户操作）
            m_toolPanelSuppressSignal = true;
            m_toolBoxDock->setVisible(toolShouldBeVisible);
            m_toolPanelSuppressSignal = false;
        }
    }

    // 用户覆盖只约束检查器自身，不应禁用窗口级的工具面板响应式规则。
    if (m_inspectorClosed || m_inspectorPanel->userOverrideMode()) {
        return;
    }

    // 计算左侧工具面板 + 流程面板的宽度
    int leftWidth = 0;
    if (m_mainSplitter && m_mainSplitter->count() >= 1) {
        leftWidth = m_mainSplitter->sizes().value(0, metrics.toolPanelWidth);
    }
    // 工具面板隐藏后，左侧宽度应视为 0
    if (m_toolBoxDock && !m_toolBoxDock->isVisible()) {
        leftWidth = 0;
    }

    // 窗口宽度 < 1200px：保留足够宽度完整显示 220px 流程节点。
    int flowPanelWidth = 0;
    if (m_rightTopSplitter && m_rightTopSplitter->count() >= 1) {
        flowPanelWidth = m_rightTopSplitter->sizes().value(0, 300);
    }
    const int desiredFlowWidth = (windowWidth < 1200) ? 260 : metrics.flowPanelWidth;
    QWidget* procPanel = findChild<QWidget*>("ProcessPanelWidget");
    if (procPanel) {
        procPanel->setMinimumWidth(desiredFlowWidth);
    }
    if (m_rightTopSplitter && m_rightTopSplitter->count() >= 3 && flowPanelWidth != desiredFlowWidth) {
        QList<int> sizes = m_rightTopSplitter->sizes();
        const int delta = desiredFlowWidth - sizes.value(0, 0);
        // 从主视图借出或归还宽度，保持 splitter 总宽不变
        if (sizes.size() >= 2) {
            sizes[0] = desiredFlowWidth;
            sizes[1] = qMax(minMainView, sizes.value(1, 0) - delta);
            m_rightTopSplitter->setSizes(sizes);
        }
        flowPanelWidth = desiredFlowWidth;
    }

    // 可用宽度 = 窗口宽度 - 左侧 - 流程面板 - splitter handle 宽度
    const int handleW = metrics.splitterHandleWidth * 3;
    const int available = windowWidth - leftWidth - flowPanelWidth - handleW;

    ModuleInspectorPanel::LayoutMode newMode = m_inspectorPanel->layoutMode();

    if (available < minMainView + 32) {
        // 小窗口：浮动模式
        newMode = ModuleInspectorPanel::LayoutMode::Floating;
    } else if (available < minMainView + metrics.inspectorMinWidth) {
        // 中等宽度：折叠为 32px 侧栏
        newMode = ModuleInspectorPanel::LayoutMode::Collapsed;
    } else {
        // 宽屏：正常停靠
        newMode = ModuleInspectorPanel::LayoutMode::Docked;
    }

    if (newMode != m_inspectorPanel->layoutMode()) {
        // Floating 模式：从 splitter 移除检查器，插入命名占位以便后续恢复
        if (newMode == ModuleInspectorPanel::LayoutMode::Floating && m_rightTopSplitter &&
            m_rightTopSplitter->indexOf(m_inspectorPanel) >= 0) {
            QWidget* placeholder = new QWidget();
            placeholder->setObjectName(QStringLiteral("InspectorPlaceholder"));
            m_rightTopSplitter->replaceWidget(m_rightTopSplitter->indexOf(m_inspectorPanel), placeholder);
        }
        // Docked/Collapsed 模式：如果检查器不在 splitter 中，加回
        if ((newMode == ModuleInspectorPanel::LayoutMode::Docked ||
             newMode == ModuleInspectorPanel::LayoutMode::Collapsed) &&
            m_rightTopSplitter && m_rightTopSplitter->indexOf(m_inspectorPanel) < 0) {
            // 先找到占位控件并删除，避免占位累积
            for (int i = 0; i < m_rightTopSplitter->count(); ++i) {
                QWidget* w = m_rightTopSplitter->widget(i);
                if (w && w->objectName() == QStringLiteral("InspectorPlaceholder")) {
                    // 用检查器替换占位，随后删除占位
                    m_rightTopSplitter->replaceWidget(i, m_inspectorPanel);
                    w->deleteLater();
                    break;
                }
            }
            if (m_rightTopSplitter->indexOf(m_inspectorPanel) < 0) {
                m_rightTopSplitter->addWidget(m_inspectorPanel);
            }
        }
        m_inspectorPanel->setLayoutMode(newMode);
        // 高: 折叠/展开后重分配 splitter 空间
        if (m_rightTopSplitter) {
            QList<int> sizes = m_rightTopSplitter->sizes();
            const int inspIdx = m_rightTopSplitter->indexOf(m_inspectorPanel);
            if (inspIdx >= 0 && inspIdx < sizes.size()) {
                if (newMode == ModuleInspectorPanel::LayoutMode::Collapsed) {
                    int freed = sizes[inspIdx] - 32;
                    sizes[inspIdx] = 32;
                    if (sizes.size() > 1)
                        sizes[1] += freed; // 给主视图
                } else if (newMode == ModuleInspectorPanel::LayoutMode::Docked) {
                    const int added = metrics.inspectorWidth - sizes[inspIdx];
                    sizes[inspIdx] = metrics.inspectorWidth;
                    if (sizes.size() > 1)
                        sizes[1] = qMax(620, sizes[1] - added);
                }
                m_rightTopSplitter->setSizes(sizes);
            }
        }
    }

    // 确保主视图宽度在任何模式下不低于 620px
    if (m_rightTopSplitter && m_rightTopSplitter->count() >= 2) {
        QList<int> sizes = m_rightTopSplitter->sizes();
        if (sizes.size() >= 2 && sizes.value(1, 0) < minMainView) {
            // 修正：将主视图（index 1）设为至少 620px
            int deficit = minMainView - sizes.value(1, 0);
            if (sizes.value(0, 0) > minMainView + deficit) {
                sizes[0] = sizes.value(0, 0) - deficit;
                sizes[1] = minMainView;
                m_rightTopSplitter->setSizes(sizes);
            }
        }
    }
}

namespace {
bool guiUserHasPermission(const QString& permission) {
    const UserManager& users = UserManager::instance();
    return users.isLoggedIn() && users.currentUser().type != UserType::Guest && users.hasPermission(permission);
}

void showGuiPermissionDenied(QWidget* parent, const QString& action) {
    QMessageBox::warning(parent, QObject::tr("权限不足"),
                         QObject::tr("您没有权限执行此操作：%1。", "Permission denied: %1").arg(action));
}

// 阶段1 §3（计划 §6.3）：GUI 侧未保存三态询问——保存后继续/放弃后继续/取消。
// 无未保存修改时返回值无关紧要（共享判定必然放行）；实际保护判定由
// ProjectManager::resolveUnsavedForSwitch 执行（GUI/CLI/Agent 同一逻辑源）。
ProjectManager::UnsavedPolicy askUnsavedPolicy(QWidget* parent, const QString& title) {
    Project* current = ProjectManager::instance().currentProject();
    if (!current || !current->isModified())
        return ProjectManager::UnsavedPolicy::Cancel;

    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setIcon(QMessageBox::Warning);
    box.setText(QObject::tr("工程「%1」有未保存的修改，是否先保存？").arg(current->name()));
    QPushButton* saveBtn = nullptr;
    if (guiUserHasPermission(QStringLiteral("Save"))) {
        saveBtn = box.addButton(QObject::tr("保存后继续"), QMessageBox::AcceptRole);
    } else {
        box.setInformativeText(QObject::tr("当前用户没有保存权限，只能放弃修改或取消操作。"));
    }
    QPushButton* discardBtn = box.addButton(QObject::tr("放弃后继续"), QMessageBox::DestructiveRole);
    box.addButton(QObject::tr("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == saveBtn)
        return ProjectManager::UnsavedPolicy::Save;
    if (box.clickedButton() == discardBtn)
        return ProjectManager::UnsavedPolicy::Discard;
    return ProjectManager::UnsavedPolicy::Cancel; // 关闭对话框等同取消——默认保留当前工程
}

// 阶段1 复审2 R2：返回 false = 应中止整个操作（用户取消「另存为」或保存失败,零副作用）;
// 返回 true 时 *policyOut 为交给 *Checked 入口执行的策略。
// 「保存后继续」对无保存路径的新工程经另存为兑现语义;已有路径的保存由共享守卫完成。
bool prepareUnsavedSwitch(QWidget* parent, const QString& title, ProjectManager::UnsavedPolicy* policyOut) {
    const ProjectManager::UnsavedPolicy policy = askUnsavedPolicy(parent, title);
    *policyOut = policy;
    if (policy != ProjectManager::UnsavedPolicy::Save)
        return true; // Cancel → *Checked 按策略中止(PROJECT_UNSAVED);Discard → 放行
    if (!guiUserHasPermission(QStringLiteral("Save"))) {
        showGuiPermissionDenied(parent, QObject::tr("保存工程"));
        return false;
    }
    Project* current = ProjectManager::instance().currentProject();
    if (!current || !current->isModified() || !current->filePath().isEmpty())
        return true; // 无工程/无修改/已有路径：共享守卫可完成保存语义
    // 无保存路径的新工程 → 「另存为」
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                  .filePath(current->name() + QStringLiteral(".dproj"));
    const QString path = QFileDialog::getSaveFileName(parent, QObject::tr("保存工程（另存为）"), suggested,
                                                      QObject::tr("工程文件 (*.dproj)"));
    if (path.isEmpty())
        return false; // 用户取消另存为 → 中止切换（等同取消,零副作用）
    if (!guiUserHasPermission(QStringLiteral("Save"))) {
        showGuiPermissionDenied(parent, QObject::tr("保存工程"));
        return false;
    }
    if (!ProjectManager::instance().saveProject(path)) {
        QMessageBox::critical(parent, title, QObject::tr("保存失败，操作已中止；当前工程保留。"));
        return false; // 保存失败不继续
    }
    return true; // 已落盘（修改标记清零）,共享守卫将放行
}
} // namespace

void MainWindow::closeEvent(QCloseEvent* event) {
    FlowExecutionService& service = FlowExecutionService::instance();
    // 后台执行（设计 §5.3 #7 / §8.1）：以 Service 状态判断是否有 job——Cycle 两轮之间
    // 引擎可能已 Idle，但 Service 仍 Running。有 job 时只请求停止并忽略本次关闭，
    // 由 jobFinished 再次触发 close；不得在 GUI 线程同步等待（gui 模块可能正阻塞回 GUI）。
    if (service.isActive() || RunEngine::instance().isBusy()) {
        m_closeWhenRunFinishes = true;
        if (service.isActive()) {
            m_closeAwaitingServiceJob = true;
            const auto retryDeferredClose = [this]() {
                if (!m_closeAwaitingServiceJob)
                    return;
                m_closeAwaitingServiceJob = false;
                m_closeWhenRunFinishes = false;
                close();
            };
            connect(&service, &FlowExecutionService::jobFinished, this,
                    [retryDeferredClose](FlowExecutionService::JobType, bool) { retryDeferredClose(); });
            connect(&service, &FlowExecutionService::jobFailed, this,
                    [retryDeferredClose](FlowExecutionService::JobType, const QString&) { retryDeferredClose(); });
            service.stop();
        } else {
            // 无 Service job 的同步引擎运行（CLI/测试兼容路径）：沿用取消请求。
            RunEngine::instance().requestCancellation();
        }
        event->ignore();
        return;
    }
    // 阶段1 §3：退出也是"关闭"路径——未保存修改须经用户三态决策；
    // 保存失败、取消或另存为被取消都不退出（默认保留当前工程）
    if (ProjectManager::instance().hasUnsavedChanges()) {
        ProjectManager::UnsavedPolicy policy;
        if (!prepareUnsavedSwitch(this, tr("退出"), &policy)) {
            event->ignore();
            return;
        }
        QString errorCode, errorMessage;
        if (!ProjectManager::instance().resolveUnsavedForSwitch(policy, &errorCode, &errorMessage)) {
            if (errorCode == QLatin1String("SAVE_FAILED"))
                QMessageBox::critical(this, tr("退出"), errorMessage);
            event->ignore(); // PROJECT_UNSAVED(取消)与保存失败都不退出
            return;
        }
    }
    CameraManager& cameraManager = CameraManager::instance();
    if (!m_cameraShutdownReady && cameraManager.isShutdownComplete())
        m_cameraShutdownReady = true;
    if (!m_cameraShutdownReady) {
        if (!m_cameraShutdownStarted) {
            m_cameraShutdownStarted = true;
            if (m_cameraShutdownConnection)
                disconnect(m_cameraShutdownConnection);
            if (m_cameraShutdownFailureConnection)
                disconnect(m_cameraShutdownFailureConnection);
            m_cameraShutdownConnection =
                connect(&CameraManager::instance(), &CameraManager::shutdownCompleted, this, [this]() {
                    m_cameraShutdownStarted = false;
                    m_cameraShutdownReady = true;
                    QTimer::singleShot(0, this, &QWidget::close);
                });
            m_cameraShutdownFailureConnection =
                connect(&CameraManager::instance(), &CameraManager::cameraOperationSettled, this,
                        [this](const QString& deviceId, quint64, const QString&, const QString& status,
                               const QString& errorCode) {
                            const bool cleanupFailed =
                                status == QLatin1String("error") ||
                                (status == QLatin1String("cancelled") &&
                                 CameraManager::instance().cameraState(deviceId) == CameraState::Error);
                            if (m_cameraShutdownStarted && cleanupFailed) {
                                m_cameraShutdownStarted = false;
                                Logger::instance().error(QString("Camera shutdown is still pending: %1").arg(errorCode),
                                                         "Camera");
                            }
                        });
            cameraManager.shutdownAsync();
        }
        // shutdownAsync() may finish synchronously when there are no active
        // cameras. Keep the first close request deferred in either case; the
        // completion callback schedules a second close with readiness latched.
        event->ignore();
        return;
    }
    m_cameraShutdownReady = true;
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // 设计 §7.5：窗口由不可见变为可见时恢复帧捕获并强制 flush 最新帧。
    updateDisplayCaptureForVisibility();
    if (m_imageCoalescer && FlowExecutionService::instance().isActive()) {
        m_imageCoalescer->forceFlush();
    }
}

void MainWindow::hideEvent(QHideEvent* event) {
    // 设计 §7.5：无显示消费者（不可见/最小化）时关闭帧捕获。
    RunEngine::instance().setDisplayCaptureEnabled(false);
    QMainWindow::hideEvent(event);
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event && event->type() == QEvent::WindowStateChange) {
        updateDisplayCaptureForVisibility();
        if (isVisible() && !isMinimized() && m_imageCoalescer && FlowExecutionService::instance().isActive()) {
            m_imageCoalescer->forceFlush();
        }
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (QToolBar* toolbar = findChild<QToolBar*>(QStringLiteral("MainToolBar"))) {
        toolbar->setToolButtonStyle(width() < 1180 ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
    if (QWidget* processControls = findChild<QWidget*>(QStringLiteral("ProcessControlBar"))) {
        const Qt::ToolButtonStyle style = width() < 1200 ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon;
        for (QToolButton* button : processControls->findChildren<QToolButton*>()) {
            button->setToolButtonStyle(style);
        }
    }
    // 窗口宽度变化时判断是否需要切换检查器模式
    adaptInspectorLayout();
}

void MainWindow::displayImage(const ImageData& image, const QString& label, const QString& moduleId) {
    Q_UNUSED(label);
    // 如果 ImageData 元数据中包含 PointCloudData，走 3D 显示路径（不参与 2D 测量叠加绑定）
    auto cloud = MeasurementData::pointCloud(image, nullptr);
    if (cloud && !cloud->isEmpty()) {
        m_displayingModuleId.clear();
        m_displayingImage2D = false;
        DisplayData data;
        data.variant() = std::move(*cloud);
        data.setTimestamp(QDateTime::currentMSecsSinceEpoch());
        if (m_displayManager) {
            m_displayManager->displayData(data);
        }
        return;
    }
    // 设计 §7.4：移除额外的 toQImage() 空判断，避免与 DisplayManager 内部的转换重复。
    if (!image.isValid()) {
        m_displayingModuleId.clear();
        m_displayingImage2D = false;
        return;
    }
    m_displayingModuleId = moduleId;
    m_displayingImage2D = true;
    DisplayData data(image);
    if (m_displayManager) {
        m_displayManager->displayData(data);
    }
}

void MainWindow::onNewSolution() {
    if (!guiUserHasPermission(QStringLiteral("NewSolution"))) {
        showGuiPermissionDenied(this, tr("新建方案"));
        return;
    }
    if (rejectBusyFlowEdit(tr("切换工程")))
        return;
    qDebug() << "onNewSolution called";
    Logger::instance().info(tr("新建方案"), "System");
    TerminalBridge::instance().onGuiAction("create-project", "new_solution");
    // 阶段1 §3 + 复审2 R2：未保存工程切换保护（保存后继续含另存为兑现/放弃后继续/取消）
    ProjectManager::UnsavedPolicy policy;
    if (!prepareUnsavedSwitch(this, tr("新建方案"), &policy))
        return; // 取消另存为或保存失败：中止,当前工程保留
    QString errorCode, errorMessage;
    Project* proj = ProjectManager::instance().newProjectChecked(policy, &errorCode, &errorMessage);
    if (proj) {
        TerminalBridge::instance().onGuiAction("create-project-done", proj->name());
        QMessageBox::information(this, tr("新建方案"), tr("方案「%1」已创建").arg(proj->name()));
    } else if (errorCode == QLatin1String("SAVE_FAILED")) {
        // 保存失败不继续，原工程保留
        QMessageBox::critical(this, tr("新建方案"), errorMessage);
        Logger::instance().error(errorMessage, "System");
    }
    // PROJECT_UNSAVED = 用户取消，静默保留当前工程
}

void MainWindow::onSolutionList() {
    if (!guiUserHasPermission(QStringLiteral("SolutionList")) || !guiUserHasPermission(QStringLiteral("Open"))) {
        showGuiPermissionDenied(this, tr("方案列表"));
        return;
    }
    if (rejectBusyFlowEdit(tr("切换工程")))
        return;
    QStringList recent = ProjectManager::instance().recentProjects();
    if (recent.isEmpty()) {
        QMessageBox::information(this, tr("方案列表"), tr("没有最近的工程"));
        return;
    }

    QStringList items;
    for (const QString& path : recent) {
        items.append(QFileInfo(path).fileName() + " - " + path);
    }

    bool ok = false;
    const QString selected = QInputDialog::getItem(this, tr("方案列表"), tr("选择工程:"), items, 0, false, &ok);
    if (!ok || selected.isEmpty()) {
        return;
    }

    const int selectedIndex = items.indexOf(selected);
    if (selectedIndex < 0 || selectedIndex >= recent.size()) {
        return;
    }

    const QString path = recent.at(selectedIndex);
    TerminalBridge::instance().onGuiAction("open-project", path);
    // 阶段1 §3 + 复审2 R2：未保存工程切换保护（保存后继续含另存为兑现）
    ProjectManager::UnsavedPolicy policy;
    if (!prepareUnsavedSwitch(this, tr("方案列表"), &policy))
        return; // 取消另存为或保存失败：中止,当前工程保留
    QString errorCode, errorMessage;
    Project* project = ProjectManager::instance().openProjectChecked(path, policy, &errorCode, &errorMessage);
    if (!project) {
        if (errorCode == QLatin1String("SAVE_FAILED"))
            QMessageBox::critical(this, tr("方案列表"), errorMessage);
        else if (errorCode != QLatin1String("PROJECT_UNSAVED")) // 用户取消则静默
            QMessageBox::warning(this, tr("方案列表"), tr("无法打开工程：%1").arg(path));
        Logger::instance().error(tr("无法打开最近工程：%1（%2）").arg(path, errorMessage), "System");
        return;
    }

    TerminalBridge::instance().onGuiAction("open-project-done", path);
    Logger::instance().info(tr("打开最近工程：%1").arg(path), "System");
}

void MainWindow::onOpenProject() {
    if (!guiUserHasPermission(QStringLiteral("Open"))) {
        showGuiPermissionDenied(this, tr("打开工程"));
        return;
    }
    if (rejectBusyFlowEdit(tr("切换工程")))
        return;
    QString filePath = QFileDialog::getOpenFileName(this, tr("打开工程"), QString(), tr("工程文件 (*.dproj)"));
    if (!filePath.isEmpty()) {
        TerminalBridge::instance().onGuiAction("open-project", filePath);
        // 阶段1 §3 + 复审2 R2：未保存工程切换保护;打开失败时原工程保留（先验证后替换）
        ProjectManager::UnsavedPolicy policy;
        if (!prepareUnsavedSwitch(this, tr("打开工程"), &policy))
            return; // 取消另存为或保存失败：中止,当前工程保留
        QString errorCode, errorMessage;
        if (!ProjectManager::instance().openProjectChecked(filePath, policy, &errorCode, &errorMessage) &&
            errorCode == QLatin1String("SAVE_FAILED")) {
            QMessageBox::critical(this, tr("打开工程"), errorMessage);
        }
    }
}

void MainWindow::onSaveProject() {
    if (!guiUserHasPermission(QStringLiteral("Save"))) {
        showGuiPermissionDenied(this, tr("保存工程"));
        return;
    }
    if (!ProjectManager::instance().hasProject()) {
        QMessageBox::warning(this, tr("保存工程"), tr("没有打开的工程"));
        return;
    }

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) {
        return;
    }

    QString filePath = proj->filePath();
    if (filePath.isEmpty()) {
        filePath = QFileDialog::getSaveFileName(this, tr("保存工程"), QString(), tr("工程文件 (*.dproj)"));
        if (filePath.isEmpty()) {
            return;
        }
        if (!guiUserHasPermission(QStringLiteral("Save"))) {
            showGuiPermissionDenied(this, tr("保存工程"));
            return;
        }
        TerminalBridge::instance().onGuiAction("save-project", filePath);
        ProjectManager::instance().saveAsProject(filePath);
    } else {
        TerminalBridge::instance().onGuiAction("save-project", filePath);
        ProjectManager::instance().saveProject();
    }
    updateProjectContext(proj);
}

void MainWindow::onQuickMode() {
    if (!guiUserHasPermission(QStringLiteral("QuickMode"))) {
        showGuiPermissionDenied(this, tr("快捷模式"));
        return;
    }
    // 快速模式：直接运行当前流程
    executeFlowOnce();
}

void MainWindow::onQuickMeasure() {
    if (m_agentQuickMeasurement.status == QLatin1String("awaiting_user_input")) {
        QString reason;
        const QString terminalStatus =
            agentQuickMeasurementTargetStillValid(&reason) ? QStringLiteral("cancelled") : QStringLiteral("expired");
        cancelAgentQuickMeasurement(
            terminalStatus, terminalStatus == QLatin1String("cancelled") ? QStringLiteral("CANCELLED_BY_GUI") : reason);
        if (m_quickMeasureAction)
            m_quickMeasureAction->setChecked(false);
        return;
    }
    if (m_quickMeasureActive) {
        m_quickMeasureActive = false;
        for (const QMetaObject::Connection& connection : m_guiQuickMeasureRoiConnections)
            QObject::disconnect(connection);
        m_guiQuickMeasureRoiConnections.clear();
        m_quickMeasurePoints.clear();
        for (ViewportWidget* vp : m_displayManager->allViewports()) {
            if (vp) {
                vp->unsetCursor();
                vp->setPickMode(false);
                // 退出矩形 ROI 模式
                if (HImageWidget* iw = vp->imageWidget()) {
                    iw->setRoiMode(RoiType::None);
                    iw->clearRois();
                }
            }
        }
        clearMeasurementOverlays();
        if (m_quickMeasureAction)
            m_quickMeasureAction->setChecked(false);
        return;
    }

    // 弹出测量类型选择菜单
    QMenu menu(this);
    menu.addAction(tr("两点距离"))->setData(0);
    menu.addAction(tr("点到线距离"))->setData(1);
    menu.addAction(tr("两线段间距"))->setData(2);
    menu.addAction(tr("三点角度"))->setData(3);
    menu.addAction(tr("三点拟合圆"))->setData(4);
    menu.addAction(tr("矩形面积"))->setData(5);

    QAction* sel = menu.exec(QCursor::pos());
    if (!sel) {
        if (m_quickMeasureAction)
            m_quickMeasureAction->setChecked(false);
        return;
    }

    int type = sel->data().toInt();
    m_quickMeasureActive = true;
    m_quickMeasureType = type;
    m_quickMeasurePoints.clear();
    m_quickMeasurePoints3D.clear();
    for (const QMetaObject::Connection& connection : m_guiQuickMeasureRoiConnections)
        QObject::disconnect(connection);
    m_guiQuickMeasureRoiConnections.clear();
    if (m_quickMeasureAction)
        m_quickMeasureAction->setChecked(true);

    for (ViewportWidget* vp : m_displayManager->allViewports()) {
        if (!vp)
            continue;
        vp->setPickMode(true);
        if (type == 5) {
            // 矩形面积：进入 ROI 拖拽模式
            vp->setCursor(Qt::CrossCursor);
            if (HImageWidget* iw = vp->imageWidget()) {
                iw->clearRois();
                iw->setRoiMode(RoiType::Rectangle1);
                // 连接 roiCreated 信号（UniqueConnection 防重复）
                m_guiQuickMeasureRoiConnections.append(
                    connect(iw, &HImageWidget::roiCreated, this, [this](const RoiData& roi) {
                        if (!m_quickMeasureActive || m_quickMeasureType != 5)
                            return;
                        QPointF a(roi.x1, roi.y1);
                        QPointF b(roi.x2, roi.y2);
                        double w = std::abs(b.x() - a.x());
                        double h = std::abs(b.y() - a.y());
                        double area = w * h;
                        QPointF tl(qMin(a.x(), b.x()), qMin(a.y(), b.y()));
                        QPointF br(qMax(a.x(), b.x()), qMax(a.y(), b.y()));
                        QPointF topRight(br.x(), tl.y());
                        QPointF botLeft(tl.x(), br.y());
                        QString label = tr("W=%1, H=%2, A=%3").arg(w, 0, 'f', 2).arg(h, 0, 'f', 2).arg(area, 0, 'f', 2);
                        Logger::instance().info(tr("快速测量: 矩形 宽=%1, 高=%2, 面积=%3")
                                                    .arg(w, 0, 'f', 3)
                                                    .arg(h, 0, 'f', 3)
                                                    .arg(area, 0, 'f', 3),
                                                "Measurement");
                        // 清除 ROI 绘制，改用测量 overlay 显示
                        if (HImageWidget* iw2 = qobject_cast<HImageWidget*>(sender())) {
                            iw2->clearRois();
                        }
                        for (ViewportWidget* vp2 : m_displayManager->allViewports()) {
                            if (HImageWidget* iw3 = vp2 ? vp2->imageWidget() : nullptr) {
                                if (iw3->hasImage()) {
                                    iw3->setMeasurementOverlay({{tl, QStringLiteral("TL")}, {br, QStringLiteral("BR")}},
                                                               {{tl, topRight, QString()},
                                                                {topRight, br, QString()},
                                                                {br, botLeft, QString()},
                                                                {botLeft, tl, label}});
                                }
                            }
                        }
                    }));
            }
        } else {
            // 其他类型：点击拾取模式
            vp->setCursor(Qt::CrossCursor);
        }
    }

    QStringList labels = {tr("两点距离"), tr("点到线距离"), tr("两线段间距"),
                          tr("三点角度"), tr("三点拟合圆"), tr("矩形面积")};
    Logger::instance().info(tr("快速测量：%1").arg(labels[type]), "Measurement");
}

void MainWindow::onQuickAnnotate() {
    // Fix P1-9: 选择最近有焦点的视口，回退到第一个有图像的视口
    ViewportWidget* targetVp = nullptr;
    ViewportWidget* fallbackVp = nullptr;
    for (ViewportWidget* vp : m_displayManager->allViewports()) {
        if (!vp || !vp->imageWidget() || !vp->imageWidget()->hasImage())
            continue;
        if (!fallbackVp)
            fallbackVp = vp;
        // 优先选择有焦点的或之前交互过的视口
        if (vp->hasFocus() || (vp->imageWidget() && vp->imageWidget()->hasFocus())) {
            targetVp = vp;
            break;
        }
    }
    if (!targetVp)
        targetVp = fallbackVp;

    if (!targetVp || !targetVp->imageWidget()) {
        Logger::instance().warning(tr("没有可用的视口图像，请先导入或采集图像"), "Annotation");
        return;
    }

    if (!m_samAnnotatorDialog) {
        m_samAnnotatorDialog = new SamAnnotatorDialog(this);
        m_samAnnotatorDialog->setAttribute(Qt::WA_DeleteOnClose);
        SamAnnotatorDialog* createdDialog = m_samAnnotatorDialog;
        connect(createdDialog, &QDialog::finished, this, [this, createdDialog](int) {
            if (m_samAnnotatorDialog == createdDialog)
                m_samAnnotatorDialog.clear();
        });
        connect(createdDialog, &QObject::destroyed, this, [this, createdDialog]() {
            if (m_samAnnotatorDialog == createdDialog)
                m_samAnnotatorDialog.clear();
        });
    }

    m_samAnnotatorDialog->applyTheme(m_isDarkTheme);
    // Fix 1: 不传全局 m_lastImportedImagePath，始终使用当前视口图像快照
    // 对话框会自动保存快照到临时文件供后端使用，避免处理旧文件
    m_samAnnotatorDialog->attachToImageWidget(targetVp->imageWidget(), QString());
    m_samAnnotatorDialog->show();
    m_samAnnotatorDialog->raise();
    m_samAnnotatorDialog->activateWindow();
}

void MainWindow::setUiRunningState(bool running, bool cycleMode) {
    m_isRunning = running;
    m_isCycleMode = running && cycleMode;
    if (!running) {
        updateBreakpointRunControls(false);
    }
}

void MainWindow::updateBreakpointRunControls(bool paused) {
    if (QAction* action = findChild<QAction*>(QStringLiteral("RunOnceAction"))) {
        action->setText(paused ? tr("继续") : tr("单次运行"));
        action->setToolTip(paused ? tr("继续执行") : tr("单次运行"));
    }
    if (QToolButton* button = findChild<QToolButton*>(QStringLiteral("FlowRunButton"))) {
        button->setText(paused ? tr("继续") : tr("运行"));
        button->setToolTip(paused ? tr("继续执行") : tr("运行流程"));
    }
}

void MainWindow::onRunOnce() {
    RunEngine& engine = RunEngine::instance();
    FlowExecutionService& service = FlowExecutionService::instance();
    if (engine.isPausedAtBreakpoint() || service.state() == FlowExecutionService::State::Paused) {
        const bool cycleMode = m_isCycleMode || service.currentJob() == FlowExecutionService::JobType::Cycle ||
                               engine.runMode() == RunMode::RunCycle;
        const QString permission = cycleMode ? QStringLiteral("RunCycle") : QStringLiteral("RunOnce");
        if (!guiUserHasPermission(permission)) {
            showGuiPermissionDenied(this, tr("继续运行"));
            return;
        }
        updateBreakpointRunControls(false);
        if (service.isActive())
            service.continueRun();
        else
            engine.resume();
        return;
    }
    // 活跃运行时该按钮仍承担停止操作；安全停止不受启动权限门限制。
    if (m_isRunning) {
        onStop();
        return;
    }
    if (!guiUserHasPermission(QStringLiteral("RunOnce"))) {
        showGuiPermissionDenied(this, tr("单次运行"));
        return;
    }
    if (m_processTree->topLevelItemCount() == 0) {
        if (m_processTimeLabel) {
            m_processTimeLabel->setText(tr("总耗时：0 ms"));
        }
        Logger::instance().warning(tr("流程为空，无法运行"), "System");
        return;
    }
    ++m_measurementPickContinuationGeneration;
    setUiRunningState(true, false);
    Logger::instance().info(tr("运行一次"), "System");
    if (requestMeasurementInputForRun()) {
        return;
    }
    executeFlowOnce();
}

void MainWindow::onRunCycle() {
    RunEngine& engine = RunEngine::instance();
    FlowExecutionService& service = FlowExecutionService::instance();
    if (engine.isPausedAtBreakpoint() || service.state() == FlowExecutionService::State::Paused) {
        const bool cycleMode = m_isCycleMode || service.currentJob() == FlowExecutionService::JobType::Cycle ||
                               engine.runMode() == RunMode::RunCycle;
        const QString permission = cycleMode ? QStringLiteral("RunCycle") : QStringLiteral("RunOnce");
        if (!guiUserHasPermission(permission)) {
            showGuiPermissionDenied(this, tr("继续运行"));
            return;
        }
        updateBreakpointRunControls(false);
        if (service.isActive())
            service.continueRun();
        else
            engine.resume();
        return;
    }
    if (!m_isRunning && !service.isActive()) {
        if (!guiUserHasPermission(QStringLiteral("RunCycle"))) {
            showGuiPermissionDenied(this, tr("循环运行"));
            return;
        }
        if (m_processTree->topLevelItemCount() == 0) {
            if (m_processTimeLabel) {
                m_processTimeLabel->setText(tr("总耗时：0 ms"));
            }
            Logger::instance().warning(tr("流程为空，无法循环运行"), "System");
            return;
        }
        ++m_measurementPickContinuationGeneration;
        setUiRunningState(true, true);
        Logger::instance().info(tr("循环运行"), "System");
        const bool waitingForMeasurementPick = requestMeasurementInputForRun();
        if (!syncModulesToRunEngine()) {
            m_pendingMeasurementInputId.clear();
            setUiRunningState(false, false);
            Logger::instance().warning(tr("流程同步失败，无法循环运行"), "System");
            return;
        }
        QString sessionId;
        if (waitingForMeasurementPick) {
            if (!RunEngine::instance().startCycle(&sessionId, 1, false, true, true)) {
                m_pendingMeasurementInputId.clear();
                setUiRunningState(false, false);
                return;
            }
            m_pendingMeasurementCycleSessionId = sessionId;
            return;
        }
        if (!startRunThroughService(true)) {
            setUiRunningState(false, false);
            return;
        }
    }
}

void MainWindow::onStepRun() {
    FlowExecutionService& service = FlowExecutionService::instance();
    // 后台执行（设计 §8.1）：单步异步后由 Service 拒绝非 Idle 的重入；
    // 快速连点只保持按钮状态，不排队第二次单步。
    if (m_isRunning || service.isActive()) {
        return;
    }
    if (!guiUserHasPermission(QStringLiteral("RunOnce"))) {
        showGuiPermissionDenied(this, tr("单步执行"));
        return;
    }
    if (m_processTree->topLevelItemCount() == 0) {
        if (m_processTimeLabel) {
            m_processTimeLabel->setText(tr("总耗时：0 ms"));
        }
        Logger::instance().warning(tr("流程为空，无法单步执行"), "System");
        return;
    }
    if (!syncModulesToRunEngine()) {
        Logger::instance().warning(tr("流程同步失败，无法单步执行"), "System");
        return;
    }

    ++m_measurementPickContinuationGeneration;
    setUiRunningState(true, false);
    m_isStepMode = true;
    Logger::instance().info(tr("单步执行"), "System");
    if (!service.stepOnce()) {
        setUiRunningState(false, false);
        m_isStepMode = false;
        Logger::instance().warning(tr("单步请求被拒绝：后台已有 job"), "Run");
    }
}

void MainWindow::onStop() {
    ++m_measurementPickContinuationGeneration;
    if (m_displayManager) {
        for (ViewportWidget* viewport : m_displayManager->allViewports()) {
            if (viewport && viewport->imageWidget())
                viewport->imageWidget()->cancelPendingPointerInput();
        }
    }
    clearPendingMeasurementPick();
    setUiRunningState(false, false);
    m_isStepMode = false;
    // 后台执行（设计 §5.2/§8.1）：有 Service job 时走 job 级取消；否则回退引擎停止。
    FlowExecutionService& service = FlowExecutionService::instance();
    if (service.isActive()) {
        service.stop();
    } else {
        RunEngine::instance().stop();
    }
    Logger::instance().info(tr("停止"), "System");
}

bool MainWindow::startRunThroughService(bool cycleMode) {
    if (!syncModulesToRunEngine()) {
        Logger::instance().warning(tr("流程同步失败，无法运行"), "Run");
        return false;
    }
    // 后台执行（设计 §7.1/§7.5）：UI 是显示消费者，窗口可见时开启帧捕获（最小化/不可见关闭）。
    updateDisplayCaptureForVisibility();
    if (m_imageCoalescer) {
        m_imageCoalescer->clearPending();
    }
    FlowExecutionService& service = FlowExecutionService::instance();
    const bool started = cycleMode ? service.startCycle() : service.startSingle();
    if (!started) {
        Logger::instance().warning(tr("启动请求被拒绝：后台已有 job 或引擎忙"), "Run");
        return false;
    }
    return true;
}

void MainWindow::finishRunUi() {
    // 设计 §7.3：job 停止后不再接受迟到帧；待渲染帧在 jobFinished 前已 flush，这里清空残留。
    if (m_imageCoalescer) {
        m_imageCoalescer->clearPending();
        m_imageCoalescer->clearExpectedIdentity();
    }
    setUiRunningState(false, false);
    m_isStepMode = false;
    updateBreakpointRunControls(false);
    if (m_closeWhenRunFinishes) {
        // jobFinished 到达时 Service 已提交 Idle：此时重试关闭不会再次被 ignore。
        m_closeWhenRunFinishes = false;
        m_closeAwaitingServiceJob = false;
        close();
    }
}

void MainWindow::renderDisplayFrame(const DisplayFrame& frame) {
    // 设计 §7.2/§7.3：合并器是唯一的大图刷新入口；叠加只读帧内快照，禁止回读实时 moduleOutput。
    if (frame.runId != RunEngine::instance().lastRunId())
        return;
    m_latestDisplayFrame = frame;
    m_hasDisplayFrame = true;
    if (frame.hasImage() && m_displayManager) {
        m_displayingModuleId = frame.moduleId;
        m_displayingImage2D = true;
        // 后台/周期刷新保留用户缩放与 ROI（设计 §7.4）。
        ImageData image(frame.image);
        image.setCoordinateProvenance(frame.coordinateProvenance);
        DisplayData display(image);
        display.setViewportId(frame.viewportId);
        m_displayManager->displayData(display, 0, /*preserveView=*/true);
    }
    if (frame.hasSnapshot()) {
        updateMeasurementResultOnOverlay();
    } else {
        // 中间帧无同轮快照：清除叠加，避免 A 帧图像搭配 B 轮结果（设计 §7.3）。
        clearMeasurementOverlays();
    }
}

void MainWindow::updateDisplayCaptureForVisibility() {
    // 设计 §7.5：窗口不可见/最小化或没有显示消费者时关闭帧捕获，工作线程不做快照与合并调度。
    RunEngine::instance().setDisplayCaptureEnabled(isVisible() && !isMinimized());
}

void MainWindow::executeFlowOnce() {
    if (FlowExecutionService::instance().isActive())
        return;
    resetFlowRunPresentation();

    // 如果没有模块，直接返回
    if (m_processTreeController->topLevelItemCount() == 0) {
        if (m_processTimeLabel) {
            m_processTimeLabel->setText(tr("总耗时：0 ms"));
        }
        return;
    }

    // 通过后台服务执行，统一在启动入口同步工程。
    if (!startRunThroughService(false))
        setUiRunningState(false, false);
}

void MainWindow::resetFlowRunPresentation() {
    // 清空之前的执行时间和所有高亮
    m_moduleExecutionTimes.clear();
    m_flowTotalTime = 0;
    m_flowInput = ImageData();
    m_currentExecutingIndex = 0;

    // 清除选中状态和所有项目的高亮
    m_processTreeController->clearSelection();
    m_processTreeController->setCurrentItem(nullptr);
    for (int i = 0; i < m_processTreeController->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_processTreeController->topLevelItem(i);
        const QString instanceId = item->data(0, Qt::UserRole + 1).toString();
        if (m_dirtyModuleIds.contains(instanceId)) {
            setProcessItemStatus(item, QStringLiteral("dirty"), tr("待运行"));
        } else {
            clearExecutionHighlight(item);
            item->setText(1, QString());
        }
    }
    m_currentExecutingItem = nullptr;
}

void MainWindow::onUserLogin() {
    // 使用已实现的 LoginDialog
    LoginDialog dialog(this);
    dialog.setWindowTitle(tr("用户登录"));
    dialog.exec();
    Logger::instance().info(tr("用户登录"), "System");
}

void MainWindow::applyAgentSettings(const AgentSettingsDialog& dialog) {
    AgentController& ctrl = AgentController::instance();
    ctrl.setPermissionLevel(dialog.permissionLevel());
    OpenAILLMClient* client = qobject_cast<OpenAILLMClient*>(ctrl.llmClient());
    if (!client) {
        client = new OpenAILLMClient(&ctrl);
        ctrl.setLLMClient(client);
    }
    client->setEndpoint(dialog.endpoint());
    client->setApiKey(dialog.apiKey());
    client->setModel(dialog.model());
    client->setTemperature(dialog.temperature());
    client->setMaxTokens(dialog.maxTokens());
    client->setToolsEnabled(dialog.toolsEnabled());
    client->setVisionEnabled(dialog.visionEnabled());
    client->setThinkingEnabled(dialog.thinkingEnabled());
    client->setStreamEnabled(dialog.streamEnabled());
    client->setReasoningEffort(dialog.reasoningEffort());
    client->setImageDetail(dialog.imageDetail());
    ctrl.setSystemPrompt(dialog.systemPrompt());
}

void MainWindow::onGlobalVar() {
    const UserManager& users = UserManager::instance();
    if (!users.isLoggedIn() || users.currentUser().type == UserType::Guest ||
        !users.hasPermission(QStringLiteral("GlobalVar"))) {
        QMessageBox::warning(this, tr("权限不足"), tr("您没有权限查看或管理全局变量。"));
        return;
    }
    // 使用已实现的 GlobalVarView
    GlobalVarView dialog(this);
    dialog.setWindowTitle(tr("全局变量"));
    dialog.exec();
    Logger::instance().info(tr("全局变量"), "System");
}

void MainWindow::onCameraSettings() {
    const UserManager& users = UserManager::instance();
    if (!users.isLoggedIn() || users.currentUser().type == UserType::Guest ||
        !users.hasPermission(QStringLiteral("Camera")) || !users.hasPermission(QStringLiteral("CameraSet"))) {
        QMessageBox::warning(this, tr("权限不足"), tr("您没有权限管理相机设置。"));
        return;
    }
    // 使用已实现的 CameraSetView
    CameraSetView dialog(this);
    dialog.setWindowTitle(tr("相机设置"));
    dialog.exec();
    Logger::instance().info(tr("相机设置"), "System");
}

void MainWindow::onCommSettings() {
    const UserManager& users = UserManager::instance();
    if (!users.isLoggedIn() || users.currentUser().type == UserType::Guest ||
        !users.hasPermission(QStringLiteral("CommunicationSet"))) {
        QMessageBox::warning(this, tr("权限不足"), tr("当前用户没有通信设置权限。"));
        Logger::instance().warning(QStringLiteral("Communication settings denied: CommunicationSet required"),
                                   QStringLiteral("Auth"));
        return;
    }

    // 使用已实现的 CommunicationSetView
    CommunicationSetView dialog(this);
    dialog.setWindowTitle(tr("通信设置"));
    dialog.exec();
    Logger::instance().info(tr("通信设置"), "System");
}

void MainWindow::onHardwareConfig() {
    const UserManager& users = UserManager::instance();
    if (!users.isLoggedIn() || users.currentUser().type == UserType::Guest ||
        !users.hasPermission(QStringLiteral("SystemParam"))) {
        QMessageBox::warning(this, tr("权限不足"), tr("当前用户没有系统参数权限。"));
        Logger::instance().warning(QStringLiteral("System settings denied: SystemParam required"),
                                   QStringLiteral("Auth"));
        return;
    }

    SystemParamView dialog(this, QStringLiteral("SystemParam"));
    dialog.setWindowTitle(tr("硬件配置"));
    const bool wasDark = m_isDarkTheme;
    dialog.exec();
    // 问题 1: 保存后立即读取并刷新主题
    ConfigManager& cfg = ConfigManager::instance();
    if (cfg.isInitialized()) {
        m_isDarkTheme = cfg.groupBool("appearance", "darkTheme", false);
        if (m_isDarkTheme != wasDark) {
            applyTheme();
        }
    }
    Logger::instance().info(tr("硬件配置"), "System");
}

void MainWindow::onHome() {
    clearCentralDisplay();
    if (m_processTabWidget && m_flowCanvas) {
        m_processTabWidget->setCurrentWidget(m_flowCanvas);
    }
    if (m_processTree) {
        m_processTree->clearSelection();
    }
    if (m_processTimeLabel) {
        m_processTimeLabel->setText(tr("总耗时：0 ms"));
    }
    Logger::instance().info(tr("主页"), "System");
}

void MainWindow::onToggleTheme() {
    m_isDarkTheme = !m_isDarkTheme;
    applyTheme();
    // P1-3: 持久化主题选择
    ConfigManager& cfg = ConfigManager::instance();
    if (cfg.isInitialized()) {
        cfg.setGroupValue("appearance", "darkTheme", m_isDarkTheme);
        cfg.save();
    }
    Logger::instance().info(m_isDarkTheme ? tr("切换到深色主题") : tr("切换到浅色主题"), "System");
}

void MainWindow::onDeviceSettings() {
    const UserManager& users = UserManager::instance();
    if (!users.isLoggedIn() || users.currentUser().type == UserType::Guest ||
        !users.hasPermission(QStringLiteral("SystemParam"))) {
        QMessageBox::warning(this, tr("权限不足"), tr("当前用户没有系统参数权限。"));
        Logger::instance().warning(QStringLiteral("System settings denied: SystemParam required"),
                                   QStringLiteral("Auth"));
        return;
    }

    // 使用已实现的 SystemParamView
    SystemParamView dialog(this, QStringLiteral("SystemParam"));
    dialog.setWindowTitle(tr("设备设置"));
    dialog.exec();
    Logger::instance().info(tr("设备设置"), "System");
}

void MainWindow::onSystemSettings() {
    // 使用已实现的 SystemParamView
    const UserManager& users = UserManager::instance();
    if (!users.isLoggedIn() || users.currentUser().type == UserType::Guest ||
        !users.hasPermission(QStringLiteral("SystemParam"))) {
        QMessageBox::warning(this, tr("权限不足"), tr("当前用户没有系统参数权限。"));
        Logger::instance().warning(QStringLiteral("System settings denied: SystemParam required"),
                                   QStringLiteral("Auth"));
        return;
    }

    SystemParamView dialog(this, QStringLiteral("SystemParam"));
    dialog.setWindowTitle(tr("系统设置"));
    dialog.exec();
    Logger::instance().info(tr("系统设置"), "System");
}

void MainWindow::onScreenshot() {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen) {
        QPixmap screenshot = screen->grabWindow(winId());
        QString fileName = QFileDialog::getSaveFileName(
            this, tr("保存截图"), QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png",
            tr("PNG 文件 (*.png)"));
        if (!fileName.isEmpty()) {
            screenshot.save(fileName);
            Logger::instance().info(tr("已保存截图：%1").arg(fileName), "System");
        }
    }
}

void MainWindow::onTest3DRender() {
    // Create a synthetic point cloud (sphere + cube)
    PointCloudData pc;
    const int N = 50;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            double theta = i * 2.0 * M_PI / N;
            double phi = j * M_PI / N;
            double r = 2.0;
            pc.points.push_back(Eigen::Vector3d(r * sin(phi) * cos(theta), r * sin(phi) * sin(theta), r * cos(phi)));
            pc.colors.push_back(Eigen::Vector3d(sin(theta), cos(phi), 0.5));
            pc.normals.push_back(Eigen::Vector3d(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi)));
        }
    }

    Logger::instance().info(tr("Test 3D: %1 points").arg(pc.size()), "Debug");
    qDebug() << "[Test3D] Creating DisplayData with" << pc.size() << "points";

    DisplayData data;
    data.variant() = std::move(pc);
    data.setTimestamp(QDateTime::currentMSecsSinceEpoch());

    qDebug() << "[Test3D] pointCloudData ptr:" << data.pointCloudData() << "isValid:" << data.isValid();

    if (m_displayManager) {
        qDebug() << "[Test3D] Calling displayManager->displayData, viewports=" << m_displayManager->viewportCount();
        m_displayManager->displayData(data);
        qDebug() << "[Test3D] displayData returned";
    } else {
        qDebug() << "[Test3D] m_displayManager is NULL!";
    }
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("关于 DeepLux"),
                       tr("<h2>DeepLux Vision 1.0.0</h2>"
                          "<p>工业视觉检测软件</p>"
                          "<p>&copy; 2024 DeepLux. All rights reserved.</p>"));
}

void MainWindow::onLogAdded(const LogEntry& entry) {
    if (!m_logTable)
        return;

    // 根据过滤器检查是否应该显示
    int filterLevel = m_logFilterLevel;
    if (filterLevel > 0) {
        // filterLevel: 1=Debug, 2=Info, 3=Warning, 4=Error
        // 只显示 >= filterLevel 的日志
        int entryLevel = 0;
        switch (entry.level) {
        case LogLevel::Debug:
            entryLevel = 1;
            break;
        case LogLevel::Info:
            entryLevel = 2;
            break;
        case LogLevel::Warning:
            entryLevel = 3;
            break;
        case LogLevel::Error:
            entryLevel = 4;
            break;
        case LogLevel::Success:
            entryLevel = 2;
            break; // Success 按 Info 处理
        }
        if (entryLevel < filterLevel) {
            return; // 不显示
        }
    }

    int row = m_logTable->rowCount();
    m_logTable->insertRow(row);

    // 时间
    QTableWidgetItem* timeItem = new QTableWidgetItem(entry.timestamp.toString("hh:mm:ss.zzz"));
    timeItem->setForeground(QColor("#888888"));
    m_logTable->setItem(row, 0, timeItem);

    // 级别
    QString levelStr = Logger::levelToString(entry.level);
    QTableWidgetItem* levelItem = new QTableWidgetItem(levelStr);
    levelItem->setForeground(QColor(entry.level == LogLevel::Error     ? "#e94560"
                                    : entry.level == LogLevel::Warning ? "#f39c12"
                                    : entry.level == LogLevel::Success ? "#27ae60"
                                                                       : "#3498db"));
    m_logTable->setItem(row, 1, levelItem);

    // 消息
    QString fullMessage =
        entry.category.isEmpty() ? entry.message : QString("[%1] %2").arg(entry.category, entry.message);
    QTableWidgetItem* msgItem = new QTableWidgetItem(fullMessage);
    m_logTable->setItem(row, 2, msgItem);

    // 自动滚动到最后一行
    m_logTable->scrollToBottom();
}

void MainWindow::onLogFilterChanged(int index) {
    if (!m_logTable)
        return;

    m_logFilterLevel = index;

    // 表头文字保持不变（避免固定宽度下文字被遮挡）
    Q_UNUSED(index)

    // 遍历所有行，根据过滤器显示/隐藏
    for (int row = 0; row < m_logTable->rowCount(); ++row) {
        QTableWidgetItem* levelItem = m_logTable->item(row, 1);
        if (!levelItem)
            continue;

        QString levelStr = levelItem->text();
        int entryLevel = 0;
        if (levelStr == "DEBUG")
            entryLevel = 1;
        else if (levelStr == "INFO")
            entryLevel = 2;
        else if (levelStr == "WARN")
            entryLevel = 3;
        else if (levelStr == "ERROR")
            entryLevel = 4;
        else if (levelStr == "SUCCESS")
            entryLevel = 2;

        // filterLevel: 0=全部, 1=Debug, 2=Info, 3=Warning, 4=Error
        if (index == 0 || entryLevel >= index) {
            m_logTable->showRow(row);
        } else {
            m_logTable->hideRow(row);
        }
    }
}

void MainWindow::showLogLevelMenu() {
    QMenu menu(this);
    QStringList items = QStringList() << tr("全部") << tr("Debug") << tr("Info") << tr("Warning") << tr("Error");
    for (int i = 0; i < items.size(); ++i) {
        QAction* action = menu.addAction(items[i]);
        action->setCheckable(true);
        action->setChecked(m_logFilterLevel == i);
        connect(action, &QAction::triggered, this, [this, i]() { onLogFilterChanged(i); });
    }

    // 在表头"级别"列下方弹出
    QHeaderView* header = m_logTable->horizontalHeader();
    QRect sectionRect = header->sectionViewportPosition(1) >= 0
                            ? QRect(header->sectionViewportPosition(1), header->height(), header->sectionSize(1), 0)
                            : QRect();
    QPoint pos = m_logTable->mapToGlobal(QPoint(sectionRect.x(), sectionRect.y()));
    menu.exec(pos);
}

void MainWindow::onImportImage() {
    QString filePath = QFileDialog::getOpenFileName(this, tr("导入图像"), QString(),
                                                    tr("图像文件 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
    if (!filePath.isEmpty()) {
        if (importFile(filePath)) {
            Logger::instance().info(tr("导入文件：%1").arg(QFileInfo(filePath).fileName()), "System");
        }
    }
}

bool MainWindow::syncModulesToRunEngine() {
    if (!m_processTreeController) {
        return false;
    }

    if (!m_modulesNeedSync) {
        return true;
    }

    RunEngine& engine = RunEngine::instance();
    Project* project = ProjectManager::instance().currentProject();
    if (!project || engine.isBusy()) {
        return false;
    }

    QList<ModuleBase*> modules;
    QMap<QString, QTreeWidgetItem*> instanceItems;

    for (int i = 0; i < m_processTreeController->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_processTreeController->topLevelItem(i);
        const QString instanceName = item->data(0, Qt::UserRole + 1).toString();
        if (instanceName.isEmpty() || !m_flowModules.contains(instanceName)) {
            continue;
        }

        IModule* module = m_flowModules.value(instanceName);
        if (!module || !module->isInitialized()) {
            continue;
        }

        ModuleBase* moduleBase = qobject_cast<ModuleBase*>(module);
        if (!moduleBase) {
            continue;
        }

        moduleBase->setInstanceName(instanceName);
        modules.append(moduleBase);
        instanceItems.insert(instanceName, item);
    }

    QString orderError;
    if (!engine.synchronizeProjectModules(project, modules, orderError)) {
        if (engine.isBusy()) {
            return false;
        }
        Logger::instance().error(orderError, "Run");
        m_processTreeController->clearInstanceItemMap();
        return false;
    }

    m_processTreeController->clearInstanceItemMap();
    for (auto it = instanceItems.cbegin(); it != instanceItems.cend(); ++it)
        m_processTreeController->setInstanceItem(it.key(), it.value());

    m_modulesNeedSync = false;
    return true;
}

// ========== 图标创建辅助方法 ==========

QIcon MainWindow::createIcon(const QString& name) {
    if (name == QStringLiteral("delete")) {
        return AppIconProvider::icon(AppIconProvider::Icon::Delete, 24, QColor("#DC2626"));
    }
    return AppIconProvider::icon(AppIconProvider::Icon::Settings, 24, QColor("#374151"));
}

void MainWindow::loadAgentSettings() {
    ConfigManager& cfg = ConfigManager::instance();
    // 视口图像提供者（设计 §4.5）：优先主数据源视口当前图，回退上次流程输出。
    // 回调在 capture_viewport_image 调用时才求值；即使配置未初始化也必须注入。
    if (AgentActor* actor = AgentController::instance().actor()) {
        // QPointer 守卫：AgentController 是进程级单例，MainWindow 可能先于它析构
        QPointer<MainWindow> guard(this);
        actor->setViewportImageProvider([guard](QString* error) -> QImage {
            if (guard) {
                if (ViewportWidget* viewport = guard->primaryDataSourceViewport()) {
                    const QImage image = viewport->currentImage();
                    if (!image.isNull())
                        return image;
                }
            }
            const ImageData lastOutput = RunEngine::instance().lastOutput();
            if (lastOutput.isValid())
                return lastOutput.toQImage();
            if (error)
                *error = QStringLiteral("No image available in the viewport");
            return {};
        });
    }
    if (!cfg.isInitialized())
        return;

    AgentController& ctrl = AgentController::instance();

    // Load permission level
    int permLevel = cfg.groupInt("agent", "permissionLevel", 1);
    ctrl.setPermissionLevel(static_cast<AgentController::PermissionLevel>(permLevel));
    ctrl.setSystemPrompt(cfg.groupString("agent", "systemPrompt", ctrl.systemPrompt()));

    // Load and configure LLM client if API key is present
    QString apiKey = cfg.groupString("agent", "apiKey", "");
    if (!apiKey.isEmpty()) {
        OpenAILLMClient* client = new OpenAILLMClient(&ctrl);
        client->setEndpoint(cfg.groupString("agent", "endpoint", "https://api.openai.com/v1/chat/completions"));
        client->setApiKey(apiKey);
        const QString configuredModel = cfg.groupString("agent", "model", "gpt-4o");
        client->setModel(configuredModel);
        client->setTemperature(cfg.groupDouble("agent", "temperature", 0.3));
        client->setMaxTokens(cfg.groupInt("agent", "maxTokens", 4096));
        client->setToolsEnabled(cfg.groupBool("agent", "toolsEnabled", true));
        // L3：visionEnabled 缺省时用模型名启发式（不写回配置，避免把猜测变成用户设置）
        if (cfg.containsGroupKey("agent", "visionEnabled"))
            client->setVisionEnabled(cfg.groupBool("agent", "visionEnabled", true));
        else
            client->setVisionEnabled(OpenAILLMClient::heuristicSupportsVision(configuredModel));
        const bool deepSeekFlash = OpenAILLMClient::isDeepSeekFlashModel(configuredModel);
        client->setReasoningEffort(
            cfg.groupString("agent", "reasoningEffort", deepSeekFlash ? QStringLiteral("high") : QString()));
        client->setThinkingEnabled(cfg.groupBool("agent", "thinkingEnabled", deepSeekFlash));
        client->setImageDetail(cfg.groupString("agent", "imageDetail", QStringLiteral("original")));
        // 第二迭代：流式开关与 reasoning 回传策略（设计 §4.1/§3.7）
        client->setStreamEnabled(cfg.groupBool("agent", "streamEnabled", true));
        // R3：三个流式配置键注入（client setter 内部按 §3.6 clamp）
        client->setStreamIncludeUsage(cfg.groupBool("agent", "streamIncludeUsage", false));
        client->setStreamIdleTimeoutMs(cfg.groupInt("agent", "streamIdleTimeoutMs", 60000));
        client->setStreamMaxDurationMs(cfg.groupInt("agent", "streamMaxDurationMs", 600000));
        const QString reasoningEcho = cfg.groupString("agent", "reasoningEcho", "none").trimmed().toLower();
        if (reasoningEcho == QStringLiteral("full"))
            client->setReasoningEcho(ReasoningEcho::Full);
        else if (reasoningEcho == QStringLiteral("current-turn"))
            client->setReasoningEcho(ReasoningEcho::CurrentTurn);
        else
            client->setReasoningEcho(ReasoningEcho::None);
        ctrl.setLLMClient(client);
    }

    updateAgentPermissionDisplay();

    // Connect permission change to update display
    connect(&ctrl, &AgentController::permissionLevelChanged, this, &MainWindow::updateAgentPermissionDisplay);
}

void MainWindow::updateAgentPermissionDisplay() {
    AgentController::PermissionLevel level = AgentController::instance().permissionLevel();
    QString text;
    QString color;
    switch (level) {
    case AgentController::PermissionLevel::Observer:
        text = "Agent: Observer";
        color = "#3498db";
        break;
    case AgentController::PermissionLevel::Advisor:
        text = "Agent: Advisor";
        color = "#f39c12";
        break;
    case AgentController::PermissionLevel::Autopilot:
        text = "Agent: Autopilot";
        color = "#e74c3c";
        break;
    }

    qDebug() << "Agent permission display:" << text;
}

} // namespace DeepLux
