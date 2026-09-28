// SAM 性能基线工具（原生推理计划·阶段0）
//
// 目的：固定「同图 N 次混合 prompt」测量口径，供三类场景复用：
//   1) 阶段0 基线：现有 Python sam_server HTTP 路径（生产机执行，真实模型）
//   2) 阶段2/3：原生 C++ 推理接入后同口径对照（后续阶段扩展 native 模式）
//   3) 工具自检：--embedded-stub 内嵌 SamTestServer（无 Python 依赖），
//      测得的是客户端+HTTP+JSON 路径开销（推理耗时≈0），不代表模型性能
//
// 指标口径（与计划书一致）：
//   - 首次编码：setImage → embeddingReady 全程耗时（真实后端含服务器读图+
//     ViT 编码；stub 模式仅 HTTP 往返）
//   - 重复预测：同一 embedding 上 N 次 predict → predictionReady 逐项耗时，
//     报告 P50/P95/P99/max/mean 与分组（正点/正负点/框选）细分
//   - prompt 序列为纯整数运算生成（无随机数），同尺寸图像在任何机台上
//     坐标完全一致，保证跨机台/跨阶段可比
//
// 失败关闭：任何一次请求错误或超时 → 退出码 4，不输出部分 JSON。

#include "core/agent/SamBackendClient.h"
#include "helpers/SamTestServer.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QScopedPointer>
#include <QSysInfo>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

using namespace DeepLux;
using DeepLuxTest::SamTestServer;

namespace {

struct PromptSpec {
    QString kind; // "pos" | "posneg" | "box"
    QList<QPointF> positive;
    QList<QPointF> negative;
    QRectF box;
};

// 确定性 prompt 配方（记录于 README，禁止改动以保证跨阶段可比）：
//   i%10 ∈ [0,3]  单个正点          （40%）
//   i%10 ∈ [4,6]  正点 + 负点       （30%）
//   i%10 ∈ [7,9]  框选（w/8 × h/8） （30%）
// 坐标全部由 i 与图像宽高经整数取模生成，落在图像内部安全边距内。
QList<PromptSpec> buildPromptRecipe(int runs, int imageW, int imageH) {
    QList<PromptSpec> prompts;
    prompts.reserve(runs);
    const int spanX = qMax(1, imageW - 200);
    const int spanY = qMax(1, imageH - 200);
    const int boxW = qMax(8, imageW / 8);
    const int boxH = qMax(8, imageH / 8);
    const int spanBoxX = qMax(1, imageW - boxW - 100);
    const int spanBoxY = qMax(1, imageH - boxH - 100);
    for (int i = 0; i < runs; ++i) {
        PromptSpec spec;
        const int kind = i % 10;
        const QPointF pos(100.0 + double((i * 127) % spanX), 100.0 + double((i * 311) % spanY));
        if (kind <= 3) {
            spec.kind = QStringLiteral("pos");
            spec.positive = {pos};
        } else if (kind <= 6) {
            spec.kind = QStringLiteral("posneg");
            spec.positive = {pos};
            spec.negative = {
                QPointF(100.0 + double(((i + runs) * 127) % spanX), 100.0 + double(((i + runs) * 311) % spanY))};
        } else {
            spec.kind = QStringLiteral("box");
            spec.box = QRectF(50.0 + double((i * 211) % spanBoxX), 50.0 + double((i * 97) % spanBoxY), double(boxW),
                              double(boxH));
        }
        prompts.append(spec);
    }
    return prompts;
}

// 确定性合成基准图 v2（阶段0）：低频平滑底 + 12×12 均匀灰度单元（2px 暗
// 边界）+ 单元内对比小块。v1 的高频斜纹底会诱导 SAM 输出退化全图掩膜与
// 越界 score（实测 1.0134>1，官方 iou_prediction_head 无 sigmoid/clamp，
// 见基线文档发现 F1），点 prompt 无法给出有效基线；v2 保证任意配方点落
// 在边界清晰的可分割单元内。内容仅由 --size 整数运算生成，跨平台逐像素
// 一致。真实工业图集到位后以 --image 替代（性能口径只依赖分辨率，内容
// 影响 score 合法性与掩膜形态）。
bool generateSyntheticImage(const QString& path, int size) {
    if (size < 64 || size > 16384) {
        std::fprintf(stderr, "sam_bench: --size 必须在 [64,16384]，收到 %d\n", size);
        return false;
    }
    QImage img(size, size, QImage::Format_RGB32);
    // 低频平滑底：96..159 灰度缓变
    for (int y = 0; y < size; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < size; ++x) {
            const int v = 96 + ((x / 32 + y / 48) % 64);
            row[x] = qRgb(v, v, v);
        }
    }
    QPainter painter(&img);
    const int grid = 12;
    const int cell = size / grid;
    if (cell >= 16) {
        for (int iy = 0; iy < grid; ++iy) {
            for (int ix = 0; ix < grid; ++ix) {
                const int g = 30 + ((ix * 31 + iy * 57) % 200);
                painter.fillRect(QRect(ix * cell, iy * cell, cell, cell), QColor(g, g, g));
                // 单元中心对比小块：给点 prompt 提供局部目标
                const int g2 = (g + 110) % 256;
                painter.fillRect(QRect(ix * cell + cell * 3 / 8, iy * cell + cell * 3 / 8, cell / 4, cell / 4),
                                 QColor(g2, g2, g2));
            }
        }
        // 2px 暗边界：单元间清晰分界，抑制跨单元合并
        for (int k = 1; k < grid; ++k) {
            painter.fillRect(QRect(k * cell - 1, 0, 2, size), QColor(20, 20, 20));
            painter.fillRect(QRect(0, k * cell - 1, size, 2), QColor(20, 20, 20));
        }
    }
    painter.end();
    return img.save(path, "PNG");
}

