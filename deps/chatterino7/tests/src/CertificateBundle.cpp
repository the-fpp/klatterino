// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/network/CertificateBundle.hpp"

#include <gtest/gtest.h>
#include <QFile>
#include <QSslConfiguration>
#include <QTemporaryFile>

namespace chatterino {
namespace {

class DefaultCertificateConfiguration
{
public:
    DefaultCertificateConfiguration()
        : previous_(QSslConfiguration::defaultConfiguration())
    {
    }

    ~DefaultCertificateConfiguration()
    {
        QSslConfiguration::setDefaultConfiguration(this->previous_);
    }

private:
    QSslConfiguration previous_;
};

TEST(CertificateBundle, RejectsMissingBundleWithoutChangingConfiguration)
{
    DefaultCertificateConfiguration restore;
    const auto before =
        QSslConfiguration::defaultConfiguration().caCertificates();

    EXPECT_FALSE(applyCertificateBundle(
        QStringLiteral(":/certificates/does-not-exist.pem")));
    EXPECT_EQ(QSslConfiguration::defaultConfiguration().caCertificates(),
              before);
}

TEST(CertificateBundle, AppliesPemCertificateChain)
{
    DefaultCertificateConfiguration restore;

    ASSERT_TRUE(applyCertificateBundle(
        QStringLiteral(":/certificates/test-chain.pem")));
    EXPECT_EQ(QSslConfiguration::defaultConfiguration().caCertificates().size(),
              3);
}

TEST(CertificateBundle, AppliesPemBundleWithCertificateLabels)
{
    DefaultCertificateConfiguration restore;
    QFile source(QStringLiteral(":/certificates/test-chain.pem"));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    auto data = source.readAll();
    data.replace("-----BEGIN CERTIFICATE-----",
                 "Friendly certificate label\n-----BEGIN CERTIFICATE-----");
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    ASSERT_EQ(file.write(data), data.size());
    ASSERT_TRUE(file.flush());

    ASSERT_TRUE(applyCertificateBundle(file.fileName()));
    EXPECT_EQ(QSslConfiguration::defaultConfiguration().caCertificates().size(),
              3);
}

TEST(CertificateBundle, RejectsInvalidPemWithoutChangingConfiguration)
{
    DefaultCertificateConfiguration restore;
    const auto before =
        QSslConfiguration::defaultConfiguration().caCertificates();
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    ASSERT_EQ(file.write("not a certificate"), 17);
    ASSERT_TRUE(file.flush());

    EXPECT_FALSE(applyCertificateBundle(file.fileName()));
    EXPECT_EQ(QSslConfiguration::defaultConfiguration().caCertificates(),
              before);
}

TEST(CertificateBundle, RejectsOversizedBundleWithoutChangingConfiguration)
{
    DefaultCertificateConfiguration restore;
    const auto before =
        QSslConfiguration::defaultConfiguration().caCertificates();
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    ASSERT_TRUE(file.resize(8 * 1024 * 1024 + 1));

    EXPECT_FALSE(applyCertificateBundle(file.fileName()));
    EXPECT_EQ(QSslConfiguration::defaultConfiguration().caCertificates(),
              before);
}

}  // namespace
}  // namespace chatterino
