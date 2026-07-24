// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/network/CertificateBundle.hpp"

#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>

namespace chatterino {

bool applyCertificateBundle(const QString &path)
{
    constexpr qint64 MAX_CERTIFICATE_BUNDLE_SIZE = 8 * 1024 * 1024;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }

    const auto data = file.read(MAX_CERTIFICATE_BUNDLE_SIZE + 1);
    if (data.isEmpty() || data.size() > MAX_CERTIFICATE_BUNDLE_SIZE)
    {
        return false;
    }

    constexpr auto BEGIN_CERTIFICATE = "-----BEGIN CERTIFICATE-----";
    constexpr auto END_CERTIFICATE = "-----END CERTIFICATE-----";
    QList<QSslCertificate> certificates;
    qsizetype position = 0;
    while (true)
    {
        const auto begin = data.indexOf(BEGIN_CERTIFICATE, position);
        if (begin < 0)
        {
            break;
        }

        const auto end = data.indexOf(END_CERTIFICATE, begin);
        if (end < 0)
        {
            return false;
        }

        position = end + QByteArrayView(END_CERTIFICATE).size();
        const auto parsed = QSslCertificate::fromData(
            data.sliced(begin, position - begin), QSsl::Pem);
        if (parsed.isEmpty())
        {
            return false;
        }
        certificates.append(parsed);
    }

    if (certificates.isEmpty())
    {
        return false;
    }

    auto configuration = QSslConfiguration::defaultConfiguration();
    configuration.setCaCertificates(certificates);
    QSslConfiguration::setDefaultConfiguration(configuration);
    return true;
}

}  // namespace chatterino
