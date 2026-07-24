#include "common/network/CertificateBundle.hpp"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <iostream>

namespace {

struct Arguments {
    QUrl httpsUrl;
    QUrl wssUrl;
    bool caOnly = false;
};

bool parseArguments(const QStringList &arguments, Arguments &parsed)
{
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("--ca-only"))
    {
        parsed.caOnly = true;
        return true;
    }

    for (qsizetype index = 1; index < arguments.size(); index += 2)
    {
        if (index + 1 >= arguments.size())
        {
            return false;
        }

        const auto &option = arguments.at(index);
        const QUrl value(arguments.at(index + 1));
        if (option == QStringLiteral("--https-url"))
        {
            parsed.httpsUrl = value;
        }
        else if (option == QStringLiteral("--wss-url"))
        {
            parsed.wssUrl = value;
        }
        else
        {
            return false;
        }
    }

    return parsed.httpsUrl.isValid() &&
           parsed.httpsUrl.scheme() == QStringLiteral("https") &&
           parsed.wssUrl.isValid() &&
           parsed.wssUrl.scheme() == QStringLiteral("wss") &&
           parsed.httpsUrl.userInfo().isEmpty() &&
           parsed.wssUrl.userInfo().isEmpty();
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Arguments arguments;
    if (!parseArguments(app.arguments(), arguments))
    {
        std::cerr << "usage: portable-linux-runtime-probe --ca-only | "
                     "--https-url HTTPS_URL --wss-url WSS_URL\n";
        return 2;
    }

    if (!chatterino::applyCertificateBundle(
            qEnvironmentVariable("CHATTERINO_SSL_CERT_FILE")))
    {
        std::cerr << "portable-ca-configuration=failed\n";
        return 1;
    }

    if (!QSslSocket::supportsSsl())
    {
        std::cerr << "portable-tls=unavailable\n";
        return 1;
    }

    if (arguments.caOnly)
    {
        std::cout << "portable-ca-configuration=ok\n";
        return 0;
    }

    bool httpsFinished = false;
    bool wssFinished = false;
    bool failed = false;

    const auto finishIfReady = [&] {
        if (!httpsFinished || !wssFinished)
        {
            return;
        }

        if (!failed)
        {
            std::cout << "portable-ca-configuration=ok\n"
                         "portable-qt-https=ok\nportable-qt-wss=ok\n";
        }
        app.exit(failed ? 1 : 0);
    };

    QNetworkAccessManager network;
    QNetworkRequest request(arguments.httpsUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = network.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &app, [&] {
        const auto status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 ||
            status >= 400)
        {
            std::cerr << "portable-qt-https=failed: "
                      << reply->errorString().toStdString() << '\n';
            failed = true;
        }
        httpsFinished = true;
        reply->deleteLater();
        finishIfReady();
    });

    QWebSocket socket;
    QObject::connect(&socket, &QWebSocket::connected, &app, [&] {
        wssFinished = true;
        socket.close();
        finishIfReady();
    });
    QObject::connect(&socket, &QWebSocket::errorOccurred, &app,
                     [&](QAbstractSocket::SocketError) {
                         if (!wssFinished)
                         {
                             std::cerr << "portable-qt-wss=failed: "
                                       << socket.errorString().toStdString()
                                       << '\n';
                             failed = true;
                             wssFinished = true;
                             finishIfReady();
                         }
                     });
    socket.open(arguments.wssUrl);

    QTimer::singleShot(15000, &app, [&] {
        if (!httpsFinished || !wssFinished)
        {
            std::cerr << "portable-runtime-probe=timeout\n";
            app.exit(1);
        }
    });

    return app.exec();
}
