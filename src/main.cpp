/**
 * Copyright (C) 2016 Deepin Technology Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 **/

#include <locale.h>

#include <QtWidgets>
#include <QtDBus>

#include <DLog>
#include <DMainWindow>
#include <DApplication>
#include <DWidgetUtil>

#include "config.h"

#include "options.h"
#include "dmr_settings.h"
#include "mainwindow.h"
#include "dbus_adpator.h"
#ifdef ENABLE_VPU_PLATFORM
#include "vpu_proxy.h"
#endif
#include "movie_configuration.h"

DWIDGET_USE_NAMESPACE


int main(int argc, char *argv[])
{
    DApplication::loadDXcbPlugin();

#if defined(STATIC_LIB)
    DWIDGET_INIT_RESOURCE();
#endif

    DApplication app(argc, argv);

    // required by mpv
    setlocale(LC_NUMERIC, "C");

    auto& clm = dmr::CommandLineManager::get();
    clm.process(app);

    QString toOpenFile;
    if (1 == clm.positionalArguments().length()) {
        toOpenFile = clm.positionalArguments().first();
    }

    app.setOrganizationName("deepin");
    app.setApplicationName("deepin-movie");
    app.setApplicationVersion(DMR_VERSION);
    app.setProductIcon(QPixmap(":/resources/icons/logo-big.svg"));
    app.setProductName(QObject::tr("Deepin Movie"));
    app.setApplicationLicense("GPL v3");
    app.setApplicationDescription(QObject::tr(
                "Deepin Movie is a well-designed and full-featured"
                " video player with simple borderless design. It supports local and"
                " streaming media play with multiple video formats."));

    auto light_theme = dmr::Settings::get().settings()->getOption("internal.light_theme").toBool();
    app.setTheme(light_theme ? "light": "dark");

    Dtk::Core::DLogManager::registerConsoleAppender();
    Dtk::Core::DLogManager::registerFileAppender();
    qDebug() << "log path: " << Dtk::Core::DLogManager::getlogFilePath();

    bool singleton = !dmr::Settings::get().isSet(dmr::Settings::MultipleInstance);
    if (singleton && !app.setSingleInstance("deepinmovie")) {
        qDebug() << "another deepin movie instance has started";
        if (!toOpenFile.isEmpty()) {
            QDBusInterface iface("com.deepin.movie", "/", "com.deepin.movie");
            iface.asyncCall("openFile", toOpenFile);
        }
        
        exit(0);
    }

    app.loadTranslator();

    app.setWindowIcon(QIcon(":/resources/icons/logo.svg"));
    app.setApplicationDisplayName(QObject::tr("Deepin Movie"));
    app.setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);

    MovieConfiguration::get().init();

    QRegExp url_re("\\w+://");

#ifdef ENABLE_VPU_PLATFORM
    if (dmr::CommandLineManager::get().vpuDemoMode()) {
        dmr::VpuProxy mw;
        mw.setMinimumSize(QSize(528, 400));
        mw.resize(850, 600);
        DUtility::moveToCenter(&mw);
        mw.show();
        auto fi = QFileInfo(toOpenFile);
        if (fi.exists()) {
            mw.setPlayFile(fi);
            mw.play();
        }

        return app.exec();
    }
#endif

    dmr::MainWindow mw;
    mw.setMinimumSize(QSize(528, 400));
    mw.resize(850, 600);
    Dtk::Widget::moveToCenter(&mw);
    mw.show();

    if (!QDBusConnection::sessionBus().isConnected()) {
        qWarning() << "dbus disconnected";
    }

    ApplicationAdaptor adaptor(&mw);
    QDBusConnection::sessionBus().registerService("com.deepin.movie");
    QDBusConnection::sessionBus().registerObject("/", &mw);

    if (!toOpenFile.isEmpty()) {
        QUrl url;
        if (url_re.indexIn(toOpenFile) == 0) {
            url = QUrl(toOpenFile);
        } else {
            url = QUrl::fromLocalFile(toOpenFile);
        }

        qDebug() << url;
        mw.play(url);
    }
    return app.exec();
}

