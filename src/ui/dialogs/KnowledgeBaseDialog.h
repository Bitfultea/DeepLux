#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;

namespace DeepLux {

/**
 * @brief 软件知识库管理对话框
 *
 * 列出内置知识库文档（qrc，只读）与用户添加的文档（appData/knowledge），
 * 支持按文件名过滤、预览内容、添加 Markdown/文本文档和删除用户文档。
 * 添加的文档会被 Agent 的 read_documentation 工具一并检索。
 */
class KnowledgeBaseDialog : public QDialog {
    Q_OBJECT

public:
    explicit KnowledgeBaseDialog(QWidget* parent = nullptr);

private slots:
    void onAddClicked();
    void onRemoveClicked();
    void onOpenLocationClicked();
    void onSelectionChanged();
    void onFilterChanged(const QString& text);

private:
    void setupUi();
    void reloadList();
    void showPreview(QListWidgetItem* item);

    QLineEdit* m_filterEdit = nullptr;
    QListWidget* m_list = nullptr;
    QPlainTextEdit* m_preview = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QLabel* m_dirLabel = nullptr;
};

} // namespace DeepLux
