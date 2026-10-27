#include "McpStdioServer.h"
#include "core/manager/ConfigManager.h"
#include "core/platform/Platform.h"

#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <cstdio>
#include <iostream>
#include <string>

using DeepLux::ConfigManager;

namespace {
constexpr int kDefaultBridgeTimeoutMs = 30000;
constexpr int kMinBridgeTimeoutMs = 1000;
constexpr int kMaxBridgeTimeoutMs = 300000;

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
            "\n"
            "Configuration (read at startup from <appData>/config/config.json):\n"
            "  agent/bridgeToken        optional shared token for AgentBridge auth\n"
            "  agent/mcpBridgeTimeoutMs per-call timeout, default %d ms, clamped to [%d, %d]\n"
            "\n"
            "Exit codes: 0 = stdin EOF / normal shutdown, 2 = invalid command line.\n",
            DEEPLUX_VERSION_STRING, kDefaultBridgeTimeoutMs, kMinBridgeTimeoutMs, kMaxBridgeTimeoutMs);
}
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

    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray frame(line.data(), static_cast<int>(line.size()));
        const QByteArray response = server.processLine(frame);
        if (response.isEmpty())
            continue;
        fwrite(response.constData(), 1, static_cast<size_t>(response.size()), stdout);
        fflush(stdout);
    }
    return 0;
}
