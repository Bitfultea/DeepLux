#include "SamPredictPlugin.h"

#include "common/Logger.h"
#include "core/agent/SamBackendClient.h"
#include "core/deeplux/DataContract.h"

#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QTemporaryFile>
#include <QTimer>
#include <cmath>

namespace DeepLux {

namespace {
bool parsePointList(const QJsonValue& v, QList<QPointF>& out, bool allowEmpty) {
    if (v.isUndefined() || v.isNull())
        return allowEmpty;
    if (!v.isArray())
        return false;
    const QJsonArray arr = v.toArray();
    if (arr.isEmpty())
        return allowEmpty;
    for (const QJsonValue& pv : arr) {
        if (!pv.isArray())
            return false;
        const QJsonArray pt = pv.toArray();
        if (pt.size() != 2 || !pt[0].isDouble() || !pt[1].isDouble())
            return false;
        const double x = pt[0].toDouble();
        const double y = pt[1].toDouble();
        if (!std::isfinite(x) || !std::isfinite(y))
            return false;
        out.append(QPointF(x, y));
    }
    return true;
}

bool parseBox(const QJsonValue& v, QRectF& out) {
    if (v.isUndefined() || v.isNull())
        return true; // 可空
    if (!v.isArray())
        return false;
    const QJsonArray arr = v.toArray();
    if (arr.size() != 4)
        return false;
    double vals[4];
    for (int i = 0; i < 4; ++i) {
        if (!arr[i].isDouble() || !std::isfinite(arr[i].toDouble()))
            return false;
        vals[i] = arr[i].toDouble();
    }
    if (vals[2] <= 0.0 || vals[3] <= 0.0)
        return false;
    out = QRectF(vals[0], vals[1], vals[2], vals[3]);
    return true;
}
} // namespace

SamPredictPlugin::SamPredictPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"serverUrl", QStringLiteral("http://127.0.0.1:8990")},
                                  {"label", QStringLiteral("object")},
                                  {"points", QJsonArray{}},
                                  {"negativePoints", QJsonArray{}},
                                  {"box", QJsonValue::Null},
                                  {"timeoutMs", 30000}};
    m_params = m_defaultParams;
}

SamPredictPlugin::~SamPredictPlugin() {}

bool SamPredictPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SamPredictPlugin initialized";
    return true;
}

void SamPredictPlugin::shutdown() {
    ModuleBase::shutdown();
}

bool SamPredictPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    const QJsonValue url = params[QLatin1String("serverUrl")];
    if (!url.isString() || url.toString().trimmed().isEmpty()) {
        error = tr("serverUrl 必须为非空字符串");
        return false;
    }
    if (!params[QLatin1String("label")].isString()) {
        error = tr("label 必须为字符串");
        return false;
    }
    QList<QPointF> pos;
    QList<QPointF> neg;
    QRectF box;
    if (!parsePointList(params[QLatin1String("points")], pos, true)) {
        error = tr("points 必须为 [[x,y],...] 有限数值数组");
        return false;
    }
    if (!parsePointList(params[QLatin1String("negativePoints")], neg, true)) {
        error = tr("negativePoints 必须为 [[x,y],...] 有限数值数组");
        return false;
    }
    if (!parseBox(params[QLatin1String("box")], box)) {
        error = tr("box 必须为 [x,y,w,h]（w/h>0）或留空");
        return false;
    }
    // 注意：prompt 存在性不在参数校验层要求——默认参数（空 prompt）必须是合法
    // 可配置状态（参数契约测试1），缺失 prompt 在 process() 运行期失败关闭。
    const QJsonValue t = params[QLatin1String("timeoutMs")];
    if (!t.isDouble() || !std::isfinite(t.toDouble()) || t.toDouble() < 100.0 || t.toDouble() > 120000.0) {
        error = tr("timeoutMs 必须为[100,120000]有限数");
        return false;
    }
    return true;
}

bool SamPredictPlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const QString serverUrl = params["serverUrl"].toString();
    const QString label = params["label"].toString();
    const int timeoutMs = params["timeoutMs"].toInt();
    QList<QPointF> pos;
    QList<QPointF> neg;
    QRectF box;
    parsePointList(params["points"], pos, true);
    parsePointList(params["negativePoints"], neg, true);
    parseBox(params["box"], box);

    if (pos.isEmpty() && !box.isValid()) {
        emit errorOccurred(tr("points 与 box 至少提供一个 prompt"));
        return false;
    }

    const QImage image = input.toQImage();
    if (image.isNull()) {
        emit errorOccurred(tr("输入图像为空"));
        return false;
    }
    QTemporaryFile tmp;
    tmp.setFileTemplate(QDir::tempPath() + QStringLiteral("/deeplux_sampredict_XXXXXX.png"));
    if (!tmp.open() || !image.save(tmp.fileName(), "PNG")) {
        emit errorOccurred(tr("输入图像写入临时文件失败"));
        return false;
    }

    // 客户端栈上创建：NAM 线程亲和 = 调用线程，本地事件循环同步收口；
    // 超时或后端错误失败关闭，不伪造区域
    SamBackendClient client;
    client.setServerUrl(serverUrl);
    client.setTimeoutMs(timeoutMs);

    Region2D region;
    QString error;
    bool done = false;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setInterval(timeoutMs * 2 + 2000);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
        error = QStringLiteral("SAM 请求超时（本地等待上限 %1ms）").arg(timeoutMs * 2 + 2000);
        done = true;
        loop.quit();
    });
    QObject::connect(
        &client, &SamBackendClient::predictionReady, &loop,
        [&](const QList<QPointF>& polygon, const QRectF& bbox, double score, const QString& maskRle, const QImage&) {
            region.label = label;
            region.polygon = polygon;
            region.bbox = bbox;
            region.maskRle = maskRle;
            region.score = score;
            done = true;
            loop.quit();
        });
    QObject::connect(&client, &SamBackendClient::errorOccurred, &loop, [&](const QString& msg) {
        if (!done) {
            error = msg;
            done = true;
            loop.quit();
        }
    });
    QObject::connect(&client, &SamBackendClient::embeddingReady, &loop,
                     [&](const QString&) { client.predict(pos, neg, box); });
    deadline.start();
    client.setImage(tmp.fileName());
    loop.exec();
    deadline.stop();

    if (!done || !error.isEmpty()) {
        emit errorOccurred(error.isEmpty() ? tr("SAM 预测失败") : error);
        return false;
    }
    if (!region.isValid()) {
        emit errorOccurred(tr("SAM 返回区域无效（polygon/bbox 均无效）"));
        return false;
    }
    output.setData("region", QVariant::fromValue(region));
    output.setData("score", region.score);
    Logger::instance().debug(QString("SAM 分割: label=%1 score=%2 polygon=%3")
                                 .arg(region.label)
                                 .arg(region.score, 0, 'f', 3)
                                 .arg(region.polygon.size()),
                             "SamPredict");
    return true;
}

IModule* SamPredictPlugin::cloneImpl() const {
    SamPredictPlugin* clone = new SamPredictPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
