#include "core/agent/AgentActor.h"
#include "dialogs/KnowledgeBaseDialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

// Agent 设置 → 知识库：文档列表（内置/用户）、过滤与预览行为
class TestKnowledgeBaseDialog : public QObject {
    Q_OBJECT

    QTemporaryDir m_appDataDir;

private slots:
    void initTestCase() {
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void testListsBuiltInAndUserDocuments() {
        const QString userDir = AgentActor::userKnowledgeDir();
        QVERIFY(userDir.startsWith(m_appDataDir.path()));
        const QString docPath = QDir(userDir).filePath(QStringLiteral("UserDoc.md"));
        {
            QFile doc(docPath);
            QVERIFY(doc.open(QIODevice::WriteOnly | QIODevice::Text));
            doc.write("# U\n\n## UserSection\n\nuser doc body 7\n");
            doc.close();
        }

        KnowledgeBaseDialog dlg;
        auto* list = dlg.findChild<QListWidget*>("KnowledgeDocList");
        QVERIFY(list);

        QStringList names;
        bool sawBuiltin = false, sawUser = false;
        for (int i = 0; i < list->count(); ++i) {
            const QString path = list->item(i)->data(Qt::UserRole).toString();
            names << QFileInfo(path).fileName();
            if (path == QStringLiteral(":/agent/knowledge/DEEPLUX.md"))
                sawBuiltin = true;
            if (path == docPath) {
                sawUser = true;
                QVERIFY(list->item(i)->data(Qt::UserRole + 1).toBool()); // 用户文档可移除
            }
        }
        QVERIFY2(sawBuiltin, qPrintable(names.join(',')));
        QVERIFY2(sawUser, qPrintable(names.join(',')));

        // 目录标签展示用户知识库位置
        auto* dirLabel = dlg.findChild<QLabel*>("KnowledgeDirLabel");
        QVERIFY(dirLabel && dirLabel->text().contains(QDir::toNativeSeparators(userDir)));

        QVERIFY(QFile::remove(docPath));
    }

    void testFilterNarrowsListAndPreviewFollowsSelection() {
        const QString userDir = AgentActor::userKnowledgeDir();
        const QString docPath = QDir(userDir).filePath(QStringLiteral("FilterTarget.txt"));
        {
            QFile doc(docPath);
            QVERIFY(doc.open(QIODevice::WriteOnly | QIODevice::Text));
            doc.write("filter target body 9\n");
            doc.close();
        }

        KnowledgeBaseDialog dlg;
        auto* list = dlg.findChild<QListWidget*>("KnowledgeDocList");
        auto* filter = dlg.findChild<QLineEdit*>("KnowledgeFilterEdit");
        auto* preview = dlg.findChild<QPlainTextEdit*>("KnowledgePreview");
        QVERIFY(list && filter && preview);

        const int all = list->count();
        filter->setText(QStringLiteral("FilterTarget"));
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), docPath);

        // 选中后预览显示文档内容
        list->setCurrentRow(0);
        QVERIFY(preview->toPlainText().contains(QStringLiteral("filter target body 9")));

        filter->clear();
        QCOMPARE(list->count(), all);

        QVERIFY(QFile::remove(docPath));
    }
};

QTEST_MAIN(TestKnowledgeBaseDialog)
#include "test_knowledgebasedialog.moc"
