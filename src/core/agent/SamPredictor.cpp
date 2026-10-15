#include "SamPredictor.h"

#include "SamBackendClient.h"
#include "core/common/CancellationToken.h"
#include "core/deeplux/DataContract.h"
#ifdef DEEPLUX_HAS_ONNXRUNTIME
#include "core/inference/SamOnnxInference.h"
#include "core/model/AnnotationMaskOps.h"

#include <opencv2/core.hpp>
#endif

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>

namespace DeepLux {

#ifdef DEEPLUX_HAS_ONNXRUNTIME
QThreadPool* samNativeThreadPool() {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread())
        return nullptr;
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(app);
        pool->setMaxThreadCount(1);
        pool->setExpiryTimeout(-1);
    }
    return pool;
}

namespace SamPredictorDetail {
SamPredictOutcome convertNativeCandidates(const SamNativeResult& prediction, const SamPromptItem& item,
                                          const QSize& imageSize, bool includeCandidates) {
    SamPredictOutcome result;
    auto fail = [&result](const QString& message) {
        result.error = message;
        result.candidateScores.clear();
        result.previewCandidates.clear();
        return result;
    };
    if (!prediction.ok || prediction.candidates.isEmpty())
        return fail(prediction.cancelled ? QStringLiteral("SAM 预测已取消")
                                         : QStringLiteral("原生 SAM 预测失败：%1").arg(prediction.error));
    if (item.candidateIndex < 0 || item.candidateIndex >= prediction.candidates.size())
        return fail(QStringLiteral("candidateIndex %1 越界（本次共 %2 个候选）")
                        .arg(item.candidateIndex)
                        .arg(prediction.candidates.size()));

    QList<SamCandidate> previews;
    Region2D selectedRegion;
    result.candidateScores.append(QList<double>{});
    for (int index = 0; index < prediction.candidates.size(); ++index) {
        const SamNativeCandidate& candidate = prediction.candidates[index];
        if (!std::isfinite(candidate.score))
            return fail(QStringLiteral("原生 SAM 返回非有限候选分数"));
        result.candidateScores[0].append(candidate.score);
        if (!includeCandidates && index != item.candidateIndex)
            continue;
        if (candidate.mask.empty() || candidate.mask.type() != CV_8UC1 || candidate.mask.cols != imageSize.width() ||
            candidate.mask.rows != imageSize.height()) {
            if (index == item.candidateIndex)
                return fail(QStringLiteral("原生 SAM 返回无效掩膜"));
            continue;
        }
        const QImage mask(candidate.mask.data, candidate.mask.cols, candidate.mask.rows, int(candidate.mask.step),
                          QImage::Format_Grayscale8);
        Region2D region;
        region.label = item.label;
        region.polygon = MaskOps::traceLargestContour(mask);
        region.bbox = MaskOps::foregroundBoundingBox(mask);
        region.maskRle = MaskOps::encodeRle(mask);
        region.score = std::clamp(candidate.score, 0.0, 1.0);
        if (!portValueMatchesType(QVariant::fromValue(region), DataType::Region2D)) {
            if (index == item.candidateIndex)
                return fail(QStringLiteral("原生 SAM 返回空掩膜或区域契约无效"));
            continue;
        }
        if (index == item.candidateIndex)
            selectedRegion = region;
        if (includeCandidates) {
            SamCandidate preview;
            preview.polygon = region.polygon;
            preview.bbox = region.bbox;
            preview.score = region.score;
            preview.maskRle = region.maskRle;
            preview.maskImage = MaskOps::toDisplayMask(mask);
            previews.append(preview);
        }
    }
    result.regions.append(selectedRegion);
    if (includeCandidates)
        result.previewCandidates.append(previews);
    result.modelName = prediction.modelName;
    result.ok = true;
    return result;
}
} // namespace SamPredictorDetail
#endif

