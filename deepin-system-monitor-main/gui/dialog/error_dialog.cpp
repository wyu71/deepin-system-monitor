// Copyright (C) 2019 ~ 2020 Uniontech Software Technology Co.,Ltd
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "error_dialog.h"

#include "gui/ui_common.h"
#include "ddlog.h"

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <DApplicationHelper>
#else
#include <DGuiApplicationHelper>
#include <DPaletteHelper>
#endif
#include <DFontSizeManager>

#include <QApplication>
#include <QMap>
#include <QDebug>

using namespace DDLog;

// Show error dialog
void ErrorDialog::show(QWidget *parent, const QString &errMsg, const QString &detail)
{
    qCDebug(app) << "Showing ErrorDialog with message:" << errMsg << "detail:" << detail;
    auto *dlg = new ErrorDialog(errMsg, detail, parent);
    dlg->exec();
}

// Initialize UI elements
void ErrorDialog::initUI()
{
    qCDebug(app) << "Initializing ErrorDialog UI";
    // set dialog icon
    setIcon(QIcon::fromTheme("dialog-warning"));
    // set dialog attribute
    setAttribute(Qt::WA_DeleteOnClose);
    // set maximum width to avoid displaying extra wide dialog
    setMaximumWidth(720);

    m_detailLabel = new DLabel(this);
    // global palette of current theme
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    auto pa = DApplicationHelper::instance()->palette(m_detailLabel);
#else
    auto pa = DPaletteHelper::instance()->palette(m_detailLabel);
#endif
    // get TextTips color from palette
    auto color = pa.color(DPalette::TextTips);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    pa.setColor(DPalette::Foreground, color);
#else
    pa.setColor(DPalette::Text, color);
#endif
    // set palette for error detail label
    m_detailLabel->setPalette(pa);
    // bind font size
    DFontSizeManager::instance()->bind(m_detailLabel, DFontSizeManager::T6, QFont::Medium);
    // word wrap on extra long description text
    m_detailLabel->setWordWrap(true);

    // set error message and error detail text
    setMessage(m_errMsg);
    m_detailLabel->setText(m_detail);
    m_detailLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    addSpacing(5);

    addContent(m_detailLabel, Qt::AlignHCenter);
    addButton(QApplication::translate("Error.Dialog", "OK", "button"), true);
}

// Private constructor
ErrorDialog::ErrorDialog(const QString &errMsg, const QString &detail, QWidget *parent)
    : DDialog(parent), m_errMsg(errMsg), m_detail(detail)
{
    qCDebug(app) << "ErrorDialog constructor";
    initUI();
}
