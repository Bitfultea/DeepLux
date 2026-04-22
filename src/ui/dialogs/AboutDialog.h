#pragma once

#include <QDialog>
#include <QLabel>

namespace DeepLux {

class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
    ~AboutDialog() override = default;

private:
    void setupUI();

    QLabel* m_logoLabel = nullptr;
    QLabel* m_nameLabel = nullptr;
    QLabel* m_versionLabel = nullptr;
    QLabel* m_descriptionLabel = nullptr;
};

} // namespace DeepLux