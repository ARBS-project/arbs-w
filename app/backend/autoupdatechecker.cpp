#include "autoupdatechecker.h"

#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>

#include "arbs/arbs_update.h"

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);

    // Allow HTTP redirects
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);

    qDebug() << "Current ARBS-W version:" << ARBS_W_VERSION_STR;
}

void AutoUpdateChecker::start()
{
    if (!m_Nam) {
        Q_ASSERT(m_Nam);
        return;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE) // Only run update checker on platforms without auto-update
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // We'll get a callback when this is finished
    // ARBS-W R1b (D-R1b-6): GitHub Releases of this fork. Drafts and pre-releases are not returned by /latest.
    QUrl url(QString::fromLatin1(arbsw::kLatestReleaseApiUrl));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString::fromStdString(arbsw::updateUserAgent(ARBS_W_VERSION_STR)));
    request.setRawHeader("Accept", "application/vnd.github+json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    m_Nam->get(request);
#endif
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    // Delete the QNetworkAccessManager to free resources and
    // prevent the bearer plugin from polling in the background.
    m_Nam->deleteLater();
    m_Nam = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        QJsonParseError error;
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(body, &error);
        if (jsonDoc.isNull() || !jsonDoc.isObject()) {
            qWarning() << "Update response malformed:" << error.errorString();
            return;
        }

        const QJsonObject release = jsonDoc.object();
        if (!release["tag_name"].isString() || !release["html_url"].isString()) {
            qWarning() << "Update response missing tag_name or html_url";
            return;
        }

        const std::string tag = release["tag_name"].toString().toStdString();
        const std::string pageUrl = release["html_url"].toString().toStdString();
        const std::string latestVersion = arbsw::releaseVersionText(tag);
        if (latestVersion.empty()) {
            qWarning() << "Update response has an unrecognized version tag";
            return;
        }
        if (!arbsw::isTrustedReleaseUrl(pageUrl)) {
            qWarning() << "Update response link is not an ARBS-W release page";
            return;
        }

        if (arbsw::isNewerRelease(ARBS_W_VERSION_STR, tag)) {
            qDebug() << "Update available";
            emit onUpdateAvailable(QString::fromStdString(latestVersion), QString::fromStdString(pageUrl));
        }
        else {
            qDebug() << "No newer ARBS-W release";
        }
    }
    else if (reply->error() == QNetworkReply::ContentNotFoundError) {
        // 404 = no published release yet: same as "no update".
        qDebug() << "No published ARBS-W release";
        reply->deleteLater();
    }
    else {
        qWarning() << "Update checking failed with error:" << reply->error();
        reply->deleteLater();
    }
}
