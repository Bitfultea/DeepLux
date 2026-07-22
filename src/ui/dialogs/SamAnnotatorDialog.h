#pragma once

#include <QDialog>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QHash>
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
class QWidget;
class QTemporaryFile;
class QUndoStack;
class QUndoCommand;
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

    // 将标注 overlay 挂到主界面的图像视图上，Dialog 仅作为配置窗口。
    void attachToImageWidget(HImageWidget* imageWidget, const QString& imagePath = QString());

    void applyTheme(bool isDark);

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
    QToolButton* undoButton() const {
        return m_btnUndo;
    }
    QToolButton* redoButton() const {
        return m_btnRedo;
    }

    // 类别管理
    QListWidget* categoryList() const {
        return m_categoryList;
    }
    QStringList categories() const;
    QString currentCategoryLabel() const;
    QColor categoryColor(const QString& label) const;
    void addCategory(const QString& label);
    void removeCategory(const QString& label);
    QPushButton* addCategoryButton() const {
        return m_addCategoryButton;
    }
    QPushButton* removeCategoryButton() const {
        return m_removeCategoryButton;
    }
    QPushButton* openAnnotationButton() const {
        return m_openAnnotationButton;
    }
    QPushButton* exportYoloButton() const {
        return m_exportYoloButton;
    }
    QShortcut* redoShortcut() const {
        return m_scRedo;
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
    QPushButton* importModelButton() const {
        return m_importModelButton;
    }
    QPushButton* initializeEnvironmentButton() const {
        return m_initEnvironmentButton;
    }
    QPushButton* restartServerButton() const {
        return m_restartServerButton;
    }
    QPushButton* cancelButton() const {
        return m_cancelButton;
    }

signals:
    void imageLoaded(const QString& imagePath);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onOpenImage();
    void onOpenAnnotation();
    void onSaveSession();
    void onExportLabelMe();
    void onExportYoloSeg();
    void onImportModel();
    void onInitializeEnvironment();
    void onRestartSam();
    void onConfirm();
    void onCancel();
    void onDeleteSelected();
    void onUndo();
    void onRedo();
    void onModeButtonToggled();
    void onObjectSelectionChanged();
    void onOverlayClicked(const QPointF& imagePoint, Qt::MouseButton button);
    void onOverlayDragEnded(const QPointF& imageStart, const QPointF& imageEnd);
    void onPredictionReady(const QList<QPointF>& polygon, const QRectF& bbox, double score, const QString& maskRle,
                           const QImage& maskImage);
    void onAddCategory();
    void onRemoveCategory();

private:
    void setupUi();
    void setupShortcuts();
    void setToolMode(ToolMode mode);
    void syncOverlayMode();
    void refreshOverlayCoordConverter();
    void refreshObjectList();
    void addConfirmedObject(const AnnotationObject& obj, const QImage& maskImage);
    void commitCurrentPromptAsObject();
    void updateSessionFromImage();
    void prepareBackendImage();
    void requestPrediction();
    void loadSavedModelPath();
    void saveModelPath(const QString& path);
    bool samEnvironmentReadyForUse() const;
    bool samModelReadyForUse() const;
    void refreshSamControlState();
    void clearCurrentPrompt();
    void setStatusText(const QString& text);
    void refreshPromptAfterEdit(bool triggerPrediction);
    HImageWidget* activeImageWidget() const;
    void moveOverlayToImageWidget(HImageWidget* imageWidget);
    void initializeDefaultCategories();

    // UI
    HImageWidget* m_imageWidget = nullptr;
    QPointer<HImageWidget> m_externalImageWidget;  // Fix P0-4: QPointer 防悬空
    AnnotationOverlayWidget* m_overlay = nullptr;  // connected to destroyed() to null
    QListWidget* m_objectList = nullptr;
    QListWidget* m_categoryList = nullptr;
    QLineEdit* m_categoryEdit = nullptr;
    QPushButton* m_addCategoryButton = nullptr;
    QPushButton* m_removeCategoryButton = nullptr;
    QToolButton* m_btnPositivePoint = nullptr;
    QToolButton* m_btnNegativePoint = nullptr;
    QToolButton* m_btnBox = nullptr;
    QToolButton* m_btnSelect = nullptr;
    QToolButton* m_btnUndo = nullptr;
    QToolButton* m_btnRedo = nullptr;
    QButtonGroup* m_modeButtonGroup = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_confirmButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_openImageButton = nullptr;
    QPushButton* m_openAnnotationButton = nullptr;
    QWidget* m_openImageRowWidget = nullptr;
    QPushButton* m_importModelButton = nullptr;
    QPushButton* m_initEnvironmentButton = nullptr;
    QPushButton* m_restartServerButton = nullptr;
    QPushButton* m_exportYoloButton = nullptr;
    QWidget* m_centerContainer = nullptr;

    // 快捷键
    QShortcut* m_scConfirm = nullptr;
    QShortcut* m_scCancel = nullptr;
    QShortcut* m_scDelete = nullptr;
    QShortcut* m_scUndo = nullptr;
    QShortcut* m_scRedo = nullptr;

    // 类别颜色映射
    QStringList m_categories;
    QHash<QString, QColor> m_categoryColors;

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
    QImage m_previewMask;
    double m_previewScore = 0.0;
    bool m_hasPrediction = false;

    // 已确认对象的 mask 图像（按 id 缓存）
    QHash<QString, QImage> m_objectMasks;

    // 标注会话
    AnnotationSession* m_session = nullptr; // 持有 ownership
    SamBackendClient* m_samClient = nullptr;
    QUndoStack* m_undoStack = nullptr;
};

} // namespace DeepLux
