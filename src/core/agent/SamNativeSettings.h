#pragma once

#include <QString>

namespace DeepLux {

struct SamNativeConfig {
    QString packageDir;
    QString device = QStringLiteral("auto");
    QString error;
    bool packageFromEnvironment = false;
    bool deviceFromEnvironment = false;
};

class SamNativeSettings {
public:
    static void initialize();
    static SamNativeConfig snapshot();
    static bool saveUserSettings(const QString& packageDir, const QString& device, QString* error = nullptr);
};

} // namespace DeepLux
