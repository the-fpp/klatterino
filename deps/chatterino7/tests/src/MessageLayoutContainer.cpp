// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayoutContainer.hpp"

#include "common/Literals.hpp"
#include "messages/Emote.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "mocks/BaseApplication.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>

#include <array>
#include <memory>
#include <vector>

using namespace chatterino;
using namespace literals;

namespace {

class MockApplication : mock::BaseApplication
{
public:
    MockApplication()
        : theme(this->paths_)
        , fonts(this->settings)
        , windows(this->args_, this->paths_, this->settings, this->theme,
                  this->fonts)
    {
    }
    Theme *getThemes() override
    {
        return &this->theme;
    }

    Fonts *getFonts() override
    {
        return &this->fonts;
    }

    WindowManager *getWindows() override
    {
        return &this->windows;
    }

    SeventvPaints *getSeventvPaints() override
    {
        return &this->paints;
    }

    Theme theme;
    Fonts fonts;
    WindowManager windows;
    SeventvPaints paints;
};

QImage renderUsername(MessageElementFlags flags, bool userLink)
{
    MessageLayoutContainer container;
    Message message;
    MessageLayoutContext ctx{
        .messageColors = {},
        .flags =
            MessageElementFlags{
                MessageElementFlag::Username,
                MessageElementFlag::KickUsername,
                MessageElementFlag::RumbleUsername,
            },
        .width = 300,
        .scale = 1.0F,
        .imageScale = 1.0F,
        .selectedChannel = nullptr,
        .message = message,
    };
    container.beginLayout(ctx.width, ctx.scale, ctx.imageScale, {});

    auto username = std::make_shared<TextElement>(
        QStringLiteral("fixtureuser:"), flags, QColor(255, 0, 255),
        FontStyle::ChatMediumBold);
    if (userLink)
    {
        username->setLink({Link::UserInfo, QStringLiteral("fixtureuser")});
    }
    username->addToContainer(container, ctx);
    container.endLayout();

    QImage image(QSize(300, 50), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    const Selection selection{};
    const MessagePreferences preferences{};
    const MessagePaintContext paintContext{
        .painter = painter,
        .selection = selection,
        .colorProvider = ColorProvider::instance(),
        .messageColors = ctx.messageColors,
        .preferences = preferences,
        .canvasWidth = image.width(),
    };
    container.paintElements(painter, paintContext);
    return image;
}

std::vector<std::shared_ptr<MessageElement>> makeElements(const QString &text)
{
    std::vector<std::shared_ptr<MessageElement>> elements;
    bool seenUsername = false;
    for (const auto &word : text.split(' '))
    {
        if (word.startsWith('@'))
        {
            if (seenUsername)
            {
                elements.emplace_back(std::make_shared<MentionElement>(
                    word, word, MessageColor{}, MessageColor{}));
            }
            else
            {
                elements.emplace_back(std::make_shared<TextElement>(
                    word, MessageElementFlag::Username, MessageColor{},
                    FontStyle::ChatMediumBold));
                seenUsername = true;
            }
            continue;
        }

        if (word.startsWith('!'))
        {
            auto emote = std::make_shared<Emote>(Emote{
                .name = EmoteName{word},
                .images = ImageSet{Image::fromResourcePixmap(
                    getResources().twitch.automod)},
                .tooltip = {},
                .homePage = {},
                .id = {},
                .author = {},
                .baseName = {},
            });
            elements.emplace_back(std::make_shared<EmoteElement>(
                emote, MessageElementFlag::Emote));
            continue;
        }

        elements.emplace_back(std::make_shared<TextElement>(
            word, MessageElementFlag::Text, MessageColor{},
            FontStyle::ChatMedium));
    }

    return elements;
}

using TestParam = std::tuple<QString, QString, TextDirection>;

}  // namespace

namespace chatterino {

class MessageLayoutContainerTest : public ::testing::TestWithParam<TestParam>
{
public:
    MessageLayoutContainerTest() = default;

