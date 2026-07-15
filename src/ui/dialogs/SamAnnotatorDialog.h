#pragma once

#include <QDialog>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <memory>

class QToolButton;
class QButtonGroup;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QShortcut;
class QStackedWidget;
class QLabel;
class QPushButton;
class QTemporaryFile;
class QUndoStack;
class QEvent;

namespace DeepLux {

class HImageWidget;
class AnnotationOverlayWidget;
class SamBackendClient;
struct AnnotationSession;
struct AnnotationObject;

/**
 * @brief SAM 快速标注对话框
 *
 * 独立窗口，提供基于 SAM 的交互式标注能力。
 * 布局：左侧（类别输入 + 对象列表）| 中间（HImageWidget + AnnotationOverlayWidget）| 底部（工具栏）
 *
 * 工作流程：
 * 1. 从文件打开图片或接收当前视口快照
 * 2. 选择模式（正点/负点/框选/选择）在图像上绘制 prompt
 * 3. 添加正点/负点/框选后触发 SAM 预测，按 Enter 确认当前预览
 * 4. 在对象列表中管理已确认对象，按 Delete 删除选中
 * 5. 保存（AnnotationSession::save）或导出 LabelMe
 */
class SamAnnotatorDialog : public QDialog {
    Q_OBJECT

public:
    explicit SamAnnotatorDialog(QWidget* parent = nullptr);
    ~SamAnnotatorDialog() override;

    // 从文件打开图片
    void openImageFromFile(const QString& suggestedPath = QString());

    // 接收当前图像快照（来自视口）
    void setImageSnapshot(const QImage& image, const QString& imagePath = QString());

    // 当前标注会话
    AnnotationSession session() const;

    // 模式枚举（与 AnnotationOverlayWidget::Mode 对齐）
    enum class ToolMode { Select, PositivePoint, NegativePoint, Box };

    ToolMode currentToolMode() const {
        return m_toolMode;
    }

    // 工具按钮访问（供测试）
    QToolButton* positivePointButton() const {
        return m_btnPositivePoint;
    }
    QToolButton* negativePointButton() const {
        return m_btnNegativePoint;
    }
    QToolButton* boxButton() const {
        return m_btnBox;
    }
    QToolButton* selectButton() const {
        return m_btnSelect;
    }

    // 快捷键访问（供测试验证）
    QShortcut* confirmShortcut() const {
        return m_scConfirm;
    }
    QShortcut* cancelShortcut() const {
        return m_scCancel;
    }
    QShortcut* deleteShortcut() const {
        return m_scDelete;
    }
    QShortcut* undoShortcut() const {
        return m_scUndo;
    }

    // 控件访问（供测试）
    HImageWidget* imageWidget() const {
        return m_imageWidget;
    }
    AnnotationOverlayWidget* overlayWidget() const {
        return m_overlay;
    }
    SamBackendClient* backendClient() const {
        return m_samClient;
    }
    QListWidget* objectList() const {
        return m_objectList;
    }
    QLineEdit* categoryEdit() const {
        return m_categoryEdit;
    }

signals:
    void imageLoaded(const QString& imagePath);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onOpenImage();
    void onSaveSession();
    void onExportLabelMe();
    void onConfirm();
    void onCancel();
    void onDeleteSelected();
    void onUndo();
    void onModeButtonToggled();
    void onObjectSelectionChanged();
    void onOverlayClicked(const QPointF& imagePoint, Qt::MouseButton button);
    void onOverlayDragEnded(const QPointF& imageStart, const QPointF& imageEnd);
    void onPredictionReady(const QList<QPointF>& polygon, const QRectF& bbox, double score, const QString& maskRle);

private:
    void setupUi();
    void setupShortcuts();
    void setToolMode(ToolMode mode);
    void syncOverlayMode();
    void refreshOverlayCoordConverter();
    void refreshObjectList();
    void addConfirmedObject(const AnnotationObject& obj);
    void commitCurrentPromptAsObject();
    void updateSessionFromImage();
    void prepareBackendImage();
    void requestPrediction();
    void clearCurrentPrompt();
    void setStatusText(const QString& text);
    void refreshPromptAfterEdit(bool triggerPrediction);

    // UI
    HImageWidget* m_imageWidget = nullptr;
    AnnotationOverlayWidget* m_overlay = nullptr;
    QListWidget* m_objectList = nullptr;
    QLineEdit* m_categoryEdit = nullptr;
    QToolButton* m_btnPositivePoint = nullptr;
    QToolButton* m_btnNegativePoint = nullptr;
    QToolButton* m_btnBox = nullptr;
    QToolButton* m_btnSelect = nullptr;
    QButtonGroup* m_modeButtonGroup = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_confirmButton = nullptr;

    // 快捷键
    QShortcut* m_scConfirm = nullptr;
    QShortcut* m_scCancel = nullptr;
    QShortcut* m_scDelete = nullptr;
    QShortcut* m_scUndo = nullptr;

    // 状态
    ToolMode m_toolMode = ToolMode::Select;
    QImage m_currentImage;
    QString m_imagePath;
    QString m_backendImagePath;
    std::unique_ptr<QTemporaryFile> m_tempImageFile;

    // 当前未确认 prompt（原图坐标）
    QList<QPointF> m_positivePoints;
    QList<QPointF> m_negativePoints;
    QRectF m_dragBox;

    QList<QPointF> m_previewPolygon;
    QRectF m_previewBbox;
    QString m_previewMaskRle;
    double m_previewScore = 0.0;
    bool m_hasPrediction = false;

    // 标注会话
    AnnotationSession* m_session = nullptr; // 持有 ownership
    SamBackendClient* m_samClient = nullptr;
    QUndoStack* m_undoStack = nullptr;
};

} // namespace DeepLux
