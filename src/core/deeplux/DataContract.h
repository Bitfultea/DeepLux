#pragma once

#include "core/deeplux/ControlFlowType.h"
#include "core/deeplux/PayloadTypes.h"

#include <QHash>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVariant>
#include <QVector>

namespace DeepLux {

class ImageData;
class CancellationToken;

/**
 * @brief 强类型数据端口类型（ABI v2）
 *
 * 所有跨模块传递的数据都必须以 DataType 声明。当前 2D/3D 几何数据以数值 QVariantList 作为
 * 兼容载荷；复杂领域对象在对应生产插件落地时升级为注册元类型。
 */
enum class DataType {
    Image2D,      // 载荷: ImageData
    HeightMap2D,  // 载荷: ImageData（深度/高度语义）
    PointCloud3D, // 载荷: ImageData(携带 PointCloudData) 或 PointCloudData
    Mask2D,
    Region2D,
    Point2D,
    Point3D,
    PointSet2D,
    Line2D,
    Circle2D,
    Ellipse2D,
    Plane3D,
    Transform2D,
    DetectionList,
    ClassScores,
    Number,
    Integer,
    Boolean,
    String,
    Binary,
    Table,
    Any
};

/**
 * @brief 控制汇合策略（多入控制端口的激活语义）
 */
enum class ControlJoinPolicy {
    Any, // 任一上游控制边触发即激活（默认）
    All  // 全部上游控制边触发后才激活
};

/**
 * @brief 端口描述（由 metadata.json 的 ports.inputs/outputs 声明）
 */
struct PortSpec {
    QString id;          // 端口 ID（模块内唯一）
    QString displayName; // 显示名称（不可为空）
    DataType type = DataType::Any;
    bool required = false; // 仅输入有效：必需输入缺失时返回结构化错误
    bool multiple = false; // 是否可连接多个上游
    bool control = false;  // 阶段 3.2: true 表示控制端口（与数据端口分离）
    ControlJoinPolicy joinPolicy = ControlJoinPolicy::Any; // multiple+control 时的汇合策略
};

/// 端口值映射：portId -> QVariant（值为 DataType 对应的注册类型）
using PortValueMap = QHash<QString, QVariant>;

/**
 * @brief 执行上下文：每次执行携带的运行期信息
 */
struct ExecutionContext {
    QString runId;          // 一次完整运行的 ID
    qint64 frameId = 0;     // 帧序号（循环/在线流程递增）
    qint64 timestampMs = 0; // 执行开始时间戳
    ControlFlowType runMode = ControlFlowType::Sequential;
    CancellationToken* cancellationToken = nullptr;

    bool isCancelled() const;
};

/**
 * @brief 执行结果（ABI v2）
 */
struct ExecutionResult {
    bool success = false;
    int errorCode = 0;   // 0 表示成功；非 0 为结构化错误码
    QString userMessage; // 用户可读错误
    QString diagnostics; // 诊断信息（日志/堆栈/端口名等）

    static ExecutionResult ok() {
        ExecutionResult r;
        r.success = true;
        return r;
    }
    static ExecutionResult fail(int code, const QString& user, const QString& diag = QString()) {
        ExecutionResult r;
        r.success = false;
        r.errorCode = code;
        r.userMessage = user;
        r.diagnostics = diag;
        return r;
    }
};

/// 结构化错误码（跨插件稳定）
namespace ExecError {
inline constexpr int None = 0;
inline constexpr int MissingRequiredInput = 1001; // 必需输入缺失
inline constexpr int TypeMismatch = 1002;         // 输入类型与端口声明不符
inline constexpr int UndeclaredOutput = 1003;     // 写出未声明端口
inline constexpr int Processing = 1004;           // 算法/运行期错误
inline constexpr int Cancelled = 1005;            // 被取消
} // namespace ExecError

/**
 * @brief 将 DataType 转为可读字符串（用于诊断与日志）
 */
QString dataTypeName(DataType type);

/**
 * @brief 解析 metadata.json 中的类型字符串为 DataType；未知返回 false
 */
bool dataTypeFromString(const QString& name, DataType& out);

/// 验证端口值是否符合声明类型。Any 始终接受有效 QVariant。
bool portValueMatchesType(const QVariant& value, DataType type);

/**
 * @brief 注册核心几何/数据元类型（幂等，进程内仅需一次）
 *
 * 在 QApplication 创建后、加载插件前调用。保证 QVariant 能跨 .so 传递注册结构体。
 */
void registerDataContractMetaTypes();

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::DataType)
Q_DECLARE_METATYPE(DeepLux::PortSpec)
Q_DECLARE_METATYPE(DeepLux::ControlJoinPolicy)
Q_DECLARE_METATYPE(QVector<QPointF>)