    MockApplication mockApplication;
};

TEST_P(MessageLayoutContainerTest, RtlReordering)
{
    auto [inputText, expected, expectedDirection] = GetParam();
    MessageLayoutContainer container;
    Message message;
    MessageLayoutContext ctx{
        .messageColors = {},
        .flags =
            {
                MessageElementFlag::Text,
                MessageElementFlag::Username,
                MessageElementFlag::Emote,
            },
        .width = 10000,
        .scale = 1.0F,
        .imageScale = 1.0F,
        .selectedChannel = nullptr,
        .message = message,
    };
    container.beginLayout(ctx.width, ctx.scale, ctx.imageScale,
                          {MessageFlag::Collapsed});

    auto elements = makeElements(inputText);
    for (const auto &element : elements)
    {
        element->addToContainer(container, ctx);
    }
    container.endLayout();
    ASSERT_EQ(container.line_, 1) << "unexpected linebreak";

    int x = -1;
    for (const auto &el : container.elements_)
    {
        ASSERT_LT(x, el->getRect().x());
        x = el->getRect().x();
    }

    QString got;
    for (const auto &el : container.elements_)
    {
        if (!got.isNull())
        {
            got.append(' ');
        }

        if (dynamic_cast<ImageLayoutElement *>(el.get()))
        {
            el->addCopyTextToString(got);
            if (el->hasTrailingSpace())
            {
                got.chop(1);
            }
        }
        else
        {
            got.append(el->getText());
        }
    }

    ASSERT_EQ(got, expected) << got;
    ASSERT_EQ(container.textDirection_, expectedDirection) << got;
}

TEST_F(MessageLayoutContainerTest, RumblePlatformBadgeUsesPackagedImage)
{
    const auto resource1x =
        platformBadgeResource(MessagePlatform::Rumble, false).toString();
    const auto resource2x =
        platformBadgeResource(MessagePlatform::Rumble, true).toString();
    EXPECT_EQ(resource1x, QStringLiteral(":/badges/platform-rumble-18.webp"));
    EXPECT_EQ(resource2x, QStringLiteral(":/badges/platform-rumble-36.webp"));

    const QImage image1x(resource1x);
    const QImage image2x(resource2x);
    ASSERT_FALSE(image1x.isNull());
    ASSERT_FALSE(image2x.isNull());
    EXPECT_EQ(image1x.size(), QSize(18, 18));
    EXPECT_EQ(image2x.size(), QSize(36, 36));
    EXPECT_TRUE(image1x.hasAlphaChannel());
    EXPECT_TRUE(image2x.hasAlphaChannel());

    bool hasVisibleGreenPixel = false;
    bool hasTransparentPixel = false;
    for (int y = 0; y < image1x.height(); ++y)
    {
        for (int x = 0; x < image1x.width(); ++x)
        {
            const auto color = image1x.pixelColor(x, y);
            hasVisibleGreenPixel =
                hasVisibleGreenPixel ||
                (color.alpha() > 0 && color.green() > color.red() &&
                 color.green() > color.blue());
            hasTransparentPixel = hasTransparentPixel || color.alpha() == 0;
        }
    }
    EXPECT_TRUE(hasVisibleGreenPixel);
    EXPECT_TRUE(hasTransparentPixel);
}

TEST_F(MessageLayoutContainerTest,
       RumbleUsernameSuppressesTwitchAndKickSevenTVPaints)
{
    this->mockApplication.paints.addPaint(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("fixture-paint")},
        {QStringLiteral("name"), QStringLiteral("Fixture paint")},
        {QStringLiteral("function"), QStringLiteral("LINEAR_GRADIENT")},
        {QStringLiteral("repeat"), false},
        {QStringLiteral("angle"), 0},
        {QStringLiteral("stops"),
         QJsonArray{
             QJsonObject{{QStringLiteral("at"), 0.0},
                         {QStringLiteral("color"), 0x00FF00FF}},
             QJsonObject{{QStringLiteral("at"), 1.0},
                         {QStringLiteral("color"), 0x00FF00FF}},
         }},
        {QStringLiteral("shadows"), QJsonArray{}},
    });
    const std::array<seventv::eventapi::User, 2> users{
        seventv::eventapi::TwitchUser(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("twitch-id")},
            {QStringLiteral("username"), QStringLiteral("fixtureuser")},
        }),
        seventv::eventapi::KickUser(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("123")},
            {QStringLiteral("username"), QStringLiteral("fixtureuser")},
        }),
    };
    this->mockApplication.paints.assignPaintToUsers(
        QStringLiteral("fixture-paint"), users);

    const auto plain = renderUsername(MessageElementFlag::Username, false);
    const auto twitch = renderUsername(MessageElementFlag::Username, true);
    const auto kick =
        renderUsername(MessageElementFlags{MessageElementFlag::Username,
                                           MessageElementFlag::KickUsername},
                       true);
    const auto rumble =
        renderUsername(MessageElementFlags{MessageElementFlag::Username,
                                           MessageElementFlag::RumbleUsername},
                       true);

    EXPECT_NE(twitch, plain);
    EXPECT_NE(kick, plain);
    EXPECT_EQ(rumble, plain);
}

