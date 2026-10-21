// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

namespace localization {
inline QString language(const QStringList &preferences = QLocale::system().uiLanguages()) {
    for (const auto &preference : preferences) {
        const auto value = QLocale(preference).language();
        if (value == QLocale::Russian) return QStringLiteral("ru");
        if (value == QLocale::English) return QStringLiteral("en");
    }
    return QStringLiteral("en");
}

// The original interface is Russian; the English catalog is embedded so an
// uninstalled build and a fresh installation use the same language resources.
inline bool load(QTranslator &translator,
                 const QStringList &preferences = QLocale::system().uiLanguages()) {
    QCoreApplication::removeTranslator(&translator);
    if (language(preferences) == QStringLiteral("ru")) {
        (void)translator.load(QString()); // discard any previous English catalog
        return true;
    }
    if (!translator.load(QStringLiteral(":/i18n/kquickshare_en.qm"))) return false;
    return QCoreApplication::installTranslator(&translator);
}
} // namespace localization
