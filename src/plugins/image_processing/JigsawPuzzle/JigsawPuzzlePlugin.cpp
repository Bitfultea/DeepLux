#include "JigsawPuzzlePlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

JigsawPuzzlePlugin::JigsawPuzzlePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"rows", 2},
        {"cols", 2},
        {"matchThreshold", 0.3}
    };
    m_params = m_defaultParams;
}

JigsawPuzzlePlugin::~JigsawPuzzlePlugin()
{
}

bool JigsawPuzzlePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "JigsawPuzzlePlugin initialized";
    return true;
}

void JigsawPuzzlePlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_resultMat.release();
#endif
    ModuleBase::shutdown();
}

bool JigsawPuzzlePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

#ifdef DEEPLUX_HAS_OPENCV
    // 获取图像
    cv::Mat mat;
    if (input.hasMat()) {
        mat = input.toMat();
    } else {
        mat = qImageToMat(input.toQImage());
    }

    if (mat.empty()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }

    m_rows = m_params["rows"].toInt();
    m_cols = m_params["cols"].toInt();

    if (m_rows < 1 || m_cols < 1) {
        emit errorOccurred(tr("行数和列数必须大于0"));
        return false;
    }

    // 如果图像数量只有一个，进行简单分割
    // 注意: 本插件主要处理单个图像的分割，碎片数据通过其他方式获取
    int pieceCount = input.data("puzzle_pieces_count").toInt();

    std::vector<cv::Mat> pieces;

    // 如果有碎片数量信息，尝试从输入数据中获取碎片（目前简化处理）
    // 实际应用中，碎片数据应该通过其他插件提取后传入
    if (pieceCount > 0 && input.hasMat()) {
        // 暂时忽略碎片数据，使用输入图像进行分割
    }

    if (pieces.empty()) {
        // 将输入图像分割成碎片
        int pieceWidth = mat.cols / m_cols;
        int pieceHeight = mat.rows / m_rows;

        for (int i = 0; i < m_rows; ++i) {
            for (int j = 0; j < m_cols; ++j) {
                cv::Rect roi(j * pieceWidth, i * pieceHeight, pieceWidth, pieceHeight);
                pieces.push_back(mat(roi).clone());
            }
        }
    }

    // 简单拼接 - 按顺序排列碎片
    int pieceWidth = pieces[0].cols;
    int pieceHeight = pieces[0].rows;
    cv::Mat result(m_rows * pieceHeight, m_cols * pieceWidth, pieces[0].type());

    for (size_t i = 0; i < pieces.size() && i < static_cast<size_t>(m_rows * m_cols); ++i) {
        int row = i / m_cols;
        int col = i % m_cols;
        cv::Rect roi(col * pieceWidth, row * pieceHeight, pieceWidth, pieceHeight);
        pieces[i].copyTo(result(roi));
    }

    m_resultMat = result;
    output.setMat(m_resultMat);
    output.setData("puzzle_rows", m_rows);
    output.setData("puzzle_cols", m_cols);
    output.setData("puzzle_piece_count", static_cast<int>(pieces.size()));

    Logger::instance().debug(QString("拼图求解: %1x%2, %3个碎片")
                                .arg(m_rows).arg(m_cols).arg(pieces.size()), "JigsawPuzzle");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool JigsawPuzzlePlugin::solvePuzzle(const std::vector<cv::Mat>& pieces, cv::Mat& result, int rows, int cols)
{
    // 简化的拼图求解 - 按相似度匹配
    if (pieces.empty()) return false;

    int pieceWidth = pieces[0].cols;
    int pieceHeight = pieces[0].rows;
    result.create(rows * pieceHeight, cols * pieceWidth, pieces[0].type());

    // 简单的顺序拼接
    for (size_t i = 0; i < pieces.size() && i < static_cast<size_t>(rows * cols); ++i) {
        int row = i / cols;
        int col = i % cols;
        cv::Rect roi(col * pieceWidth, row * pieceHeight, pieceWidth, pieceHeight);
        pieces[i].copyTo(result(roi));
    }

    return true;
}

bool JigsawPuzzlePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    int rows = params["rows"].toInt();
    int cols = params["cols"].toInt();

    if (rows < 1 || cols < 1) {
        error = tr("行数和列数必须大于0");
        return false;
    }

    return true;
}

QWidget* JigsawPuzzlePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("行数:")));
    QSpinBox* rowsSpin = new QSpinBox();
    rowsSpin->setRange(1, 10);
    rowsSpin->setValue(m_params["rows"].toInt());
    layout->addWidget(rowsSpin);

    layout->addWidget(new QLabel(tr("列数:")));
    QSpinBox* colsSpin = new QSpinBox();
    colsSpin->setRange(1, 10);
    colsSpin->setValue(m_params["cols"].toInt());
    layout->addWidget(colsSpin);

    layout->addStretch();

    connect(rowsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["rows"] = value;
    });

    connect(colsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["cols"] = value;
    });

    return widget;
}

IModule* JigsawPuzzlePlugin::cloneImpl() const
{
    JigsawPuzzlePlugin* clone = new JigsawPuzzlePlugin();
    return clone;
}

} // namespace DeepLux
