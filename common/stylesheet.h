#ifndef STYLESHEET_H
#define STYLESHEET_H

#include <QWidget>

namespace RetailStyle {

inline QString commonWidgets()
{
    return QString()
        + "QPushButton {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #3498db, stop:1 #2980b9);"
        "    color: white; border: none; border-radius: 6px;"
        "    padding: 8px 16px; font-weight: bold; min-width: 80px;"
        "}"
        "QPushButton:hover {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2980b9, stop:1 #1f618d);"
        "}"
        "QPushButton:pressed { background: #1f618d; }"
        "QPushButton:disabled { background: #bdc3c7; color: #7f8c8d; }"
        "QLineEdit, QDoubleSpinBox {"
        "    border: 2px solid #bdc3c7; border-radius: 6px; padding: 6px;"
        "    background: white; color: #2c3e50;"
        "    selection-background-color: #3498db;"
        "}"
        "QLineEdit:focus, QDoubleSpinBox:focus { border-color: #3498db; }"
        "QLabel { color: #2c3e50; font-size: 14px; }"
        "QGroupBox {"
        "    border: 2px solid #3498db; border-radius: 8px;"
        "    margin-top: 10px; padding-top: 10px;"
        "    font-weight: bold; color: #2c3e50;"
        "}"
        "QGroupBox::title {"
        "    subcontrol-origin: margin; left: 15px;"
        "    padding: 0 10px; color: #3498db;"
        "}"
        "QComboBox {"
        "    border: 2px solid #bdc3c7; border-radius: 6px; padding: 6px;"
        "    background: white; color: #2c3e50; min-width: 100px;"
        "}"
        "QComboBox:focus { border-color: #3498db; }"
        "QComboBox::drop-down { border: none; width: 30px; }"
        "QComboBox QAbstractItemView {"
        "    background: white; color: #2c3e50;"
        "    selection-background-color: #3498db; selection-color: white;"
        "}"
        "QTableWidget {"
        "    border: 2px solid #bdc3c7; border-radius: 5px;"
        "    background: white; font-size: 15px; gridline-color: #ecf0f1;"
        "}"
        "QHeaderView::section {"
        "    background: #3498db; color: white; font-weight: bold;"
        "    font-size: 14px; padding: 6px 8px; border: 1px solid #2980b9;"
        "}";
}

inline void apply(QWidget *w)
{
    w->setStyleSheet(commonWidgets());
}

} // namespace RetailStyle

#endif
