#include "KnowledgeBaseDialog.h"

#include "../widgets/AppIconProvider.h"
#include "common/Logger.h"
#include "core/agent/AgentActor.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

namespace DeepLux {

namespace {
constexpr qint64 kPreviewLimit = 512 * 1024; // 预览截断上限，避免超大文档卡住界面
}

KnowledgeBaseDialog::KnowledgeBaseDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("软件知识库"));
    setMinimumSize(760, 520);
    setupUi();
    reloadList();
}

void KnowledgeBaseDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(10);

    auto* hintLabel = new QLabel(tr("Agent 通过 read_documentation 工具检索以下文档；"
                                    "添加的 Markdown/文本文档会复制到用户知识库目录。"),
                                 this);
    hintLabel->setWordWrap(true);
    mainLayout->addWidget(hintLabel);

    m_dirLabel = new QLabel(this);
    m_dirLabel->setObjectName(QStringLiteral("KnowledgeDirLabel"));
    m_dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(m_dirLabel);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setObjectName(QStringLiteral("KnowledgeFilterEdit"));
    m_filterEdit->setPlaceholderText(tr("按文件名过滤..."));
    m_filterEdit->setClearButtonEnabled(true);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &KnowledgeBaseDialog::onFilterChanged);
    mainLayout->addWidget(m_filterEdit);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    m_list = new QListWidget(splitter);
    m_list->setObjectName(QStringLiteral("KnowledgeDocList"));
    connect(m_list, &QListWidget::itemSelectionChanged, this, &KnowledgeBaseDialog::onSelectionChanged);
    splitter->addWidget(m_list);

    m_preview = new QPlainTextEdit(splitter);
    m_preview->setObjectName(QStringLiteral("KnowledgePreview"));
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    splitter->addWidget(m_preview);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    mainLayout->addWidget(splitter, 1);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    auto* addBtn = new QPushButton(tr("添加文档"), this);
    addBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#16A34A")));
    m_removeBtn = new QPushButton(tr("移除"), this);
    m_removeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Delete, 16, QColor("#DC2626")));
    m_removeBtn->setEnabled(false);
    auto* openBtn = new QPushButton(tr("打开目录"), this);
    openBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::OpenFolder, 16, QColor("#2563EB")));
    auto* closeBtn = new QPushButton(tr("关闭"), this);
    closeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 16, QColor("#4B5563")));

    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(m_removeBtn);
    btnLayout->addWidget(openBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(closeBtn);
    mainLayout->addLayout(btnLayout);

    connect(addBtn, &QPushButton::clicked, this, &KnowledgeBaseDialog::onAddClicked);
    connect(m_removeBtn, &QPushButton::clicked, this, &KnowledgeBaseDialog::onRemoveClicked);
    connect(openBtn, &QPushButton::clicked, this, &KnowledgeBaseDialog::onOpenLocationClicked);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}

void KnowledgeBaseDialog::reloadList() {
    const QString selected = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();

    m_list->clear();
    const QString userDir = AgentActor::userKnowledgeDir();
    m_dirLabel->setText(tr("用户文档目录：%1").arg(QDir::toNativeSeparators(userDir)));

    const QString filter = m_filterEdit->text().trimmed();
    const QStringList paths = AgentActor::knowledgeDocumentPaths();
    for (const QString& path : paths) {
        const QString name = QFileInfo(path).fileName();
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        const bool isUser = path.startsWith(userDir);
        auto* item = new QListWidgetItem(m_list);
        item->setText(QStringLiteral("%1    [%2]").arg(name, isUser ? tr("用户") : tr("内置")));
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, isUser);
        item->setToolTip(QDir::toNativeSeparators(path));
        if (path == selected) {
            item->setSelected(true);
        }
    }

    if (!selected.isEmpty() && m_list->currentItem()) {
        showPreview(m_list->currentItem());
    } else {
        m_preview->clear();
        m_removeBtn->setEnabled(false);
    }
}

void KnowledgeBaseDialog::showPreview(QListWidgetItem* item) {
    if (!item) {
        m_preview->clear();
        m_removeBtn->setEnabled(false);
        return;
    }

    m_removeBtn->setEnabled(item->data(Qt::UserRole + 1).toBool());

    QFile file(item->data(Qt::UserRole).toString());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_preview->setPlainText(tr("无法读取文档：%1").arg(file.errorString()));
        return;
    }
    QByteArray data = file.read(kPreviewLimit + 1);
    file.close();
    if (data.size() > kPreviewLimit) {
        data.resize(kPreviewLimit);
        m_preview->setPlainText(QString::fromUtf8(data) + tr("\n\n... (文档过大，仅预览前 512 KB)"));
    } else {
        m_preview->setPlainText(QString::fromUtf8(data));
    }
}

void KnowledgeBaseDialog::onSelectionChanged() {
    showPreview(m_list->currentItem());
}

void KnowledgeBaseDialog::onFilterChanged(const QString& text) {
    Q_UNUSED(text);
    reloadList();
}

void KnowledgeBaseDialog::onAddClicked() {
    const QStringList files =
        QFileDialog::getOpenFileNames(this, tr("添加知识库文档"), QString(), tr("知识库文档 (*.md *.markdown *.txt)"));
    if (files.isEmpty()) {
        return;
    }

    const QString userDir = AgentActor::userKnowledgeDir();
    QString lastAdded;
    for (const QString& src : files) {
        const QString name = QFileInfo(src).fileName();
        const QString dest = QDir(userDir).filePath(name);
        if (QFileInfo::exists(dest)) {
            const auto answer =
                QMessageBox::question(this, tr("添加知识库文档"), tr("“%1”已存在于知识库，是否覆盖？").arg(name));
            if (answer != QMessageBox::Yes) {
                continue;
            }
            QFile::remove(dest);
        }
        if (QFile::copy(src, dest)) {
            lastAdded = dest;
            Logger::instance().info(tr("知识库添加文档: %1").arg(name), QStringLiteral("Agent"));
        } else {
            QMessageBox::warning(this, tr("添加知识库文档"), tr("复制“%1”失败。").arg(name));
        }
    }

    reloadList();
    if (!lastAdded.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(Qt::UserRole).toString() == lastAdded) {
                m_list->setCurrentRow(i);
                break;
            }
        }
    }
}

void KnowledgeBaseDialog::onRemoveClicked() {
    QListWidgetItem* item = m_list->currentItem();
    if (!item) {
        return;
    }
    if (!item->data(Qt::UserRole + 1).toBool()) {
        QMessageBox::information(this, tr("移除"), tr("内置文档不可移除。"));
        return;
    }

    const QString path = item->data(Qt::UserRole).toString();
    const auto answer = QMessageBox::question(
        this, tr("移除"),
        tr("确定从知识库移除“%1”？\n（添加时是复制入库，原始文件不受影响）").arg(QFileInfo(path).fileName()));
    if (answer != QMessageBox::Yes) {
        return;
    }
    if (QFile::remove(path)) {
        Logger::instance().info(tr("知识库移除文档: %1").arg(QFileInfo(path).fileName()), QStringLiteral("Agent"));
    } else {
        QMessageBox::warning(this, tr("移除"), tr("移除失败：%1").arg(path));
    }
    reloadList();
}

void KnowledgeBaseDialog::onOpenLocationClicked() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(AgentActor::userKnowledgeDir()));
}

} // namespace DeepLux