QString fileSha256(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

double percentile(const QList<double>& sortedAsc, double p) {
    if (sortedAsc.isEmpty())
        return 0.0;
    // nearest-rank：idx = ceil(p/100 * N) - 1
    const int idx = qBound(0, int(std::ceil(p / 100.0 * double(sortedAsc.size()))) - 1, sortedAsc.size() - 1);
    return sortedAsc[idx];
}

QJsonObject statsFor(const QList<double>& samples) {
    QJsonObject stats;
    if (samples.isEmpty())
        return stats;
    QList<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double v : sorted)
        sum += v;
    stats["count"] = sorted.size();
    stats["minMs"] = sorted.first();
    stats["maxMs"] = sorted.last();
    stats["meanMs"] = sum / double(sorted.size());
    stats["p50Ms"] = percentile(sorted, 50.0);
    stats["p95Ms"] = percentile(sorted, 95.0);
    stats["p99Ms"] = percentile(sorted, 99.0);
    return stats;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sam_bench"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("DeepLux SAM 性能基线工具（阶段0）：首次编码 + N 次混合 prompt 重复预测"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption optServer(QStringLiteral("server"),
                                       QStringLiteral("sam_server 基地址（与 --embedded-stub 二选一）"),
                                       QStringLiteral("url"));
    const QCommandLineOption optEmbeddedStub(QStringLiteral("embedded-stub"),
                                             QStringLiteral("使用内嵌 SamTestServer（无 Python；仅测路径开销）"));
    const QCommandLineOption optImage(QStringLiteral("image"), QStringLiteral("基准图像路径（真实后端必须可读）"),
                                      QStringLiteral("path"));
    const QCommandLineOption optGenImage(QStringLiteral("gen-image"),
                                         QStringLiteral("生成确定性合成基准图到该路径（与 --image 二选一）"),
                                         QStringLiteral("path"));
    const QCommandLineOption optSize(QStringLiteral("size"), QStringLiteral("合成图边长（默认 3000）"),
                                     QStringLiteral("n"), QStringLiteral("3000"));
    const QCommandLineOption optRuns(QStringLiteral("runs"), QStringLiteral("重复预测次数（默认 100）"),
                                     QStringLiteral("n"), QStringLiteral("100"));
    const QCommandLineOption optWarmup(QStringLiteral("warmup"),
                                       QStringLiteral("计时前的预热预测次数（默认 0，不计入统计）"),
                                       QStringLiteral("n"), QStringLiteral("0"));
    const QCommandLineOption optTimeout(QStringLiteral("timeout"), QStringLiteral("单请求超时 ms（默认 120000）"),
                                        QStringLiteral("ms"), QStringLiteral("120000"));
    const QCommandLineOption optJson(QStringLiteral("json"), QStringLiteral("原始结果 JSON 输出路径"),
                                     QStringLiteral("path"));
    const QCommandLineOption optTag(QStringLiteral("tag"), QStringLiteral("自由标签（写入 JSON，如机台/阶段标识）"),
                                    QStringLiteral("text"));
    const QCommandLineOption optOnError(
        QStringLiteral("on-error"),
        QStringLiteral("predict 失败策略：fail=失败关闭退出（默认，产品语义）；continue=记录违约样本并继续——"
                       "用于 F1 类契约冲突（真实 SAM score 可>1）下取得有效样本延迟；违约样本不计入延迟"
                       "统计并全量写入 JSON violations；连续 5 次失败判定硬故障中止"),
        QStringLiteral("mode"), QStringLiteral("fail"));
    const QCommandLineOption optInject(
        QStringLiteral("inject-invalid-embedding"),
        QStringLiteral("仅 --embedded-stub：前 N 次 /predict 注入 invalid_embedding，验证客户端自动重试链"
                       "下的计时正确性（retryEvents 入 JSON；回归见 check_retry_regression.sh）"),
        QStringLiteral("n"), QStringLiteral("0"));
    const QCommandLineOption optInjectPredictError(
        QStringLiteral("inject-predict-error"),
        QStringLiteral("仅 --embedded-stub：/predict 持续返回 {\"status\":\"error\"}（非 F1 错误口径），"
                       "验证 continue 模式对非 score 越界错误同样失败关闭"));
    parser.addOptions({optServer, optEmbeddedStub, optImage, optGenImage, optSize, optRuns, optWarmup, optTimeout,
                       optJson, optTag, optOnError, optInject, optInjectPredictError});
    parser.process(app);

    const QString onErrorMode = parser.value(optOnError);
    if (onErrorMode != QLatin1String("fail") && onErrorMode != QLatin1String("continue")) {
        std::fprintf(stderr, "sam_bench: --on-error 必须为 fail 或 continue\n");
        return 2;
    }
    bool okInject = false;
    const int injectInvalidEmbedding = parser.value(optInject).toInt(&okInject);
    if (!okInject || injectInvalidEmbedding < 0 || injectInvalidEmbedding > 1000) {
        std::fprintf(stderr, "sam_bench: --inject-invalid-embedding 必须为 [0,1000] 整数\n");
        return 2;
    }
    if (injectInvalidEmbedding > 0 && !parser.isSet(optEmbeddedStub)) {
        std::fprintf(stderr, "sam_bench: --inject-invalid-embedding 仅支持 --embedded-stub（无法注入外部服务器）\n");
        return 2;
    }
    const bool injectPredictError = parser.isSet(optInjectPredictError);
    if (injectPredictError && !parser.isSet(optEmbeddedStub)) {
        std::fprintf(stderr, "sam_bench: --inject-predict-error 仅支持 --embedded-stub（无法注入外部服务器）\n");
        return 2;
    }

    const bool embeddedStub = parser.isSet(optEmbeddedStub);
    const QString serverArg = parser.value(optServer);
    if (embeddedStub == !serverArg.isEmpty()) {
        std::fprintf(stderr, "sam_bench: --server 与 --embedded-stub 必须恰好提供一个\n");
        return 2;
    }
    const bool hasImage = parser.isSet(optImage);
    const bool hasGen = parser.isSet(optGenImage);
    if (hasImage == hasGen) {
        std::fprintf(stderr, "sam_bench: --image 与 --gen-image 必须恰好提供一个\n");
        return 2;
    }
    // 每个数值参数独立校验 ok：共享单个 ok 会被后续成功解析覆盖
    // （--warmup abc 曾因此被误收，回归见 check_arg_guards.sh）
    bool okRuns = false;
    bool okWarmup = false;
    bool okTimeout = false;
    bool okSize = false;
    const int runs = parser.value(optRuns).toInt(&okRuns);
    const int warmup = parser.value(optWarmup).toInt(&okWarmup);
    const int timeoutMs = parser.value(optTimeout).toInt(&okTimeout);
    const int synthSize = parser.value(optSize).toInt(&okSize);
    if (!okRuns || !okWarmup || !okTimeout || !okSize || runs < 1 || runs > 100000 || warmup < 0 || warmup > 10000 ||
        timeoutMs < 100 || timeoutMs > 600000 || synthSize < 64 || synthSize > 16384) {
        std::fprintf(stderr, "sam_bench: 数值参数越界（runs∈[1,100000] warmup∈[0,10000] timeout∈[100,600000] "
                             "size∈[64,16384]，且必须为整数）\n");
        return 2;
    }

    // ===== 图像 =====
    const QString imagePath = hasImage ? parser.value(optImage) : parser.value(optGenImage);
    if (hasGen && !generateSyntheticImage(imagePath, synthSize)) {
        std::fprintf(stderr, "sam_bench: 合成图生成失败：%s\n", qPrintable(imagePath));
        return 3;
    }
    const QImage probe(imagePath);
    if (probe.isNull()) {
        std::fprintf(stderr, "sam_bench: 图像不可读或格式不支持：%s\n", qPrintable(imagePath));
        return 3;
    }
    const QString imageSha = fileSha256(imagePath);

    // ===== 后端（内嵌 stub 或外部 serverUrl）=====
    QScopedPointer<SamTestServer> stub;
    QString serverUrl = serverArg;
    if (embeddedStub) {
        stub.reset(new SamTestServer());
        if (!stub->listen()) {
            std::fprintf(stderr, "sam_bench: 内嵌 stub 监听失败\n");
            return 4;
        }
        stub->predictInvalidEmbeddingOnce = injectInvalidEmbedding; // 复核二轮 P1-2：重试计时回归注入
        if (injectPredictError)
            stub->predictStatus = QStringLiteral("error"); // 复核二轮 P2-4：非 F1 错误收窄回归
        serverUrl = stub->url();
    }

    SamBackendClient client;
    client.setServerUrl(serverUrl);
    client.setTimeoutMs(timeoutMs);

    // 单操作同步收口：与 runSamPredictBatch 同范式（本地事件循环 + 兜底超时）。
    // 复核二轮 P1-2：完成信号按操作类型隔离——setImage 只等 embeddingReady；
    // predict 只等 predictionReady。predict 期间出现 embeddingReady 只能来自
    // 客户端对 invalid_embedding 的自动重试链（重 setImage → 重发 predict），
    // 记为重试事件并继续等待真正的预测结果；若在本操作已完成后才收到，
    // 判定计时早退缺陷（内部不变量，exit 5）。
    enum class OpKind { SetImage, Predict };
    QString opError;
    int retryEvents = 0;
    bool timingInvariantViolated = false;
    const auto runOp = [&](OpKind kind, const std::function<void()>& start, double& outMs) -> bool {
        opError.clear();
        bool done = false;
        QEventLoop loop;
        QElapsedTimer timer;
        const auto finish = [&](const QString& err) {
            if (!done) {
                opError = err;
                done = true;
                loop.quit();
            }
        };
        auto errConn = QObject::connect(&client, &SamBackendClient::errorOccurred, &loop,
                                        [&finish](const QString& msg) { finish(msg); });
        // 兜底：客户端自身定时器故障时仍可退出（timeout+2s，与 SamPredictor 口径一致）
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&finish]() { finish(QStringLiteral("本地等待超时")); });
        deadline.start(timeoutMs + 2000);
        QMetaObject::Connection completionConn;
        QMetaObject::Connection retryConn;
        if (kind == OpKind::SetImage) {
            completionConn =
                QObject::connect(&client, &SamBackendClient::embeddingReady, &loop, [&finish]() { finish(QString()); });
        } else {
            completionConn = QObject::connect(&client, &SamBackendClient::predictionReady, &loop,
                                              [&finish](const QList<QPointF>&, const QRectF&, double, const QString&,
                                                        const QImage&) { finish(QString()); });
            retryConn = QObject::connect(&client, &SamBackendClient::embeddingReady, &loop, [&]() {
                if (done)
                    timingInvariantViolated = true; // 完成后不应再收到重试信号
                else
                    ++retryEvents;
            });
        }
        timer.start();
        start();
        loop.exec();
        outMs = double(timer.nsecsElapsed()) / 1e6; // 亚毫秒路径（stub）也保留精度
        QObject::disconnect(errConn);
        QObject::disconnect(completionConn);
        if (retryConn)
            QObject::disconnect(retryConn);
        deadline.stop();
        return done && opError.isEmpty();
    };

    // ===== 首次编码 =====
    double setMs = 0.0;
    if (!runOp(
            OpKind::SetImage, [&]() { client.setImage(imagePath); }, setMs)) {
        std::fprintf(stderr, "sam_bench: setImage 失败：%s\n", qPrintable(opError));
        return 4;
    }
    const QString embeddingId = client.currentEmbeddingId();

    // ===== 混合 prompt 序列（配方确定性，见 README）=====
    const QList<PromptSpec> recipe = buildPromptRecipe(runs + warmup, probe.width(), probe.height());
    QList<double> latenciesMs;
    QList<double> warmupMs;
    QList<double> posLat;
    QList<double> posnegLat;
    QList<double> boxLat;
    latenciesMs.reserve(runs);
    int firstCandidates = -1;
    int lastCandidates = -1;
    double firstScore = 0.0;
    double lastScore = 0.0;
    // score 在信号里采样（runOp 内部的连接先于本连接触发也无妨，值同一帧更新）
    auto scoreConn = QObject::connect(
        &client, &SamBackendClient::predictionReady, &app,
        [&lastScore](const QList<QPointF>&, const QRectF&, double s, const QString&, const QImage&) { lastScore = s; });

    QJsonArray violations;
    for (int i = 0; i < runs + warmup; ++i) {
        const PromptSpec& spec = recipe[i];
        double ms = 0.0;
        if (!runOp(
                OpKind::Predict, [&]() { client.predict(spec.positive, spec.negative, spec.box); }, ms)) {
            // 复核二轮 P2-4：continue 仅放行明确识别的 F1 score 越界（顶层与
            // candidates[i] 拒绝串同口径）；网络错误/超时/其他解析失败无论
            // 模式一律失败关闭退出，不得混入违约统计
            const bool f1ScoreViolation = opError.contains(QStringLiteral("score 必须为 [0,1] 有限数值"));
            std::fprintf(stderr, "sam_bench: predict[%d]（%s）失败：%s%s\n", i, qPrintable(spec.kind),
                         qPrintable(opError), f1ScoreViolation ? "［F1 score 越界］" : "");
            if (onErrorMode == QLatin1String("fail") || !f1ScoreViolation) {
                QObject::disconnect(scoreConn);
                return 4;
            }
            violations.append(QJsonObject{{"index", i},
                                          {"kind", spec.kind},
                                          {"error", opError},
                                          {"measured", i >= warmup},
                                          {"classification", QStringLiteral("F1-score-out-of-range")}});
            continue; // 越界样本不计入延迟统计（拒绝发生在 PNG 解码前，延迟不可比）
        }
        const int candCount = client.lastCandidates().size();
        if (i < warmup) {
            warmupMs.append(ms);
            continue;
        }
        if (latenciesMs.isEmpty()) { // 第一个计时样本
            firstCandidates = candCount;
            firstScore = lastScore;
        }
        latenciesMs.append(ms);
        lastCandidates = candCount;
        if (spec.kind == QLatin1String("pos"))
            posLat.append(ms);
        else if (spec.kind == QLatin1String("posneg"))
            posnegLat.append(ms);
        else
            boxLat.append(ms);
    }
    QObject::disconnect(scoreConn);
    if (timingInvariantViolated) {
        std::fprintf(stderr, "sam_bench: 内部不变量违背：predict 完成后又收到 embeddingReady（计时早退缺陷）\n");
        return 5;
    }
    client.unloadImage();

    // ===== 统计与输出 =====
    QJsonArray latencyArr;
    for (double v : latenciesMs)
        latencyArr.append(v);
    const QJsonObject result{
        {"tool", QStringLiteral("sam_bench")},
        {"toolVersion", QStringLiteral("1.0")},
        {"tag", parser.value(optTag)},
        {"mode", embeddedStub ? QStringLiteral("embedded-stub") : QStringLiteral("remote-http")},
        {"serverUrl", serverUrl},
        {"modelName", client.modelName()},
        {"embeddingId", embeddingId},
        {"environment", QJsonObject{{"os", QSysInfo::prettyProductName()},
                                    {"kernel", QSysInfo::kernelVersion()},
                                    {"arch", QSysInfo::currentCpuArchitecture()},
                                    {"qt", QString::fromLatin1(qVersion())}}},
        {"image",
         QJsonObject{{"path", imagePath}, {"width", probe.width()}, {"height", probe.height()}, {"sha256", imageSha}}},
        {"runs", runs},
        {"runsValid", latenciesMs.size()},
        {"warmup", warmup},
        {"timeoutMs", timeoutMs},
        {"onError", onErrorMode},
        {"injectedInvalidEmbedding", injectInvalidEmbedding},
        {"injectedPredictError", injectPredictError},
        {"retryEvents", retryEvents},
        {"violationCount", violations.size()},
        {"violations", violations},
        {"promptRecipe", QStringLiteral("i%10: 0-3=pos, 4-6=posneg, 7-9=box; 整数取模坐标（README 配方）")},
        {"initialEncodeMs", setMs},
        {"firstCandidates", firstCandidates},
        {"lastCandidates", lastCandidates},
        {"firstScore", firstScore},
        {"lastScore", lastScore},
        {"latenciesMs", latencyArr},
        {"predictStats", statsFor(latenciesMs)},
        {"predictStatsByKind",
         QJsonObject{{"pos", statsFor(posLat)}, {"posneg", statsFor(posnegLat)}, {"box", statsFor(boxLat)}}},
        {"warmupStats", statsFor(warmupMs)},
    };

    const QJsonObject predictStats = result.value("predictStats").toObject();
    std::printf("== sam_bench (%s) ==\n", qPrintable(result.value("mode").toString()));
    std::printf("server=%s model=%s\n", qPrintable(serverUrl), qPrintable(client.modelName()));
    std::printf("image=%dx%d sha256=%s...\n", probe.width(), probe.height(), qPrintable(imageSha.left(16)));
    std::printf("首次编码(set_image): %.1f ms\n", setMs);
    std::printf("重复预测 x%d: P50=%.1f P95=%.1f P99=%.1f max=%.1f mean=%.1f ms\n", runs,
                predictStats.value("p50Ms").toDouble(), predictStats.value("p95Ms").toDouble(),
                predictStats.value("p99Ms").toDouble(), predictStats.value("maxMs").toDouble(),
                predictStats.value("meanMs").toDouble());
    const QJsonObject byKind = result.value("predictStatsByKind").toObject();
    for (const char* kind : {"pos", "posneg", "box"}) {
        const QJsonObject g = byKind.value(QLatin1String(kind)).toObject();
        std::printf("  %-7s n=%d P50=%.1f P95=%.1f max=%.1f ms\n", kind, g.value("count").toInt(),
                    g.value("p50Ms").toDouble(), g.value("p95Ms").toDouble(), g.value("maxMs").toDouble());
    }
    std::printf("候选数(首/末)=%d/%d score(首/末)=%.3f/%.3f\n", firstCandidates, lastCandidates, firstScore, lastScore);
    std::printf("有效样本=%d/%d 违约样本=%d（on-error=%s；违约不计入延迟统计，明细见 JSON violations）\n",
                int(latenciesMs.size()), runs, int(violations.size()), qPrintable(onErrorMode));
    std::printf("重试事件=%d（predict 期间 embeddingReady=invalid_embedding 自动重试）注入=%d\n", retryEvents,
                injectInvalidEmbedding);

    if (parser.isSet(optJson)) {
        QFile out(parser.value(optJson));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "sam_bench: JSON 输出不可写：%s\n", qPrintable(parser.value(optJson)));
            return 3;
        }
        out.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
        out.close();
        std::printf("JSON 已写入 %s\n", qPrintable(parser.value(optJson)));
    }
    return 0;
}