bool parseSamPointsJson(const QJsonValue& value, QList<QPointF>& out, bool allowEmpty, QString& error) {
    error.clear();
    if (value.isUndefined() || value.isNull()) {
        if (allowEmpty)
            return true;
        error = QStringLiteral("必须为 [[x,y],...] 有限数值数组");
        return false;
    }
    if (!value.isArray()) {
        error = QStringLiteral("必须为 [[x,y],...] 有限数值数组");
        return false;
    }
    const QJsonArray arr = value.toArray();
    if (arr.isEmpty()) {
        if (allowEmpty)
            return true;
        error = QStringLiteral("不能为空数组");
        return false;
    }
    for (const QJsonValue& pv : arr) {
        if (!pv.isArray()) {
            error = QStringLiteral("元素必须为 [x,y] 数值对");
            return false;
        }
        const QJsonArray pt = pv.toArray();
        if (pt.size() != 2 || !pt[0].isDouble() || !pt[1].isDouble()) {
            error = QStringLiteral("元素必须为 [x,y] 数值对");
            return false;
        }
        const double x = pt[0].toDouble();
        const double y = pt[1].toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) {
            error = QStringLiteral("坐标必须为有限数值");
            return false;
        }
        out.append(QPointF(x, y));
    }
    return true;
}

bool parseSamBoxJson(const QJsonValue& value, QRectF& out, QString& error) {
    error.clear();
    out = QRectF();
    if (value.isUndefined() || value.isNull())
        return true; // 可空 = 未提供
    if (!value.isArray()) {
        error = QStringLiteral("必须为 [x,y,w,h]（w/h>0）或留空");
        return false;
    }
    const QJsonArray arr = value.toArray();
    if (arr.size() != 4) {
        error = QStringLiteral("必须为 [x,y,w,h]（w/h>0）或留空");
        return false;
    }
    double vals[4];
    for (int i = 0; i < 4; ++i) {
        if (!arr[i].isDouble() || !std::isfinite(arr[i].toDouble())) {
            error = QStringLiteral("必须为 4 个有限数值 [x,y,w,h]");
            return false;
        }
        vals[i] = arr[i].toDouble();
    }
    if (vals[2] <= 0.0 || vals[3] <= 0.0) {
        error = QStringLiteral("w/h 必须 > 0");
        return false;
    }
    out = QRectF(vals[0], vals[1], vals[2], vals[3]);
    return true;
}

