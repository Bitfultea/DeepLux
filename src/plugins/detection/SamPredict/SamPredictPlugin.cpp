#include "SamPredictPlugin.h"

#include "common/Logger.h"
#include "core/agent/SamPredictor.h"
#include "core/deeplux/DataContract.h"

#include <QDir>
#include <QJsonArray>
#include <QTemporaryFile>
#include <cmath>

namespace DeepLux {

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
    // SAM 批3：prompt 解析与 Agent sam_predict 工具共享 SamPredictor 严格口径
    QList<QPointF> pos;
    QList<QPointF> neg;
    QRectF box;
    QString perr;
    if (!parseSamPointsJson(params[QLatin1String("points")], pos, true, perr)) {
        error = tr("points %1").arg(perr);
        return false;
    }
    if (!parseSamPointsJson(params[QLatin1String("negativePoints")], neg, true, perr)) {
        error = tr("negativePoints %1").arg(perr);
        return false;
    }
    if (!parseSamBoxJson(params[QLatin1String("box")], box, perr)) {
        error = tr("box %1").arg(perr);
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
    SamPromptItem item;
    item.label = params["label"].toString();
    parseSamPointsJson(params["points"], item.positivePoints, true, perr);
    parseSamPointsJson(params["negativePoints"], item.negativePoints, true, perr);
    parseSamBoxJson(params["box"], item.box, perr);

    if (item.positivePoints.isEmpty() && !item.box.isValid()) {
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

    // SAM 批3：同步预测核心（栈上客户端 + 本地事件循环 + 取消轮询 + Region2D
    // 契约门禁）收口到 SamPredictor，与 Agent sam_predict/sam_annotate 共享；
    // 超时、后端错误或引擎取消（stop）失败关闭，不伪造区域
    const SamPredictOutcome outcome = runSamPredictBatch(params["serverUrl"].toString(), tmp.fileName(), {item},
                                                         params["timeoutMs"].toInt(), cancellationToken());
    if (!outcome.ok) {
        emit errorOccurred(outcome.error);
        return false;
    }
    const Region2D region = outcome.regions.first();
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