INSTANTIATE_TEST_SUITE_P(
    MessageLayoutContainer, MessageLayoutContainerTest,
    testing::Values(
        TestParam{
            u"@aliens foo bar baz @foo qox !emote1 !emote2"_s,
            u"@aliens foo bar baz @foo qox !emote1 !emote2"_s,
            TextDirection::LTR,
        },
        TestParam{
            u"@aliens ! foo bar baz @foo qox !emote1 !emote2"_s,
            u"@aliens ! foo bar baz @foo qox !emote1 !emote2"_s,
            TextDirection::LTR,
        },
        TestParam{
            u"@aliens ."_s,
            u"@aliens ."_s,
            TextDirection::Neutral,
        },
        // RTL
        TestParam{
            u"@aliens و غير دارت إعادة, بل كما وقام قُدُماً. قام تم الجوي بوابة, خلاف أراض هو بلا. عن وحتّى ميناء غير"_s,
            u"@aliens غير ميناء وحتّى عن بلا. هو أراض خلاف بوابة, الجوي تم قام قُدُماً. وقام كما بل إعادة, دارت غير و"_s,
            TextDirection::RTL,
        },
        TestParam{
            u"@aliens و غير دارت إعادة, بل ض هو my LTR 123 بلا. عن 123 456 وحتّى ميناء غير"_s,
            u"@aliens غير ميناء وحتّى 456 123 عن بلا. my LTR 123 هو ض بل إعادة, دارت غير و"_s,
            TextDirection::RTL,
        },
        TestParam{
            u"@aliens ور دارت إ @user baz bar عاد هو my LTR 123 بلا. عن 123 456 وحتّ غير"_s,
            u"@aliens غير وحتّ 456 123 عن بلا. my LTR 123 هو عاد baz bar @user إ دارت ور"_s,
            TextDirection::RTL,
        },
        TestParam{
            u"@aliens ور !emote1 !emote2 !emote3 دارت إ @user baz bar عاد هو my LTR 123 بلا. عن 123 456 وحتّ غير"_s,
            u"@aliens غير وحتّ 456 123 عن بلا. my LTR 123 هو عاد baz bar @user إ دارت !emote3 !emote2 !emote1 ور"_s,
            TextDirection::RTL,
        },
        TestParam{
            u"@aliens ور !emote1 !emote2 LTR text !emote3 !emote4 غير"_s,
            u"@aliens غير LTR text !emote3 !emote4 !emote2 !emote1 ور"_s,
            TextDirection::RTL,
        },

        TestParam{
            u"@aliens !!! ور !emote1 !emote2 LTR text !emote3 !emote4 غير"_s,
            u"@aliens غير LTR text !emote3 !emote4 !emote2 !emote1 ور !!!"_s,
            TextDirection::RTL,
        },
        // LTR
        TestParam{
            u"@aliens LTR و غير دا ميناء غير"_s,
            u"@aliens LTR غير ميناء دا غير و"_s,
            TextDirection::LTR,
        },
        TestParam{
            u"@aliens LTR و غير د ض هو my LTR 123 بلا. عن 123 456 وحتّى مير"_s,
            u"@aliens LTR هو ض د غير و my LTR 123 مير وحتّى 456 123 عن بلا."_s,
            TextDirection::LTR,
        },
        TestParam{
            u"@aliens LTR ور دارت إ @user baz bar عاد هو my LTR 123 بلا. عن 123 456 وحتّ غير"_s,
            u"@aliens LTR @user إ دارت ور baz bar هو عاد my LTR 123 غير وحتّ 456 123 عن بلا."_s,
            TextDirection::LTR,
        },
        TestParam{
            u"@aliens LTR ور !emote1 !emote2 !emote3 دارت إ @user baz bar عاد هو my LTR 123 بلا. عن 123 456 وحتّ غير"_s,
            u"@aliens LTR @user إ دارت !emote3 !emote2 !emote1 ور baz bar هو عاد my LTR 123 غير وحتّ 456 123 عن بلا."_s,
            TextDirection::LTR,
        },
        TestParam{
            u"@aliens LTR غير وحتّ !emote1 !emote2 LTR text !emote3 !emote4 عاد هو"_s,
            u"@aliens LTR !emote2 !emote1 وحتّ غير LTR text !emote3 !emote4 هو عاد"_s,
            TextDirection::LTR,
        }));

}  // namespace chatterino