SamPredictOutcome runSamPredictBatch(const QString& serverUrl, const QString& imagePath,
                                     const QList<SamPromptItem>& items, int timeoutMs, const CancellationToken* cancel,
                                     bool includeCandidates) {
    SamPredictOutcome outcome;
    auto fail = [&outcome](const QString& error, int index) {
        outcome.ok = false;
        outcome.error = error;
        outcome.failedIndex = index;
        outcome.regions.clear();
        outcome.candidateScores.clear();
        outcome.previewCandidates.clear();
        return outcome;
    };

    // 先行校验：参数/图像非法不发任何网络请求（失败关闭）
    if (serverUrl.trimmed().isEmpty())
        return fail(QStringLiteral("serverUrl 不能为空"), -1);
    if (timeoutMs < 100 || timeoutMs > 120000)
        return fail(QStringLiteral("timeoutMs 必须为[100,120000]"), -1);
    if (imagePath.isEmpty() || !QFileInfo::exists(imagePath))
        return fail(QStringLiteral("输入图像不存在：%1").arg(imagePath), -1);
    if (items.isEmpty())
        return fail(QStringLiteral("未提供任何 prompt"), -1);
    for (int i = 0; i < items.size(); ++i) {
        const SamPromptItem& item = items[i];
        if (item.positivePoints.isEmpty() && item.negativePoints.isEmpty() && !item.box.isValid())
            return fail(QStringLiteral("prompt[%1]：正点、负点与 box 至少提供一个").arg(i), i);
        // 解析层已保证有限性；直调结构体路径（插件/内部复用）再防一道
        const auto finitePoints = [](const QList<QPointF>& points) {
            for (const QPointF& p : points) {
                if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
                    return false;
            }
            return true;
        };
        if (!finitePoints(item.positivePoints) || !finitePoints(item.negativePoints))
            return fail(QStringLiteral("prompt[%1] 含非有限坐标").arg(i), i);
        if (item.box.isValid() && (!std::isfinite(item.box.x()) || !std::isfinite(item.box.y()) ||
                                   !std::isfinite(item.box.width()) || !std::isfinite(item.box.height())))
            return fail(QStringLiteral("prompt[%1] box 含非有限值").arg(i), i);
        if (item.candidateIndex < 0) // 批4（C）：越界上界在收到候选后校验
            return fail(QStringLiteral("prompt[%1] candidateIndex 必须 >= 0").arg(i), i);
    }

    if (serverUrl == QLatin1String("native")) {
#ifndef DEEPLUX_HAS_ONNXRUNTIME
        Q_UNUSED(includeCandidates);
        return fail(QStringLiteral("此构建未启用原生 SAM 推理"), -1);
#else
        if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()) {
            QThreadPool* pool = samNativeThreadPool();
            auto work = std::make_shared<std::packaged_task<SamPredictOutcome()>>(
                [serverUrl, imagePath, items, timeoutMs, cancel, includeCandidates] {
                    return runSamPredictBatch(serverUrl, imagePath, items, timeoutMs, cancel, includeCandidates);
                });
            auto future = work->get_future();
            pool->start([work] { (*work)(); });

            QEventLoop loop;
            QTimer poll;
            poll.setInterval(10);
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
                    loop.quit();
            });
            poll.start();
            loop.exec();
            future.wait(); // 应用退出使嵌套事件循环提前结束时，也不能留下悬空的取消令牌
            try {
                return future.get();
            } catch (const std::exception& e) {
                return fail(QStringLiteral("原生 SAM 工作线程失败：%1").arg(e.what()), -1);
            }
        }

        const QString packageDir = qEnvironmentVariable("DEEPLUX_SAM_PACKAGE");
        if (packageDir.isEmpty())
            return fail(QStringLiteral("未设置 DEEPLUX_SAM_PACKAGE 模型包路径"), -1);
        if (cancel && cancel->isCancelledFast())
            return fail(QStringLiteral("SAM 预测已取消"), -1);

        // shortcut: each worker thread owns one model session; consolidate only if session memory becomes a bottleneck.
        thread_local SamOnnxInference inference;
        QElapsedTimer timer;
        timer.start();
        if ((!inference.isLoaded() || inference.packageDir() != packageDir) && !inference.loadPackage(packageDir))
            return fail(QStringLiteral("原生 SAM 模型包加载失败：%1").arg(inference.lastError()), -1);
        if (timer.elapsed() > timeoutMs)
            return fail(QStringLiteral("原生 SAM 模型包加载超时"), -1);

        const QImage source(imagePath);
        if (source.isNull())
            return fail(QStringLiteral("无法解码输入图像：%1").arg(imagePath), -1);
        const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
        if (timer.elapsed() > timeoutMs)
            return fail(QStringLiteral("原生 SAM 图像读取超时"), -1);
        const cv::Mat image(rgb.height(), rgb.width(), CV_8UC3, const_cast<uchar*>(rgb.constBits()),
                            rgb.bytesPerLine());
        for (int i = 0; i < items.size(); ++i) {
            if (cancel && cancel->isCancelledFast())
                return fail(QStringLiteral("SAM 预测已取消"), i);
            timer.restart();
            const SamPromptItem& item = items[i];
            const SamNativeResult prediction =
                inference.predict(image, item.positivePoints, item.negativePoints, item.box);
            if (cancel && cancel->isCancelledFast())
                return fail(QStringLiteral("SAM 预测已取消"), i);
            if (timer.elapsed() > timeoutMs)
                return fail(QStringLiteral("原生 SAM 预测超时"), i);
            const SamPredictOutcome converted =
                SamPredictorDetail::convertNativeCandidates(prediction, item, rgb.size(), includeCandidates);
            if (!converted.ok)
                return fail(converted.error, i);
            if (cancel && cancel->isCancelledFast())
                return fail(QStringLiteral("SAM 预测已取消"), i);
            if (timer.elapsed() > timeoutMs)
                return fail(QStringLiteral("原生 SAM 结果转换超时"), i);
            outcome.regions.append(converted.regions.first());
            outcome.candidateScores.append(converted.candidateScores.first());
            if (includeCandidates)
                outcome.previewCandidates.append(converted.previewCandidates.first());
            outcome.modelName = converted.modelName;
        }
        outcome.ok = true;
        return outcome;
