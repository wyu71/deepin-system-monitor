// Copyright (C) 2019 ~ 2021 Uniontech Software Technology Co.,Ltd.
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mem_stat_view_widget.h"
#include "chart_view_widget.h"
#include "common/common.h"
#include "system/device_db.h"
#include "system/mem.h"
#include "ddlog.h"

#include <QPainter>
#include <QtMath>

#include <DApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <DApplicationHelper>
#else
#include <DGuiApplicationHelper>
#endif

using namespace DDLog;
using namespace common::format;
using namespace core::system;

MemStatViewWidget::MemStatViewWidget(QWidget *parent) : QWidget(parent)
{
    qCDebug(app) << "MemStatViewWidget constructor";
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_memChartWidget = new ChartViewWidget(ChartViewWidget::ChartViewTypes::MEM_CHART, this);
    m_swapChartWidget = new ChartViewWidget(ChartViewWidget::ChartViewTypes::MEM_CHART, this);

    m_memChartWidget->setData1Color(memoryColor);
    m_swapChartWidget->setData1Color(swapColor);

    m_memInfo = DeviceDB::instance()->memInfo();
}

void MemStatViewWidget::fontChanged(const QFont &font)
{
    qCDebug(app) << "MemStatViewWidget fontChanged";
    m_font = font;
}

void MemStatViewWidget::onModelUpdate()
{
    qCDebug(app) << "MemStatViewWidget onModelUpdate";
    // After memory size text, add a space before the brackets
    QString memoryDetail = QString("%1 (%2)")
                           .arg(tr("Size"))
                           .arg(formatUnit_memory_disk(m_memInfo->memTotal() << 10, B, 1));
    parent()->setProperty("detail", memoryDetail);

    m_memChartWidget->addData1((m_memInfo->memTotal() - m_memInfo->memAvailable()) * 1.0 / m_memInfo->memTotal());
    m_swapChartWidget->addData1((m_memInfo->swapTotal() - m_memInfo->swapFree()) * 1.0 / m_memInfo->swapTotal());
    updateWidgetGeometry();
}

void MemStatViewWidget::updateWidgetGeometry()
{
    qCDebug(app) << "MemStatViewWidget updateWidgetGeometry";
    int avgWidth = this->width();
    if (m_memInfo->swapTotal() > 0) {
        qCDebug(app) << "Swap detected, adjusting layout for two charts";
        avgWidth = this->width() / 2 - 10;
        m_swapChartWidget->setVisible(true);
    } else {
        qCDebug(app) << "No swap detected, using layout for one chart";
        m_swapChartWidget->setVisible(false);
    }

    int fontHeight = QFontMetrics(m_font).height();
    m_memChartWidget->setGeometry(0, fontHeight / 2, avgWidth, this->height() - fontHeight / 2);
    m_swapChartWidget->setGeometry(m_memChartWidget->width() + 20, m_memChartWidget->y(), avgWidth, this->height() - fontHeight / 2);
}

void MemStatViewWidget::resizeEvent(QResizeEvent *event)
{
    // qCDebug(app) << "MemStatViewWidget resizeEvent";
    QWidget::resizeEvent(event);
    updateWidgetGeometry();
}

void MemStatViewWidget::paintEvent(QPaintEvent *event)
{
    // qCDebug(app) << "MemStatViewWidget paintEvent";
    QWidget::paintEvent(event);
    QPainter painter(this);
    QFont font = DApplication::font();
    font.setPointSizeF(font.pointSizeF() - 1);
    painter.setFont(font);
    painter.setRenderHint(QPainter::Antialiasing, true);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    auto *dAppHelper = DApplicationHelper::instance();
#else
    auto *dAppHelper = DGuiApplicationHelper::instance();
#endif
    auto palette = dAppHelper->applicationPalette();
    painter.setPen(palette.color(DPalette::TextTips));

    int spacing = 10;
    int sectionSize = 6;

    QString memory = DApplication::translate("Process.Graph.Title", "Memory");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QRect memtitleRect(sectionSize + spacing, 0, painter.fontMetrics().width(memory), painter.fontMetrics().height());
#else
    QRect memtitleRect(sectionSize + spacing, 0, painter.fontMetrics().horizontalAdvance(memory), painter.fontMetrics().height());
#endif
    painter.drawText(memtitleRect, Qt::AlignLeft | Qt::AlignVCenter, memory);

    QString swap = DApplication::translate("Process.Graph.View", "Swap");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QRect swaptitleRect(sectionSize + memtitleRect.right() + 2 * spacing, memtitleRect.y(), painter.fontMetrics().width(swap), painter.fontMetrics().height());
#else
    QRect swaptitleRect(sectionSize + memtitleRect.right() + 2 * spacing, memtitleRect.y(), painter.fontMetrics().horizontalAdvance(swap), painter.fontMetrics().height());
#endif
    if (m_memInfo->swapTotal() > 0)
    {
        // qCDebug(app) << "Swap detected, drawing swap title";
        painter.drawText(swaptitleRect, Qt::AlignLeft | Qt::AlignVCenter, swap);
        painter.setPen(Qt::NoPen);
        painter.setBrush(swapColor);
        painter.drawEllipse(memtitleRect.right() + spacing, swaptitleRect.y() + qCeil((swaptitleRect.height() - sectionSize) / 2.0), sectionSize, sectionSize);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(memoryColor);
    painter.drawEllipse(0, memtitleRect.y() + qCeil((memtitleRect.height() - sectionSize) / 2.0), sectionSize, sectionSize);
}
