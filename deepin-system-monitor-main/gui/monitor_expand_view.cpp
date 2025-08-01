// Copyright (C) 2019 ~ 2020 Uniontech Software Technology Co.,Ltd
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "monitor_expand_view.h"

#include "cpu_monitor.h"
#include "memory_monitor.h"
#include "network_monitor.h"
#include "detailwidgetmanager.h"
#include "ddlog.h"

#include <DPalette>
#include <DStyle>

#include <QVBoxLayout>

using namespace DDLog;

// constructor
MonitorExpandView::MonitorExpandView(QWidget *parent)
    : DFrame(parent)
{
    qCDebug(app) << "MonitorExpandView constructor";
    // disable auto fill frame background
    setAutoFillBackground(false);
    // set background role
    setBackgroundRole(DPalette::Window);
    // frameless style
    setFrameStyle(DFrame::NoFrame);

    // cpu monitor view
    m_cpuMonitor = new CpuMonitor(this);
    // memory monitor view
    m_memoryMonitor = new MemoryMonitor(this);
    // network monitor view
    m_networkMonitor = new NetworkMonitor(this);

    // monitor view layout
    QVBoxLayout *layout = new QVBoxLayout(this);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    layout->setMargin(0);
#else
    layout->setContentsMargins(0, 0, 0, 0);
#endif
    layout->setSpacing(0);
    layout->addStretch(1);
    layout->addWidget(m_cpuMonitor, 0, Qt::AlignHCenter);
    layout->addStretch(2);
    layout->addWidget(m_memoryMonitor, 0, Qt::AlignHCenter);
    layout->addStretch(2);
    layout->addWidget(m_networkMonitor, 0, Qt::AlignHCenter);
    layout->addStretch(1);
    // set monitor view layout
    setLayout(layout);

    connect(m_cpuMonitor, &CpuMonitor::signalDetailInfoClicked, this, &MonitorExpandView::signalDetailInfoClicked);
    // 连接dbus发来的信号
    connect(&DetailWidgetManager::getInstance(), &DetailWidgetManager::sigJumpToDetailWidget, this, [=](QString msgCode) {
        qCDebug(app) << "Received signal from DBus to show detail info:" << msgCode;
        emit signalDetailInfoByDbus(msgCode);
    });

    //点击左侧区域触发跳转
    connect(m_cpuMonitor, &CpuMonitor::clicked, &DetailWidgetManager::getInstance(), &DetailWidgetManager::jumpDetailWidget);
    connect(m_memoryMonitor, &MemoryMonitor::clicked, &DetailWidgetManager::getInstance(), &DetailWidgetManager::jumpDetailWidget);
    connect(m_networkMonitor, &NetworkMonitor::clicked, &DetailWidgetManager::getInstance(), &DetailWidgetManager::jumpDetailWidget);
}

void MonitorExpandView::setDetailButtonVisible(bool visible)
{
    qCDebug(app) << "Setting detail button visibility to:" << visible;
    m_cpuMonitor->setDetailButtonVisible(visible);
}