#endif
    }

    // 客户端栈上创建：NAM 线程亲和 = 调用线程，本地事件循环同步收口。
    // set_image 一次，embedding 复用逐项 predict。
    SamBackendClient client;
    client.setServerUrl(serverUrl);
    client.setTimeoutMs(timeoutMs);

    QString error;
    bool done = false;
    bool embeddingAcquired = false;
    int nextIndex = 0;
    QEventLoop loop;

    // 外层 deadline 仅为客户端定时器故障兜底（批1 复核二轮 P2-2 口径），
    // 每收到一次响应重置，使批量预测的每一步都受同一兜底保护
    const int deadlineMs = timeoutMs + 2000;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setInterval(deadlineMs);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("SAM 请求超时（本地等待上限 %1ms）").arg(deadlineMs);
        done = true;
        loop.quit();
    });
    // 取消轮询：调用方（引擎 stop / Agent 取消）令牌 100ms 内响应
    QTimer cancelPoll;
    cancelPoll.setInterval(100);
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
        if (cancel && cancel->isCancelledFast()) {
            error = QStringLiteral("SAM 预测已取消");
            done = true;
            loop.quit();
        }
    });

    QObject::connect(
        &client, &SamBackendClient::predictionReady, &loop,
        [&](const QList<QPointF>& polygon, const QRectF& bbox, double score, const QString& maskRle, const QImage&) {
            // 批4（C）：多掩膜候选选择——candidates 按 score 降序，index 0 与
            // 顶层 best 字段一致；越界或无候选却指定 index>0 均失败关闭
            const QList<SamCandidate> candidates = client.lastCandidates();
            QList<double> scores;
            for (const SamCandidate& candidate : candidates)
                scores.append(candidate.score);
            outcome.candidateScores.append(scores);
            const int wanted = items[nextIndex].candidateIndex;
            QList<QPointF> chosenPolygon = polygon;
            QRectF chosenBbox = bbox;
            double chosenScore = score;
            QString chosenRle = maskRle;
            if (!candidates.isEmpty()) {
                if (wanted < 0 || wanted >= candidates.size()) {
                    error =
                        QStringLiteral("candidateIndex %1 越界（本次共 %2 个候选）").arg(wanted).arg(candidates.size());
                    outcome.failedIndex = nextIndex;
                    done = true;
                    loop.quit();
                    return;
                }
                const SamCandidate& chosen = candidates[wanted];
                chosenPolygon = chosen.polygon;
                chosenBbox = chosen.bbox;
                chosenScore = chosen.score;
                chosenRle = chosen.maskRle;
            } else if (wanted > 0) {
                error = QStringLiteral("后端未返回多掩膜候选，无法选择 candidateIndex=%1").arg(wanted);
                outcome.failedIndex = nextIndex;
                done = true;
                loop.quit();
                return;
            }
            Region2D region;
            region.label = items[nextIndex].label;
            region.polygon = chosenPolygon;
            region.bbox = chosenBbox;
            region.maskRle = chosenRle;
            region.score = chosenScore;
            // 产物在返回前过 Region2D 端口契约门禁（isValid + 有限值 +
            // score∈[0,1]）：不合格按后端错误失败关闭，不下发可疑区域
            if (!portValueMatchesType(QVariant::fromValue(region), DataType::Region2D)) {
                error = QStringLiteral("SAM 返回区域无效或不符合 Region2D 契约");
                outcome.failedIndex = nextIndex;
                done = true;
                loop.quit();
                return;
            }
            outcome.regions.append(region);
            ++nextIndex;
            deadline.start(); // 每收到一次响应重置兜底
            if (nextIndex < items.size()) {
                // 不可在 predictionReady 回调内重入 predict()：客户端在信号
                // 返回后才对当前回复 settleReply，重入发起的新请求会被置空并
                // 丢失超时保护（下一项响应按"过期回复"丢弃 → 挂死到兜底）。
                // 延迟到下一轮事件循环发起，settle 完成后时序干净。
                const int next = nextIndex;
                QTimer::singleShot(0, &client, [&client, &deadline, items, next]() {
                    deadline.start();
                    client.predict(items[next].positivePoints, items[next].negativePoints, items[next].box);
                });
            } else {
                outcome.modelName = client.modelName();
                done = true;
                loop.quit();
            }
        });
    QObject::connect(&client, &SamBackendClient::errorOccurred, &loop, [&](const QString& msg) {
        if (!done) {
            error = msg;
            outcome.failedIndex = embeddingAcquired ? qMin(nextIndex, items.size() - 1) : -1;
            done = true;
            loop.quit();
        }
    });
    QObject::connect(&client, &SamBackendClient::embeddingReady, &loop, [&](const QString&) {
        embeddingAcquired = true;
        deadline.start();
        client.predict(items[0].positivePoints, items[0].negativePoints, items[0].box);
    });

    deadline.start();
    cancelPoll.start();
    client.setImage(imagePath);
    loop.exec();
    deadline.stop();
    cancelPoll.stop();

    if (!done || !error.isEmpty()) {
        outcome.ok = false;
        outcome.error = error.isEmpty() ? QStringLiteral("SAM 预测失败") : error;
        outcome.regions.clear(); // 全有或全无：不返回部分结果
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

} // namespace DeepLux
