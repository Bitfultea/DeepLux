#pragma once

// 阶段 5 产出、SAM 批1 抽取为共享 helper：测试内 HTTP 服务，覆盖 SAM 后端
// 四个端点（/health、/set_image、/predict、/unload_image），不依赖 Python/真实
// 模型，可模拟挂起与崩溃。供 test_sambackendclient 与 SAM 流程测试复用。

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>

namespace DeepLuxTest {

class SamTestServer : public QObject {
public:
    explicit SamTestServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, &SamTestServer::onNewConnection);
    }

    bool listen() {
        if (!m_server.listen(QHostAddress::LocalHost, m_port))
            return false;
        m_port = m_server.serverPort(); // 记录已分配端口，供崩溃后原端口重启
        return true;
    }
    // 崩溃恢复：尽量在原端口重新监听
    bool restart() {
        if (m_server.isListening())
            m_server.close();
        return listen();
    }
    void crash() {
        m_server.close();
    }
    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
    bool isListening() const {
        return m_server.isListening();
    }

    // 行为开关
    bool hangAll = false;        // 接受连接但不响应（模拟卡死 → 客户端超时）
    QSet<QString> hangPaths;     // 指定路径接受连接但不响应（模拟单个请求挂起）
    QHash<QString, int> delayMs; // 指定路径延迟 N 毫秒再响应（模拟慢请求）
    QString healthStatus = QStringLiteral("ok");
    QString predictStatus = QStringLiteral("ok");
    QHash<QString, qint64> responseContentLengths;
    // 批3 复核二轮（P1-3）：/predict 响应整体覆盖——构造畸形/缺字段响应，
    // 验证客户端严格解析失败关闭
    bool usePredictOverride = false;
    QJsonObject predictResponseOverride;
    // SAM 批4：多掩膜候选（candidates[0] 与顶层 best 字段一致，score 降序）
    bool includePredictCandidates = true;

    // 请求记录（断言四个端点都被真实调用）
    QStringList requestLog;
    QJsonObject lastPredictBody;
    QString lastSetImagePath;
    int setImageCount = 0; // set_image 计数，用于生成递增的 embedding_id

private:
    void onNewConnection() {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleBytes(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    }

    void handleBytes(QTcpSocket* socket) {
        m_buffers[socket] += socket->readAll();
        if (hangAll)
            return; // 故意不响应，触发客户端超时
        if (!hangPaths.isEmpty()) {
            const QList<QByteArray> requestLine = m_buffers[socket].left(m_buffers[socket].indexOf('\r')).split(' ');
            if (hangPaths.contains(QString::fromLatin1(requestLine.value(1))))
                return; // 该路径故意不响应（请求体可能未收全也无妨，直接挂起）
        }

        QByteArray& data = m_buffers[socket];
        const int headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray header = data.left(headerEnd);
        int contentLength = 0;
        for (const QByteArray& line : header.split('\n')) {
            if (line.trimmed().toLower().startsWith("content-length:"))
                contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
        }
        if (data.size() < headerEnd + 4 + contentLength)
            return; // 请求体未收全

        const QByteArray body = data.mid(headerEnd + 4, contentLength);
        data.remove(0, headerEnd + 4 + contentLength);

        const QList<QByteArray> requestLine = header.left(header.indexOf('\r')).split(' ');
        const QString method = QString::fromLatin1(requestLine.value(0));
        const QString path = QString::fromLatin1(requestLine.value(1));
        requestLog.append(method + " " + path);

        QJsonObject response;
        if (path == "/health") {
            response = QJsonObject{{"status", healthStatus}, {"model_name", "sam_test_vit_b"}};
        } else if (path == "/set_image") {
            lastSetImagePath = QJsonDocument::fromJson(body).object().value("image_path").toString();
            ++setImageCount;
            response = QJsonObject{{"embedding_id", QStringLiteral("emb-%1").arg(setImageCount)},
                                   {"model_name", "sam_test_vit_b"}};
        } else if (path == "/predict") {
            lastPredictBody = QJsonDocument::fromJson(body).object();
            response = predictResponse();
        } else if (path == "/unload_image") {
            response = QJsonObject{{"status", "ok"}};
        } else {
            response = QJsonObject{{"status", "error"}, {"error", "unknown endpoint"}};
        }

        const QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
        QByteArray http;
        http += "HTTP/1.1 200 OK\r\n";
        http += "Content-Type: application/json\r\n";
        http += "Content-Length: " + QByteArray::number(responseContentLengths.value(path, payload.size())) + "\r\n";
        http += "Connection: close\r\n\r\n";
        http += payload;

        const int delay = delayMs.value(path, 0);
        if (delay > 0) {
            QPointer<QTcpSocket> guarded(socket);
            QTimer::singleShot(delay, this, [guarded, http]() {
                if (!guarded)
                    return;
                guarded->write(http);
                guarded->disconnectFromHost();
            });
        } else {
            socket->write(http);
            socket->disconnectFromHost();
        }
    }

    QJsonObject predictResponse() {
        if (usePredictOverride)
            return predictResponseOverride;
        QJsonObject response;
        response["status"] = predictStatus;
        if (predictStatus == "ok") {
            response["polygon"] =
                QJsonArray{QJsonArray{10, 10}, QJsonArray{20, 10}, QJsonArray{20, 20}, QJsonArray{10, 20}};
            response["bbox"] = QJsonArray{10, 10, 10, 10};
            response["score"] = 0.97;
            response["mask_png_base64"] = QString::fromLatin1(tinyMaskPngBase64());
            if (includePredictCandidates) {
                // SAM 批4：candidates[0] 与顶层 best 一致；候选 1/2 为不同区域
                response["candidates"] =
                    QJsonArray{QJsonObject{{"polygon", QJsonArray{QJsonArray{10, 10}, QJsonArray{20, 10},
                                                                  QJsonArray{20, 20}, QJsonArray{10, 20}}},
                                           {"bbox", QJsonArray{10, 10, 10, 10}},
                                           {"score", 0.97},
                                           {"mask_png_base64", QString::fromLatin1(tinyMaskPngBase64())}},
                               QJsonObject{{"polygon", QJsonArray{QJsonArray{30, 30}, QJsonArray{40, 30},
                                                                  QJsonArray{40, 40}, QJsonArray{30, 40}}},
                                           {"bbox", QJsonArray{30, 30, 10, 10}},
                                           {"score", 0.9}},
                               QJsonObject{{"polygon", QJsonArray{QJsonArray{50, 50}, QJsonArray{55, 50},
                                                                  QJsonArray{55, 55}, QJsonArray{50, 55}}},
                                           {"bbox", QJsonArray{50, 50, 5, 5}},
                                           {"score", 0.8}}};
            }
        }
        return response;
    }

    static QByteArray tinyMaskPngBase64() {
        QImage mask(2, 2, QImage::Format_ARGB32);
        mask.fill(QColor(255, 255, 255, 255));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        mask.save(&buffer, "PNG");
        return png.toBase64();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    quint16 m_port = 0; // 0 = 由系统分配；crash() 后 restart() 尽量复用已分配端口
};

// 等待谓词成立（带事件循环），超时返回 false
inline bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

} // namespace DeepLuxTest
