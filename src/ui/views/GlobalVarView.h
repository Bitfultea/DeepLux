#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>

namespace DeepLux {

class GlobalVarView : public QDialog
{
    Q_OBJECT

public:
    explicit GlobalVarView(QWidget* parent = nullptr);
    ~GlobalVarView() override = default;

private slots:
    void onAddClicked();
    void onDeleteClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onExecuteClicked();
    void onApplyClicked();
    void onCloseClicked();
    void onSelectionChanged();
    void onAddVarTypeChanged(const QString& type);

private:
    void setupUI();
    void loadVariables();
    void updateButtons();

    QTableWidget* m_varTable = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_deleteBtn = nullptr;
    QPushButton* m_moveUpBtn = nullptr;
    QPushButton* m_moveDownBtn = nullptr;
    QPushButton* m_executeBtn = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;

    QComboBox* m_addTypeCombo = nullptr;
    QLineEdit* m_valueEdit = nullptr;
    QLineEdit* m_noteEdit = nullptr;
};

} // namespace DeepLux