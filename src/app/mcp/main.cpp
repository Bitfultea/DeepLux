#include "McpStdioServer.h"
#include "core/manager/ConfigManager.h"
#include "core/platform/Platform.h"

#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QThread>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using DeepLux::ConfigManager;

namespace {
constexpr int kDefaultBridgeTimeoutMs = 30000;
constexpr int kMinBridgeTimeoutMs = 1000;
constexpr int kMaxBridgeTimeoutMs = 300000;
constexpr int kReadChunkBytes = 64 * 1024;

/// 读取"当前可用"字节：fread 会尝试填满 64KB，在长连接管道上会无限阻塞。
int readStdinChunk(char* buffer, int size) {
#ifdef _WIN32
    return _read(_fileno(stdin), buffer, size);
#else
    const ssize_t count = ::read(fileno(stdin), buffer, static_cast<size_t>(size));
    return count < 0 ? -1 : static_cast<int>(count);
#endif
}

/// 诊断一律走 stderr，stdout 只承载 JSON-RPC 协议帧
void stderrMessageHandler(QtMsgType, const QMessageLogContext&, const QString& message) {
    const QByteArray utf8 = message.toUtf8();
    fprintf(stderr, "%s\n", utf8.constData());
    fflush(stderr);
}

void printUsage(FILE* out) {
    fprintf(out,
            "deeplux-mcp-bridge %s - MCP stdio bridge for DeepLux\n"
            "\n"
            "Usage:\n"
            "  deeplux-mcp-bridge [--help|--version]\n"
            "\n"
            "Without options the process speaks JSON-RPC 2.0 MCP over stdin/stdout\n"
            "(one JSON object per line, UTF-8, max 4 MiB per line). Diagnostics go to\n"
            "stderr only. Tools are forwarded to a running DeepLux GUI through\n"
            "AgentBridge v1.0 (named pipe \\\\.\\pipe\\deeplux_agent on Windows); if the\n"
            "GUI is not running, tools/call returns JSON-RPC error -32001.\n"
            "\n"
            "External calls are read-only: tools whose ToolSchema readOnly is false are\n"
            "rejected locally (isError + code=permission_denied) and never forwarded.\n"
            "notifications/cancelled suppresses the late response for the matching id.\n"
            "\n"
            "Configuration (read at startup from <appData>/config/config.json):\n"
            "  agent/bridgeToken        optional shared token for AgentBridge auth\n"
            "  agent/mcpBridgeTimeoutMs per-call timeout, default %d ms, clamped to [%d, %d]\n"
            "\n"
            "Exit codes: 0 = stdin EOF / normal shutdown, 2 = invalid command line.\n",
            DEEPLUX_VERSION_STRING, kDefaultBridgeTimeoutMs, kMinBridgeTimeoutMs, kMaxBridgeTimeoutMs);
}

/**
 * stdin 读取线程：Windows 管道与 Linux 都通过 fread 工作，不依赖 Unix fd。
 * 只负责有界分块读取；分帧与协议状态全部在主线程的 McpStdioServer 内。
 */
class StdinReader : public QThread {
public:
    std::function<void(const QByteArray&)> onChunk;
    std::function<void()> onFinished;

protected:
    void run() override {
        char buffer[kReadChunkBytes];
        while (true) {
            const int count = readStdinChunk(buffer, kReadChunkBytes);
            if (count > 0) {
                if (onChunk)
                    onChunk(QByteArray(buffer, count));
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break; // 0 = EOF；其它错误也退出，不挂死
        }
        if (onFinished)
            onFinished();
    }
};
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QStringLiteral("--help") || argument == QStringLiteral("-h")) {
            printUsage(stdout);
            return 0;
        }
        if (argument == QStringLiteral("--version")) {
            fprintf(stdout, "%s\n", DEEPLUX_VERSION_STRING);
            return 0;
        }
        fprintf(stderr, "deeplux-mcp-bridge: unknown option '%s' (try --help)\n", argv[i]);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    qInstallMessageHandler(stderrMessageHandler);
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("deeplux-mcp-bridge"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(DEEPLUX_VERSION_STRING));

    ConfigManager::instance().initialize();
    DeepLux::Mcp::McpStdioServer server;
    server.bridgeClient()->setAuthToken(
        ConfigManager::instance().groupString(QStringLiteral("agent"), QStringLiteral("bridgeToken")));
    const int configuredTimeout = ConfigManager::instance().groupInt(
        QStringLiteral("agent"), QStringLiteral("mcpBridgeTimeoutMs"), kDefaultBridgeTimeoutMs);
    server.bridgeClient()->setOperationTimeoutMs(qBound(kMinBridgeTimeoutMs, configuredTimeout, kMaxBridgeTimeoutMs));

    QObject::connect(&server, &DeepLux::Mcp::McpStdioServer::responseReady, [](const QByteArray& frame) {
        if (frame.isEmpty())
            return;
        fwrite(frame.constData(), 1, static_cast<size_t>(frame.size()), stdout);
        fflush(stdout);
    });

    StdinReader reader;
    reader.onChunk = [&server](const QByteArray& chunk) {
        QMetaObject::invokeMethod(
            &server, [&server, chunk]() { server.ingestChunk(chunk); }, Qt::QueuedConnection);
    };
    reader.onFinished = [&server]() {
        QMetaObject::invokeMethod(
            &server,
            [&server]() {
                server.finishInput();
                QCoreApplication::quit();
            },
            Qt::QueuedConnection);
    };
    reader.start();

    const int exitCode = application.exec();
    reader.wait(2000);
    return exitCode;
}
