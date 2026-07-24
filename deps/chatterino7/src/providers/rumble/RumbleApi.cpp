// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleApi.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrlQuery>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include <algorithm>
#include <array>
#include <deque>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace chatterino::rumble {
namespace {

// Current pages include a large unrelated inline application after the
// resolver contract. A sanitized video page captured on 2026-07-17 was
// 1,827,560 bytes, but its title and embed reference were complete by about
// 631 KiB. Page requests are parsed incrementally and cancelled once their
// authoritative prefix is complete; this remains the hard bound for a page
// which never supplies that contract.
constexpr qsizetype MAX_PAGE_BYTES = 4 * 1024 * 1024;
constexpr qsizetype MAX_EMBED_BYTES = 256 * 1024;
// Rumble's init record carries the current user/channel catalogs and history
// in one JSON event. The browser client accepts that record as a single SSE
// message, and observed public init records can exceed the old 64 KiB cap.
// Keep both limits bounded while leaving room for the current web contract.
constexpr qsizetype MAX_SSE_PENDING_BYTES = 2 * 1024 * 1024;
constexpr qsizetype MAX_SSE_EVENT_BYTES = 1024 * 1024;
constexpr std::size_t MAX_SSE_EVENTS = 64;
constexpr std::size_t MAX_STREAM_HANDOFFS = 8;

bool isDecimalId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[1-9][0-9]{0,127}$"));
    return pattern.match(value).hasMatch();
}

bool isDecimalText(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+$"));
    return pattern.match(value).hasMatch();
}

bool isEmbedId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^v[a-z0-9]{1,127}$"));
    return pattern.match(value).hasMatch();
}

bool isChannelSlug(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$"));
    return pattern.match(value).hasMatch();
}

bool hasValidPercentEncoding(const QByteArray &encoded)
{
    auto hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    };
    for (qsizetype i = 0; i < encoded.size(); ++i)
    {
        if (encoded[i] != '%')
        {
            continue;
        }
        if (i + 2 >= encoded.size() || !hex(encoded[i + 1]) ||
            !hex(encoded[i + 2]))
        {
            return false;
        }
        i += 2;
    }
    return true;
}

std::optional<QString> decodePathSegment(const QByteArray &encoded)
{
    if (encoded.isEmpty() || !hasValidPercentEncoding(encoded))
    {
        return std::nullopt;
    }
    const auto decoded = QUrl::fromPercentEncoding(encoded);
    if (decoded.isEmpty() || decoded.contains(QChar::ReplacementCharacter) ||
        decoded == QStringLiteral(".") || decoded == QStringLiteral(".."))
    {
        return std::nullopt;
    }
    for (const auto ch : decoded)
    {
        if (ch == u'/' || ch == u'\\' || ch.unicode() == 0 ||
            ch.category() == QChar::Other_Control)
        {
            return std::nullopt;
        }
    }
    return decoded;
}

std::optional<std::vector<QString>> decodedPathSegments(const QUrl &url)
{
    const auto encodedPath = url.path(QUrl::FullyEncoded).toUtf8();
    if (!hasValidPercentEncoding(encodedPath))
    {
        return std::nullopt;
    }

    const auto raw = encodedPath.split('/');
    std::vector<QString> segments;
    segments.reserve(static_cast<std::size_t>(raw.size()));
    for (int i = 0; i < raw.size(); ++i)
    {
        if (raw[i].isEmpty())
        {
            if (i == 0 || i == raw.size() - 1)
            {
                continue;
            }
            return std::nullopt;
        }
        const auto decoded = decodePathSegment(raw[i]);
        if (!decoded)
        {
            return std::nullopt;
        }
        segments.push_back(*decoded);
    }
    return segments;
}

QString encodedSegment(const QString &segment)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(segment, "-._~"));
}

constexpr qsizetype MAX_HTML_TAG_CHARS = 4096;

bool isHtmlSpace(QChar value)
{
    return value == u' ' || value == u'\t' || value == u'\r' ||
           value == u'\n' || value == u'\f';
}

bool isHtmlTagNameChar(QChar value)
{
    const auto code = value.unicode();
    return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
           (code >= '0' && code <= '9') || value == u'-' || value == u':';
}

bool isHtmlAttributeNameChar(QChar value)
{
    return !isHtmlSpace(value) && value != QChar::Null && value != u'"' &&
           value != u'\'' && value != u'<' && value != u'>' && value != u'/' &&
           value != u'=';
}

struct HtmlAttribute {
    QString name;
    QString value;
};

struct HtmlTag {
    QString name;
    std::vector<HtmlAttribute> attributes;
    qsizetype end = 0;
    bool closing = false;
    bool selfClosing = false;
};

std::optional<qsizetype> findHtmlTagEnd(const QString &html, qsizetype start,
                                        qsizetype cursor,
                                        bool enforceCandidateLimit = true)
{
    QChar quote;
    for (; cursor < html.size(); ++cursor)
    {
        if (enforceCandidateLimit && cursor - start > MAX_HTML_TAG_CHARS)
        {
            return std::nullopt;
        }
        const auto value = html[cursor];
        if (!quote.isNull())
        {
            if (value == quote)
            {
                quote = {};
            }
            continue;
        }
        if (value == u'"' || value == u'\'')
        {
            quote = value;
        }
        else if (value == u'>')
        {
            return cursor;
        }
    }
    return std::nullopt;
}

bool isPageContractTag(const QString &name)
{
    static const std::array contractTags = {
        QStringLiteral("form"),     QStringLiteral("iframe"),
        QStringLiteral("noembed"),  QStringLiteral("noframes"),
        QStringLiteral("noscript"), QStringLiteral("plaintext"),
        QStringLiteral("script"),   QStringLiteral("style"),
        QStringLiteral("template"), QStringLiteral("textarea"),
        QStringLiteral("title"),    QStringLiteral("xmp"),
    };
    return std::ranges::find(contractTags, name) != contractTags.end();
}

std::optional<qsizetype> skipUnrelatedHtmlTag(const QString &html,
                                              qsizetype start, qsizetype cursor)
{
    // The response body is already capped at MAX_PAGE_BYTES. Do not reject a
    // page because an unrelated hydration/SVG tag exceeds the much smaller
    // contract-candidate limit or repeats an attribute which cannot affect
    // title, interstitial, template/raw-text, or embed extraction.
    return findHtmlTagEnd(html, start, cursor, false);
}

std::optional<HtmlTag> parseHtmlTag(const QString &html, qsizetype start)
{
    if (start < 0 || start >= html.size() || html[start] != u'<' ||
        start + 1 >= html.size())
    {
        return std::nullopt;
    }

    auto cursor = start + 1;
    bool closing = false;
    if (html[cursor] == u'/')
    {
        closing = true;
        ++cursor;
    }
    const auto nameStart = cursor;
    while (cursor < html.size() && isHtmlTagNameChar(html[cursor]))
    {
        ++cursor;
    }
    if (cursor == nameStart)
    {
        return std::nullopt;
    }

    const auto end = findHtmlTagEnd(html, start, cursor);
    if (!end)
    {
        return std::nullopt;
    }

    HtmlTag tag{
        .name = html.mid(nameStart, cursor - nameStart).toLower(),
        .end = *end,
        .closing = closing,
    };
    if (closing)
    {
        while (cursor < *end && isHtmlSpace(html[cursor]))
        {
            ++cursor;
        }
        return cursor == *end ? std::optional<HtmlTag>(std::move(tag))
                              : std::nullopt;
    }

    if (cursor < *end && !isHtmlSpace(html[cursor]) && html[cursor] != u'/')
    {
        return std::nullopt;
    }

    while (cursor < *end)
    {
        while (cursor < *end && isHtmlSpace(html[cursor]))
        {
            ++cursor;
        }
        if (cursor == *end)
        {
            break;
        }
        if (html[cursor] == u'/')
        {
            ++cursor;
            while (cursor < *end && isHtmlSpace(html[cursor]))
            {
                ++cursor;
            }
            if (cursor != *end)
            {
                return std::nullopt;
            }
            tag.selfClosing = true;
            break;
        }

        const auto attributeStart = cursor;
        while (cursor < *end && isHtmlAttributeNameChar(html[cursor]))
        {
            ++cursor;
        }
        if (cursor == attributeStart)
        {
            return std::nullopt;
        }
        auto name = html.mid(attributeStart, cursor - attributeStart).toLower();
        while (cursor < *end && isHtmlSpace(html[cursor]))
        {
            ++cursor;
        }

        QString value;
        if (cursor < *end && html[cursor] == u'=')
        {
            ++cursor;
            while (cursor < *end && isHtmlSpace(html[cursor]))
            {
                ++cursor;
            }
            if (cursor == *end)
            {
                return std::nullopt;
            }
            if (html[cursor] == u'"' || html[cursor] == u'\'')
            {
                const auto quote = html[cursor++];
                const auto valueStart = cursor;
                while (cursor < *end && html[cursor] != quote)
                {
                    ++cursor;
                }
                if (cursor == *end)
                {
                    return std::nullopt;
                }
                value = html.mid(valueStart, cursor - valueStart);
                ++cursor;
            }
            else
            {
                const auto valueStart = cursor;
                while (cursor < *end && !isHtmlSpace(html[cursor]))
                {
                    if (html[cursor] == u'"' || html[cursor] == u'\'' ||
                        html[cursor] == u'<' || html[cursor] == u'=' ||
                        html[cursor] == u'`')
                    {
                        return std::nullopt;
                    }
                    ++cursor;
                }
                if (cursor == valueStart)
                {
                    return std::nullopt;
                }
                value = html.mid(valueStart, cursor - valueStart);
            }
        }

        if (std::ranges::any_of(tag.attributes,
                                [&](const HtmlAttribute &attribute) {
                                    return attribute.name == name;
                                }))
        {
            return std::nullopt;
        }
        tag.attributes.push_back({std::move(name), std::move(value)});
    }
    return tag;
}

const QString *htmlAttribute(const HtmlTag &tag, const QString &name)
{
    const auto found = std::ranges::find_if(
        tag.attributes, [&](const HtmlAttribute &attribute) {
            return attribute.name == name;
        });
    return found == tag.attributes.end() ? nullptr : &found->value;
}

std::optional<std::pair<qsizetype, qsizetype>> findHtmlRawClose(
    const QString &html, qsizetype bodyStart, const QString &name)
{
    const auto needle = QStringLiteral("</") + name;
    auto cursor = bodyStart;
    while (cursor < html.size())
    {
        const auto start = html.indexOf(needle, cursor, Qt::CaseInsensitive);
        if (start < 0)
        {
            return std::nullopt;
        }
        const auto afterName = start + needle.size();
        if (afterName < html.size() && isHtmlTagNameChar(html[afterName]))
        {
            cursor = afterName;
            continue;
        }
        const auto tag = parseHtmlTag(html, start);
        if (!tag || !tag->closing || tag->name != name)
        {
            return std::nullopt;
        }
        return std::pair{start, tag->end + 1};
    }
    return std::nullopt;
}

bool isHtmlRawTextElement(const QString &name)
{
    static const std::array rawTextElements = {
        QStringLiteral("script"),   QStringLiteral("style"),
        QStringLiteral("textarea"), QStringLiteral("title"),
        QStringLiteral("noscript"), QStringLiteral("xmp"),
        QStringLiteral("iframe"),   QStringLiteral("noembed"),
        QStringLiteral("noframes"),
    };
    return std::ranges::find(rawTextElements, name) != rawTextElements.end();
}

std::optional<std::pair<qsizetype, qsizetype>> findHtmlTemplateClose(
    const QString &html, qsizetype bodyStart)
{
    // Unlike raw-text elements, template contents are tokenized markup and
    // templates may nest. Walk complete tags so `</template>` text inside a
    // comment, attribute, or raw-text child cannot end the inert scope.
    qsizetype cursor = bodyStart;
    std::size_t depth = 1;
    while (cursor < html.size())
    {
        const auto start = html.indexOf(u'<', cursor);
        if (start < 0)
        {
            return std::nullopt;
        }
        if (html.mid(start, 4) == QStringLiteral("<!--"))
        {
            const auto end = html.indexOf(QStringLiteral("-->"), start + 4);
            if (end < 0)
            {
                return std::nullopt;
            }
            cursor = end + 3;
            continue;
        }
        if (start + 1 < html.size() &&
            (html[start + 1] == u'!' || html[start + 1] == u'?'))
        {
            const auto end = findHtmlTagEnd(html, start, start + 2);
            if (!end)
            {
                return std::nullopt;
            }
            cursor = *end + 1;
            continue;
        }

        auto candidate = start + 1;
        bool closingCandidate = false;
        if (candidate < html.size() && html[candidate] == u'/')
        {
            closingCandidate = true;
            ++candidate;
        }
        if (candidate >= html.size() || !isHtmlTagNameChar(html[candidate]))
        {
            if (closingCandidate)
            {
                return std::nullopt;
            }
            cursor = start + 1;
            continue;
        }

        const auto tag = parseHtmlTag(html, start);
        if (!tag)
        {
            return std::nullopt;
        }
        cursor = tag->end + 1;
        if (tag->closing)
        {
            if (tag->name == QStringLiteral("template"))
            {
                if (--depth == 0)
                {
                    return std::pair{start, cursor};
                }
            }
            continue;
        }

        if (tag->name == QStringLiteral("template"))
        {
            // The slash in `<template/>` is ignored by the HTML parser.
            ++depth;
            continue;
        }
        if (tag->name == QStringLiteral("plaintext"))
        {
            // No later source text can close the template once the tokenizer
            // enters plaintext state. Fail closed rather than resuming outside.
            return std::nullopt;
        }
        if (isHtmlRawTextElement(tag->name))
        {
            // A trailing slash does not close these non-void elements either.
            const auto close = findHtmlRawClose(html, cursor, tag->name);
            if (!close)
            {
                return std::nullopt;
            }
            cursor = close->second;
        }
    }
    return std::nullopt;
}

QString decodeHtmlText(QString value)
{
    value.replace(QStringLiteral("&lt;"), QStringLiteral("<"),
                  Qt::CaseInsensitive);
    value.replace(QStringLiteral("&gt;"), QStringLiteral(">"),
                  Qt::CaseInsensitive);
    value.replace(QStringLiteral("&quot;"), QStringLiteral("\""),
                  Qt::CaseInsensitive);
    value.replace(QStringLiteral("&apos;"), QStringLiteral("'"),
                  Qt::CaseInsensitive);
    value.replace(QStringLiteral("&#39;"), QStringLiteral("'"),
                  Qt::CaseInsensitive);
    // Decode ampersand last so an escaped entity is decoded exactly once.
    value.replace(QStringLiteral("&amp;"), QStringLiteral("&"),
                  Qt::CaseInsensitive);
    return value.simplified();
}

struct PageParse {
    enum class State {
        Live,
        Offline,
        Interstitial,
        Malformed,
    };

    enum class FirstVideoState {
        Missing,
        Live,
        Offline,
        Malformed,
    };

    State state = State::Malformed;
    QString title;
    QString embedId;
    FirstVideoState firstVideo = FirstVideoState::Missing;
};

enum class PageParseMode {
    CompleteDocument,
    VideoPrefix,
    ChannelPrefix,
};

bool hasHtmlClass(const HtmlTag &tag, const QString &expected)
{
    const auto *value = htmlAttribute(tag, QStringLiteral("class"));
    if (!value)
    {
        return false;
    }
    qsizetype start = 0;
    while (start < value->size())
    {
        while (start < value->size() && isHtmlSpace((*value)[start]))
        {
            ++start;
        }
        auto end = start;
        while (end < value->size() && !isHtmlSpace((*value)[end]))
        {
            ++end;
        }
        if (value->mid(start, end - start) == expected)
        {
            return true;
        }
        start = end;
    }
    return false;
}

PageParse::FirstVideoState firstVideoState(const HtmlTag &tag)
{
    const auto *duration = htmlAttribute(tag, QStringLiteral("duration"));
    if (!duration)
    {
        return PageParse::FirstVideoState::Malformed;
    }
    if (*duration == QStringLiteral("0"))
    {
        return PageParse::FirstVideoState::Live;
    }
    if (!duration->startsWith(u'0') && isDecimalText(*duration))
    {
        return PageParse::FirstVideoState::Offline;
    }
    return PageParse::FirstVideoState::Malformed;
}

bool isInterstitialPageTitle(const QString &title)
{
    static const std::array interstitialTitles = {
        QStringLiteral("just a moment"),
        QStringLiteral("access denied"),
        QStringLiteral("verify you are human"),
        QStringLiteral("attention required"),
    };
    const auto lowerTitle = title.toLower();
    return std::ranges::any_of(interstitialTitles, [&](const QString &needle) {
        return lowerTitle.contains(needle);
    });
}

std::optional<PageParse> decisivePagePrefix(
    PageParseMode mode, const std::vector<QString> &titles,
    const std::set<QString> &embedIds, bool challengeForm, bool bodyStarted,
    PageParse::FirstVideoState firstVideo)
{
    if (mode == PageParseMode::CompleteDocument || !bodyStarted ||
        titles.size() != 1 || titles.front().size() > 4096)
    {
        return std::nullopt;
    }
    if (challengeForm || isInterstitialPageTitle(titles.front()))
    {
        return PageParse{
            PageParse::State::Interstitial, titles.front(), {}, firstVideo};
    }
    if (mode == PageParseMode::ChannelPrefix &&
        firstVideo == PageParse::FirstVideoState::Offline)
    {
        return PageParse{
            PageParse::State::Offline, titles.front(), {}, firstVideo};
    }
    if (embedIds.size() != 1)
    {
        return std::nullopt;
    }
    if (mode == PageParseMode::VideoPrefix ||
        firstVideo == PageParse::FirstVideoState::Live)
    {
        return PageParse{PageParse::State::Live, titles.front(),
                         *embedIds.begin(), firstVideo};
    }
    return std::nullopt;
}

void collectEmbedUrl(const QJsonValue &value, std::set<QString> &ids,
                     int depth = 0)
{
    if (depth > 8)
    {
        return;
    }
    if (value.isObject())
    {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if ((it.key() == QStringLiteral("embedUrl") ||
                 it.key() == QStringLiteral("embed_url") ||
                 it.key() == QStringLiteral("videoUrl")) &&
                it.value().isString())
            {
                static const QRegularExpression embedUrl(
                    QStringLiteral("^https://(?:www\\.)?rumble\\.com/embed/"
                                   "(v[a-z0-9]+)/?$"),
                    QRegularExpression::CaseInsensitiveOption);
                const auto match = embedUrl.match(it.value().toString());
                if (match.hasMatch() && isEmbedId(match.captured(1)))
                {
                    ids.insert(match.captured(1));
                }
            }
            collectEmbedUrl(it.value(), ids, depth + 1);
        }
    }
    else if (value.isArray())
    {
        for (const auto &child : value.toArray())
        {
            collectEmbedUrl(child, ids, depth + 1);
        }
    }
}

class UniqueObjectKeysHandler
    : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>,
                                          UniqueObjectKeysHandler>
{
public:
    bool StartObject()
    {
        if (this->depth_ >= 16)
        {
            return false;
        }
        ++this->depth_;
        this->objectKeys_.emplace_back();
        return true;
    }

    bool Key(const char *value, rapidjson::SizeType length, bool)
    {
        if (this->objectKeys_.empty() || ++this->keyCount_ > 4096)
        {
            return false;
        }
        return this->objectKeys_.back()
            .emplace(value, static_cast<std::size_t>(length))
            .second;
    }

    bool EndObject(rapidjson::SizeType)
    {
        if (this->objectKeys_.empty())
        {
            return false;
        }
        this->objectKeys_.pop_back();
        --this->depth_;
        return true;
    }

    bool StartArray()
    {
        if (this->depth_ >= 16)
        {
            return false;
        }
        ++this->depth_;
        return true;
    }

    bool EndArray(rapidjson::SizeType)
    {
        if (this->depth_ == 0)
        {
            return false;
        }
        --this->depth_;
        return true;
    }

private:
    std::vector<std::set<std::string>> objectKeys_;
    std::size_t depth_ = 0;
    std::size_t keyCount_ = 0;
};

bool hasUniqueJsonObjectKeys(const QByteArray &body)
{
    rapidjson::MemoryStream stream(body.constData(),
                                   static_cast<std::size_t>(body.size()));
    rapidjson::Reader reader;
    UniqueObjectKeysHandler handler;
    const auto result =
        reader.Parse<rapidjson::kParseValidateEncodingFlag>(stream, handler);
    return !result.IsError();
}

PageParse parsePage(const QByteArray &bytes,
                    PageParseMode mode = PageParseMode::CompleteDocument)
{
    if (bytes.contains('\0'))
    {
        return {};
    }
    auto html = QString::fromUtf8(bytes);
    if (html.contains(QChar::ReplacementCharacter))
    {
        return {};
    }

    std::vector<QString> titles;
    std::set<QString> embedIds;
    bool challengeForm = false;
    bool bodyStarted = false;
    auto firstVideo = PageParse::FirstVideoState::Missing;
    static const QRegularExpression embedUrl(
        QStringLiteral(
            "^https://(?:www\\.)?rumble\\.com/embed/(v[a-z0-9]+)/?$"),
        QRegularExpression::CaseInsensitiveOption);

    qsizetype cursor = 0;
    while (cursor < html.size())
    {
        const auto start = html.indexOf(u'<', cursor);
        if (start < 0)
        {
            break;
        }
        if (html.mid(start, 4) == QStringLiteral("<!--"))
        {
            const auto end = html.indexOf(QStringLiteral("-->"), start + 4);
            if (end < 0)
            {
                return {};
            }
            cursor = end + 3;
            continue;
        }
        if (start + 1 < html.size() &&
            (html[start + 1] == u'!' || html[start + 1] == u'?'))
        {
            const auto end = findHtmlTagEnd(html, start, start + 2);
            if (!end)
            {
                return {};
            }
            cursor = *end + 1;
            continue;
        }

        auto candidate = start + 1;
        bool closingCandidate = false;
        if (candidate < html.size() && html[candidate] == u'/')
        {
            closingCandidate = true;
            ++candidate;
        }
        if (candidate >= html.size() || !isHtmlTagNameChar(html[candidate]))
        {
            if (closingCandidate)
            {
                return {};
            }
            cursor = start + 1;
            continue;
        }

        const auto nameStart = candidate;
        while (candidate < html.size() && isHtmlTagNameChar(html[candidate]))
        {
            ++candidate;
        }
        const auto name = html.mid(nameStart, candidate - nameStart).toLower();
        const bool nameHasBoundary =
            candidate < html.size() &&
            (isHtmlSpace(html[candidate]) || html[candidate] == u'/' ||
             html[candidate] == u'>');
        if (name == QStringLiteral("body") && nameHasBoundary)
        {
            const auto end = skipUnrelatedHtmlTag(html, start, candidate);
            if (!end)
            {
                return {};
            }
            if (!closingCandidate)
            {
                // Only the pre-body HTML document title identifies the page.
                // Body titles (most notably inline-SVG accessibility labels)
                // are foreign/inert metadata, not page-title candidates.
                bodyStarted = true;
            }
            cursor = *end + 1;
            if (const auto prefix =
                    decisivePagePrefix(mode, titles, embedIds, challengeForm,
                                       bodyStarted, firstVideo))
            {
                return *prefix;
            }
            continue;
        }
        if (bodyStarted && !closingCandidate &&
            name == QStringLiteral("title") && nameHasBoundary)
        {
            const auto end = skipUnrelatedHtmlTag(html, start, candidate);
            if (!end)
            {
                return {};
            }
            const auto close =
                findHtmlRawClose(html, *end + 1, QStringLiteral("title"));
            if (!close)
            {
                return {};
            }
            cursor = close->second;
            continue;
        }
        if (!closingCandidate && name == QStringLiteral("div") &&
            nameHasBoundary &&
            firstVideo == PageParse::FirstVideoState::Missing)
        {
            const auto end = skipUnrelatedHtmlTag(html, start, candidate);
            if (!end)
            {
                return {};
            }
            const auto raw = html.mid(start, *end - start + 1);
            if (raw.contains(QStringLiteral("videostream"),
                             Qt::CaseInsensitive))
            {
                const auto tag = parseHtmlTag(html, start);
                if (!tag)
                {
                    firstVideo = PageParse::FirstVideoState::Malformed;
                }
                else if (hasHtmlClass(*tag, QStringLiteral("videostream")))
                {
                    firstVideo = firstVideoState(*tag);
                }
            }
            cursor = *end + 1;
            if (const auto prefix =
                    decisivePagePrefix(mode, titles, embedIds, challengeForm,
                                       bodyStarted, firstVideo))
            {
                return *prefix;
            }
            continue;
        }
        if (!isPageContractTag(name))
        {
            const auto end = skipUnrelatedHtmlTag(html, start, candidate);
            if (!end)
            {
                return {};
            }
            cursor = *end + 1;
            continue;
        }

        const auto tag = parseHtmlTag(html, start);
        if (!tag)
        {
            return {};
        }
        cursor = tag->end + 1;
        if (tag->closing)
        {
            continue;
        }

        if (tag->name == QStringLiteral("script"))
        {
            if (tag->selfClosing)
            {
                return {};
            }
            const auto close =
                findHtmlRawClose(html, cursor, QStringLiteral("script"));
            if (!close)
            {
                return {};
            }
            const auto *type = htmlAttribute(*tag, QStringLiteral("type"));
            if (type)
            {
                const auto normalized = type->trimmed().toLower();
                if (normalized == QStringLiteral("application/json") ||
                    normalized == QStringLiteral("application/ld+json"))
                {
                    const auto json =
                        html.mid(cursor, close->first - cursor).toUtf8();
                    QJsonParseError error;
                    const auto document = QJsonDocument::fromJson(json, &error);
                    if (error.error == QJsonParseError::NoError &&
                        hasUniqueJsonObjectKeys(json))
                    {
                        collectEmbedUrl(document.isObject()
                                            ? QJsonValue(document.object())
                                            : QJsonValue(document.array()),
                                        embedIds);
                    }
                }
            }
            cursor = close->second;
            if (const auto prefix =
                    decisivePagePrefix(mode, titles, embedIds, challengeForm,
                                       bodyStarted, firstVideo))
            {
                return *prefix;
            }
            continue;
        }

        if (tag->name == QStringLiteral("title"))
        {
            if (tag->selfClosing)
            {
                return {};
            }
            const auto close =
                findHtmlRawClose(html, cursor, QStringLiteral("title"));
            if (!close)
            {
                return {};
            }
            const auto raw = html.mid(cursor, close->first - cursor);
            if (raw.size() > 4096 || raw.contains(u'<'))
            {
                return {};
            }
            const auto title = decodeHtmlText(raw);
            if (!title.isEmpty())
            {
                titles.push_back(title);
            }
            cursor = close->second;
            continue;
        }

        // In HTML, <plaintext> consumes the rest of the document as text and
        // has no closing tag. Never expose iframe-looking text after it.
        if (tag->name == QStringLiteral("plaintext"))
        {
            break;
        }

        if (tag->name == QStringLiteral("template"))
        {
            const auto close = findHtmlTemplateClose(html, cursor);
            if (!close)
            {
                return {};
            }
            cursor = close->second;
            continue;
        }

        static const std::array ignoredRawElements = {
            QStringLiteral("style"),    QStringLiteral("textarea"),
            QStringLiteral("noscript"), QStringLiteral("xmp"),
            QStringLiteral("noembed"),  QStringLiteral("noframes"),
        };
        if (std::ranges::find(ignoredRawElements, tag->name) !=
            ignoredRawElements.end())
        {
            // The slash in a non-void HTML start tag is ignored by HTML
            // parsing. Consume through the matching end tag even for `<x/>`
            // so inert content cannot escape into the outer scanner.
            const auto close = findHtmlRawClose(html, cursor, tag->name);
            if (!close)
            {
                return {};
            }
            cursor = close->second;
            continue;
        }

        if (tag->name == QStringLiteral("form"))
        {
            for (const auto *name : {"id", "action"})
            {
                const auto *value =
                    htmlAttribute(*tag, QString::fromLatin1(name));
                if (!value)
                {
                    continue;
                }
                const auto lower = value->toLower();
                challengeForm = challengeForm ||
                                lower.contains(QStringLiteral("challenge")) ||
                                lower.contains(QStringLiteral("captcha")) ||
                                lower.contains(QStringLiteral("cdn-cgi"));
            }
        }

        if (tag->name == QStringLiteral("iframe"))
        {
            const auto *src = htmlAttribute(*tag, QStringLiteral("src"));
            if (src)
            {
                const auto match = embedUrl.match(*src);
                if (match.hasMatch() && isEmbedId(match.captured(1)))
                {
                    embedIds.insert(match.captured(1));
                }
            }
            // iframe is also non-void: `<iframe/>` does not close it in HTML.
            // Its fallback text is never scanned for nested candidates.
            const auto close =
                findHtmlRawClose(html, cursor, QStringLiteral("iframe"));
            if (!close)
            {
                return {};
            }
            cursor = close->second;
        }

        if (const auto prefix = decisivePagePrefix(
                mode, titles, embedIds, challengeForm, bodyStarted, firstVideo))
        {
            return *prefix;
        }
    }

    if (titles.size() != 1 || titles.front().size() > 4096)
    {
        return {};
    }

    if (isInterstitialPageTitle(titles.front()) || challengeForm)
    {
        return {PageParse::State::Interstitial, titles.front(), {}, firstVideo};
    }
    if (firstVideo == PageParse::FirstVideoState::Malformed)
    {
        return {PageParse::State::Malformed, titles.front(), {}, firstVideo};
    }
    if (firstVideo == PageParse::FirstVideoState::Offline)
    {
        return {PageParse::State::Offline, titles.front(), {}, firstVideo};
    }

    if (embedIds.empty())
    {
        return {firstVideo == PageParse::FirstVideoState::Live
                    ? PageParse::State::Malformed
                    : PageParse::State::Offline,
                titles.front(),
                {},
                firstVideo};
    }
    if (embedIds.size() != 1)
    {
        return {PageParse::State::Malformed, titles.front(), {}, firstVideo};
    }
    return {PageParse::State::Live, titles.front(), *embedIds.begin(),
            firstVideo};
}

struct EmbedParse {
    enum class State {
        Live,
        Offline,
        Malformed,
    };

    State state = State::Malformed;
    QString streamId;
    QString title;
    QString channelIdentity;
    QString channelTitle;
};

struct TopLevelMemberToken {
    bool valid = false;
    bool found = false;
    QByteArray token;
};

bool isJsonWhitespace(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

void skipJsonWhitespace(const QByteArray &body, qsizetype &cursor)
{
    while (cursor < body.size() && isJsonWhitespace(body[cursor]))
    {
        ++cursor;
    }
}

std::optional<QString> decodeJsonStringToken(const QByteArray &token)
{
    QByteArray wrapped("[");
    wrapped += token;
    wrapped += ']';
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(wrapped, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
    {
        return std::nullopt;
    }
    const auto values = document.array();
    if (values.size() != 1 || !values.at(0).isString())
    {
        return std::nullopt;
    }
    return values.at(0).toString();
}

TopLevelMemberToken topLevelMemberToken(const QByteArray &body,
                                        const QString &field)
{
    qsizetype cursor = 0;
    skipJsonWhitespace(body, cursor);
    if (cursor >= body.size() || body[cursor] != '{')
    {
        return {};
    }
    ++cursor;

    TopLevelMemberToken result;
    std::set<QString> seenKeys;
    while (true)
    {
        skipJsonWhitespace(body, cursor);
        if (cursor >= body.size())
        {
            return {};
        }
        if (body[cursor] == '}')
        {
            ++cursor;
            skipJsonWhitespace(body, cursor);
            result.valid = cursor == body.size();
            return result.valid ? result : TopLevelMemberToken{};
        }
        if (body[cursor] != '"')
        {
            return {};
        }

        const auto keyStart = cursor++;
        bool escaped = false;
        while (cursor < body.size())
        {
            const auto byte = body[cursor++];
            if (escaped)
            {
                escaped = false;
            }
            else if (byte == '\\')
            {
                escaped = true;
            }
            else if (byte == '"')
            {
                break;
            }
        }
        if (cursor > body.size() || body[cursor - 1] != '"')
        {
            return {};
        }
        const auto key =
            decodeJsonStringToken(body.mid(keyStart, cursor - keyStart));
        if (!key)
        {
            return {};
        }
        if (!seenKeys.insert(*key).second)
        {
            return {};
        }

        skipJsonWhitespace(body, cursor);
        if (cursor >= body.size() || body[cursor] != ':')
        {
            return {};
        }
        ++cursor;
        skipJsonWhitespace(body, cursor);
        const auto valueStart = cursor;
        int nested = 0;
        bool inString = false;
        escaped = false;
        while (cursor < body.size())
        {
            const auto byte = body[cursor];
            if (inString)
            {
                ++cursor;
                if (escaped)
                {
                    escaped = false;
                }
                else if (byte == '\\')
                {
                    escaped = true;
                }
                else if (byte == '"')
                {
                    inString = false;
                }
                continue;
            }
            if (byte == '"')
            {
                inString = true;
                ++cursor;
                continue;
            }
            if (byte == '{' || byte == '[')
            {
                ++nested;
                ++cursor;
                continue;
            }
            if (byte == '}' || byte == ']')
            {
                if (nested > 0)
                {
                    --nested;
                    ++cursor;
                    continue;
                }
                if (byte == '}')
                {
                    break;
                }
                return {};
            }
            if (byte == ',' && nested == 0)
            {
                break;
            }
            ++cursor;
        }
        if (inString || nested != 0 || cursor >= body.size())
        {
            return {};
        }

        auto valueEnd = cursor;
        while (valueEnd > valueStart && isJsonWhitespace(body[valueEnd - 1]))
        {
            --valueEnd;
        }
        if (valueEnd == valueStart)
        {
            return {};
        }
        if (*key == field)
        {
            if (result.found)
            {
                return {};
            }
            result.found = true;
            result.token = body.mid(valueStart, valueEnd - valueStart);
        }

        if (body[cursor] == ',')
        {
            ++cursor;
            continue;
        }
        if (body[cursor] != '}')
        {
            return {};
        }
    }
}

std::optional<QString> losslessJsonId(const QByteArray &body,
                                      const QJsonValue &value,
                                      const QString &field)
{
    const auto member = topLevelMemberToken(body, field);
    if (!member.valid || !member.found)
    {
        return std::nullopt;
    }
    if (value.isString())
    {
        const auto id = value.toString();
        return isDecimalId(id) ? std::optional<QString>(id) : std::nullopt;
    }
    if (!value.isDouble())
    {
        return std::nullopt;
    }

    const auto id = QString::fromLatin1(member.token);
    if (!isDecimalId(id))
    {
        return std::nullopt;
    }
    return id;
}

EmbedParse parseEmbed(const QByteArray &body)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return {};
    }
    const auto object = document.object();
    const auto structure = topLevelMemberToken(body, QStringLiteral("vid"));
    if (!structure.valid)
    {
        return {};
    }
    if (!structure.found || !object.contains(QStringLiteral("vid")))
    {
        return {EmbedParse::State::Offline, {}, {}, {}, {}};
    }

    const auto vid = object.value(QStringLiteral("vid"));
    if (vid.isNull())
    {
        return {EmbedParse::State::Offline, {}, {}, {}, {}};
    }

    const auto streamId = losslessJsonId(body, vid, QStringLiteral("vid"));
    const auto title = object.value(QStringLiteral("title"));
    if (!streamId || !title.isString() ||
        title.toString().trimmed().isEmpty() || title.toString().size() > 4096)
    {
        return {};
    }

    EmbedParse parsed{
        .state = EmbedParse::State::Live,
        .streamId = *streamId,
        .title = title.toString().trimmed(),
    };

    const auto channelId = object.value(QStringLiteral("channel_id"));
    if (!channelId.isUndefined() && !channelId.isNull())
    {
        const auto lossless =
            losslessJsonId(body, channelId, QStringLiteral("channel_id"));
        if (!lossless)
        {
            return {};
        }
        parsed.channelIdentity = *lossless;
    }

    const auto channelTitle = object.value(QStringLiteral("channel_title"));
    if (!channelTitle.isUndefined())
    {
        if (!channelTitle.isString())
        {
            return {};
        }
        const auto rawChannelTitle = channelTitle.toString();
        parsed.channelTitle = rawChannelTitle.trimmed();
        if (rawChannelTitle.size() > 4096 || parsed.channelTitle.isEmpty() ||
            parsed.channelTitle.size() > 4096)
        {
            return {};
        }
    }
    return parsed;
}

QByteArray headerValue(const ResponseHead &head, const QByteArray &name)
{
    QByteArray found;
    int count = 0;
    for (const auto &header : head.headers)
    {
        if (header.name.compare(name, Qt::CaseInsensitive) == 0)
        {
            found = header.value;
            ++count;
        }
    }
    return count == 1 ? found : QByteArray{};
}

bool eventStreamMediaTypeMatches(const QByteArray &value)
{
    const auto parts = value.trimmed().toLower().split(';');
    if (parts.empty() ||
        parts.front().trimmed() != QByteArrayLiteral("text/event-stream"))
    {
        return false;
    }
    if (parts.size() == 1)
    {
        return true;
    }
    if (parts.size() > 3)
    {
        return false;
    }
    return std::ranges::all_of(
        parts.cbegin() + 1, parts.cend(), [](const QByteArray &parameter) {
            return parameter.trimmed() == QByteArrayLiteral("charset=utf-8");
        });
}

bool mediaTypeMatches(const ResponseHead &head, ExpectedMediaType expected)
{
    if (head.status == 204)
    {
        return true;
    }
    const auto value = headerValue(head, QByteArrayLiteral("Content-Type"));
    if (value.isEmpty())
    {
        return false;
    }
    if (expected == ExpectedMediaType::EventStream)
    {
        return eventStreamMediaTypeMatches(value);
    }
    const auto normalized = value.trimmed().toLower();
    const auto semicolon = normalized.indexOf(';');
    const auto essence =
        (semicolon < 0 ? normalized : normalized.left(semicolon)).trimmed();
    if (expected == ExpectedMediaType::Html)
    {
        return essence == QByteArrayLiteral("text/html");
    }
    return essence == QByteArrayLiteral("application/json") ||
           (essence.startsWith("application/") && essence.endsWith("+json"));
}

bool headersWithinLimits(const ResponseHead &head,
                         const TransportRequest &request)
{
    if (static_cast<int>(head.headers.size()) > request.maxHeaders)
    {
        return false;
    }
    qsizetype bytes = 0;
    for (const auto &header : head.headers)
    {
        if (header.name.isEmpty() || header.name.contains('\0') ||
            header.name.contains('\r') || header.name.contains('\n') ||
            header.value.contains('\0') || header.value.contains('\r') ||
            header.value.contains('\n'))
        {
            return false;
        }
        if (bytes > request.maxHeaderBytes ||
            header.name.size() > request.maxHeaderBytes - bytes)
        {
            return false;
        }
        const auto remaining =
            request.maxHeaderBytes - bytes - header.name.size();
        if (remaining < 4 || header.value.size() > remaining - 4)
        {
            return false;
        }
        bytes += header.name.size() + header.value.size() + 4;
    }
    return true;
}

struct SseParseBatch {
    qsizetype consumedBytes = 0;
    std::size_t frameCount = 0;
    std::vector<QByteArray> records;
};

enum class SseParseFailure {
    Malformed,
    EventSizeLimit,
    EventCountLimit,
};

struct SseParseResult {
    SseParseBatch batch;
    std::optional<SseParseFailure> failure;
};

bool hasCompleteSseBoundary(const QByteArray &body, qsizetype newBytesStart)
{
    // A CRLF/CR/LF blank-line delimiter is at most four raw bytes. Inspecting
    // the new suffix keeps adversarial one-byte chunking linear.
    const auto start = std::max<qsizetype>(0, newBytesStart - 3);
    auto suffix = body.mid(start);
    suffix.replace("\r\n", "\n");
    suffix.replace('\r', '\n');
    return suffix.contains("\n\n");
}

SseParseResult parseSse(const QByteArray &body, qsizetype start,
                        std::size_t remainingFrames)
{
    if (start < 0 || start > body.size() || body.indexOf('\0', start) >= 0)
    {
        return {.failure = SseParseFailure::Malformed};
    }

    SseParseBatch parsed{.consumedBytes = start};
    qsizetype frameStart = start;
    qsizetype lineStart = start;
    qsizetype cursor = start;
    while (cursor < body.size())
    {
        if (body[cursor] != '\r' && body[cursor] != '\n')
        {
            ++cursor;
            continue;
        }

        auto next = cursor + 1;
        if (body[cursor] == '\r' && next < body.size() && body[next] == '\n')
        {
            ++next;
        }
        if (cursor != lineStart)
        {
            lineStart = next;
            cursor = next;
            continue;
        }

        // This empty line terminates a complete raw SSE block. Keep an
        // unterminated tail for the next transport chunk.
        auto block = body.mid(frameStart, lineStart - frameStart);
        parsed.consumedBytes = next;
        frameStart = next;
        lineStart = next;
        cursor = next;

        block.replace("\r\n", "\n");
        block.replace('\r', '\n');
        if (block.trimmed().isEmpty())
        {
            continue;
        }

        QByteArray eventName;
        QByteArray data;
        bool hasData = false;
        const auto lines = block.split('\n');
        for (const auto &line : lines)
        {
            if (line.startsWith(':'))
            {
                continue;
            }
            const auto colon = line.indexOf(':');
            const auto field = colon < 0 ? line : line.left(colon);
            auto value = colon < 0 ? QByteArray{} : line.mid(colon + 1);
            if (value.startsWith(' '))
            {
                value.remove(0, 1);
            }
            if (field == "event")
            {
                if (eventName.isEmpty())
                {
                    eventName = value;
                }
                else if (eventName != value)
                {
                    return {.failure = SseParseFailure::Malformed};
                }
            }
            else if (field == "data")
            {
                if (hasData)
                {
                    data.append('\n');
                }
                data.append(value);
                hasData = true;
            }
        }

        if (!hasData)
        {
            continue;
        }
        if (data.size() > MAX_SSE_EVENT_BYTES)
        {
            return {.failure = SseParseFailure::EventSizeLimit};
        }
        if (parsed.frameCount >= remainingFrames)
        {
            return {.failure = SseParseFailure::EventCountLimit};
        }
        ++parsed.frameCount;

        const auto decodedEventName = QString::fromUtf8(eventName);
        if (decodedEventName.contains(QChar::ReplacementCharacter))
        {
            return {.failure = SseParseFailure::Malformed};
        }
        parsed.records.push_back(std::move(data));
    }
    return {.batch = std::move(parsed)};
}

Outcome outcomeForFailure(TransportFailure failure)
{
    switch (failure)
    {
        case TransportFailure::Timeout:
            return Outcome::Timeout;
        case TransportFailure::Cancelled:
        case TransportFailure::OwnerDestroyed:
            return Outcome::Cancelled;
        case TransportFailure::RedirectRejected:
            return Outcome::RedirectRejected;
        case TransportFailure::InvalidMediaType:
            return Outcome::InvalidMediaType;
        case TransportFailure::BodyLimit:
        case TransportFailure::HeaderLimit:
            return Outcome::LimitExceeded;
        case TransportFailure::Network:
            return Outcome::TransportError;
    }
    return Outcome::TransportError;
}

QString bodyLimitCode(ExpectedMediaType mediaType)
{
    switch (mediaType)
    {
        case ExpectedMediaType::Html:
            return QStringLiteral("page_body_limit");
        case ExpectedMediaType::Json:
            return QStringLiteral("embed_body_limit");
        case ExpectedMediaType::EventStream:
            return QStringLiteral("sse_body_limit");
    }
    return QStringLiteral("body_limit");
}

QString codeForFailure(TransportFailure failure, ExpectedMediaType mediaType)
{
    switch (failure)
    {
        case TransportFailure::Timeout:
            return QStringLiteral("transport_timeout");
        case TransportFailure::Cancelled:
            return QStringLiteral("transport_cancelled");
        case TransportFailure::OwnerDestroyed:
            return QStringLiteral("transport_owner_destroyed");
        case TransportFailure::RedirectRejected:
            return QStringLiteral("redirect_rejected");
        case TransportFailure::InvalidMediaType:
            return QStringLiteral("invalid_media_type");
        case TransportFailure::BodyLimit:
            return bodyLimitCode(mediaType);
        case TransportFailure::HeaderLimit:
            return QStringLiteral("header_limit");
        case TransportFailure::Network:
            return QStringLiteral("transport_failure");
    }
    return QStringLiteral("transport_failure");
}

RetryMetadata retryForFailure(TransportFailure failure)
{
    return {
        .retryable = failure == TransportFailure::Network ||
                     failure == TransportFailure::Timeout,
    };
}

TransportRequest pageRequest(const QUrl &url)
{
    return {
        .url = url,
        .headers =
            {
                {QByteArrayLiteral("Accept"), QByteArrayLiteral("text/html")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::Html,
        .maxBodyBytes = MAX_PAGE_BYTES,
    };
}

TransportRequest embedRequest(const QString &embedId)
{
    QUrl url(QStringLiteral("https://rumble.com/embedJS/u3/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("request"), QStringLiteral("video"));
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("v"), embedId);
    url.setQuery(query);
    return {
        .url = url,
        .headers =
            {
                {QByteArrayLiteral("Accept"),
                 QByteArrayLiteral("application/json")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::Json,
        .maxBodyBytes = MAX_EMBED_BYTES,
    };
}

TransportRequest emoteCatalogRequest(const QString &streamId)
{
    QUrl url(QStringLiteral("https://rumble.com/service.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("name"), QStringLiteral("emote.list"));
    query.addQueryItem(QStringLiteral("chat_id"), streamId);
    url.setQuery(query);
    return {
        .url = url,
        .headers =
            {
                {QByteArrayLiteral("Accept"),
                 QByteArrayLiteral("application/json")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::Json,
        .maxBodyBytes = MAX_EMOTE_CATALOG_BYTES,
    };
}

TransportRequest sseRequest(const QString &streamId)
{
    return {
        .url = QUrl(
            QStringLiteral("https://web7.rumble.com/chat/api/chat/%1/stream")
                .arg(streamId)),
        .headers =
            {
                {QByteArrayLiteral("Accept"),
                 QByteArrayLiteral("text/event-stream")},
                {QByteArrayLiteral("Cache-Control"),
                 QByteArrayLiteral("no-cache")},
                {QByteArrayLiteral("Origin"),
                 QByteArrayLiteral("https://rumble.com")},
                {QByteArrayLiteral("Referer"),
                 QByteArrayLiteral("https://rumble.com/")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::EventStream,
        .maxBodyBytes = MAX_SSE_PENDING_BYTES,
        .timeoutMs = 20 * 1000,
        .deadlineScope = DeadlineScope::UntilFinalHead,
        .bodyLimitScope = BodyLimitScope::PendingDelivery,
        .maxPendingBodyChunks = 64,
    };
}

}  // namespace

namespace detail {

struct ApiOperation {
    enum class Kind {
        Resolve,
        Bootstrap,
        EmoteCatalog,
        Stream,
    };

    Kind kind = Kind::Resolve;
    Transport *transport = nullptr;
    RumbleApi::Defer defer;
    RumbleApi::Clock clock;
    std::weak_ptr<bool> apiAlive;

    bool cancelled = false;
    bool terminalQueued = false;
    bool delivered = false;
    std::uint64_t generation = 0;
    bool requestFinished = false;

    std::unique_ptr<TransportHandle> transportHandle;
    std::unique_ptr<Cancellation> wrappedCancellation;
    std::weak_ptr<ApiOperation> bootstrapParent;
    std::optional<ResponseHead> head;
    QByteArray body;
    TransportRequest request;
    qsizetype sseConsumedBytes = 0;
    std::deque<StreamBatch> streamDeliveryQueue;
    std::optional<StreamTerminal> streamTerminal;
    bool streamPumpQueued = false;
    EventParser eventParser;
    std::vector<Diagnostic> sseDiagnostics;

    Locator locator;
    Metadata metadata;
    bool legacyFallback = false;

    RumbleApi::ResolveCallback resolveCallback;
    RumbleApi::BootstrapCallback bootstrapCallback;
    RumbleApi::EmoteCatalogCallback emoteCatalogCallback;
    StreamCallbacks streamCallbacks;

    void cancel() noexcept
    {
        if (this->cancelled || this->delivered)
        {
            return;
        }
        this->cancelled = true;
        if (this->transportHandle)
        {
            this->transportHandle->cancel();
            this->transportHandle.reset();
        }
        if (this->wrappedCancellation)
        {
            this->wrappedCancellation->cancel();
            this->wrappedCancellation.reset();
        }
        this->resolveCallback = {};
        this->bootstrapCallback = {};
        this->emoteCatalogCallback = {};
        this->streamCallbacks = {};
    }
};

}  // namespace detail

namespace {

struct SseConsumeResult {
    std::vector<Event> events;
    std::optional<SseParseFailure> failure;
};

Outcome outcomeForSseFailure(SseParseFailure failure)
{
    return failure == SseParseFailure::Malformed ? Outcome::MalformedSchema
                                                 : Outcome::LimitExceeded;
}

QString codeForSseFailure(SseParseFailure failure)
{
    switch (failure)
    {
        case SseParseFailure::Malformed:
            return QStringLiteral("sse_schema");
        case SseParseFailure::EventSizeLimit:
            return QStringLiteral("sse_event_size_limit");
        case SseParseFailure::EventCountLimit:
            return QStringLiteral("sse_event_count_limit");
    }
    return QStringLiteral("sse_schema");
}

SseConsumeResult consumeCompleteSse(
    const std::shared_ptr<detail::ApiOperation> &operation)
{
    const auto parsed =
        parseSse(operation->body, operation->sseConsumedBytes, MAX_SSE_EVENTS);
    if (parsed.failure)
    {
        return {.failure = parsed.failure};
    }
    operation->sseConsumedBytes = parsed.batch.consumedBytes;

    std::vector<Event> events;
    events.reserve(parsed.batch.records.size());
    for (const auto &record : parsed.batch.records)
    {
        auto result = operation->eventParser.parse(QByteArrayView(record));
        for (auto &diagnostic : result.diagnostics)
        {
            operation->sseDiagnostics.push_back(std::move(diagnostic));
        }
        if (result.event)
        {
            events.push_back(std::move(*result.event));
        }
    }
    if (operation->sseConsumedBytes > 0)
    {
        operation->body.remove(0, operation->sseConsumedBytes);
        operation->sseConsumedBytes = 0;
    }
    return {.events = std::move(events)};
}

void finishResolve(const std::shared_ptr<detail::ApiOperation> &operation,
                   ResolveResult result)
{
    if (!operation || operation->cancelled || operation->terminalQueued ||
        operation->delivered)
    {
        return;
    }
    operation->terminalQueued = true;
    if (operation->transportHandle)
    {
        if (operation->transportHandle->active())
        {
            operation->transportHandle->cancel();
        }
        operation->transportHandle.reset();
    }
    if (operation->wrappedCancellation)
    {
        operation->wrappedCancellation->cancel();
        operation->wrappedCancellation.reset();
    }

    const std::weak_ptr weak = operation;
    operation->defer([weak, result = std::move(result)]() mutable {
        const auto locked = weak.lock();
        const auto alive = locked ? locked->apiAlive.lock() : nullptr;
        if (!locked || !alive || !*alive || locked->cancelled ||
            locked->delivered)
        {
            return;
        }
        locked->delivered = true;
        auto callback = std::move(locked->resolveCallback);
        locked->transportHandle.reset();
        if (callback)
        {
            callback(std::move(result));
        }
    });
}

void finishBootstrap(const std::shared_ptr<detail::ApiOperation> &operation,
                     BootstrapResult result)
{
    if (!operation || operation->cancelled || operation->terminalQueued ||
        operation->delivered)
    {
        return;
    }
    operation->terminalQueued = true;
    if (operation->transportHandle)
    {
        if (operation->transportHandle->active())
        {
            operation->transportHandle->cancel();
        }
        operation->transportHandle.reset();
    }
    if (operation->wrappedCancellation)
    {
        operation->wrappedCancellation->cancel();
        operation->wrappedCancellation.reset();
    }

    const std::weak_ptr weak = operation;
    operation->defer([weak, result = std::move(result)]() mutable {
        const auto locked = weak.lock();
        const auto alive = locked ? locked->apiAlive.lock() : nullptr;
        if (!locked || !alive || !*alive || locked->cancelled ||
            locked->delivered)
        {
            return;
        }
        locked->delivered = true;
        auto callback = std::move(locked->bootstrapCallback);
        locked->transportHandle.reset();
        if (callback)
        {
            callback(std::move(result));
        }
    });
}

void finishEmoteCatalog(const std::shared_ptr<detail::ApiOperation> &operation,
                        EmoteCatalogResult result)
{
    if (!operation || operation->cancelled || operation->terminalQueued ||
        operation->delivered)
    {
        return;
    }
    operation->terminalQueued = true;
    if (operation->transportHandle)
    {
        if (operation->transportHandle->active())
        {
            operation->transportHandle->cancel();
        }
        operation->transportHandle.reset();
    }

    const std::weak_ptr weak = operation;
    operation->defer([weak, result = std::move(result)]() mutable {
        const auto locked = weak.lock();
        const auto alive = locked ? locked->apiAlive.lock() : nullptr;
        if (!locked || !alive || !*alive || locked->cancelled ||
            locked->delivered)
        {
            return;
        }
        locked->delivered = true;
        auto callback = std::move(locked->emoteCatalogCallback);
        locked->transportHandle.reset();
        if (callback)
        {
            callback(std::move(result));
        }
    });
}

void queueStreamPump(const std::shared_ptr<detail::ApiOperation> &operation)
{
    if (!operation || operation->streamPumpQueued || operation->cancelled ||
        operation->delivered)
    {
        return;
    }
    operation->streamPumpQueued = true;
    const std::weak_ptr weak = operation;
    operation->defer([weak] {
        const auto locked = weak.lock();
        const auto alive = locked ? locked->apiAlive.lock() : nullptr;
        if (!locked)
            return;
        locked->streamPumpQueued = false;
        if (!alive || !*alive || locked->cancelled || locked->delivered)
        {
            locked->streamDeliveryQueue.clear();
            return;
        }

        while (!locked->streamDeliveryQueue.empty())
        {
            auto batch = std::move(locked->streamDeliveryQueue.front());
            locked->streamDeliveryQueue.pop_front();
            const auto callback = locked->streamCallbacks.onEvents;
            callback(std::move(batch));
            const auto stillAlive = locked->apiAlive.lock();
            if (!stillAlive || !*stillAlive || locked->cancelled ||
                locked->delivered)
            {
                locked->streamDeliveryQueue.clear();
                return;
            }
        }
        if (locked->streamTerminal)
        {
            auto terminal = std::move(*locked->streamTerminal);
            locked->streamTerminal.reset();
            locked->delivered = true;
            auto callbacks = std::move(locked->streamCallbacks);
            locked->transportHandle.reset();
            if (callbacks.onTerminal)
                callbacks.onTerminal(std::move(terminal));
        }
    });
}

bool deliverBootstrapBatch(const std::shared_ptr<detail::ApiOperation> &parent,
                           StreamBatch batch)
{
    if (!parent || parent->cancelled || parent->terminalQueued ||
        parent->delivered)
    {
        return false;
    }
    if (parent->sseDiagnostics.size() + batch.diagnostics.size() >
        MAX_SSE_EVENTS)
    {
        finishBootstrap(parent,
                        {
                            .outcome = Outcome::LimitExceeded,
                            .diagnostics = std::move(parent->sseDiagnostics),
                            .error =
                                Error{
                                    .outcome = Outcome::LimitExceeded,
                                    .code = QStringLiteral("diagnostic_limit"),
                                },
                        });
        return true;
    }
    for (auto &diagnostic : batch.diagnostics)
        parent->sseDiagnostics.push_back(std::move(diagnostic));
    if (!batch.events.empty())
    {
        finishBootstrap(parent,
                        {
                            .outcome = Outcome::ResolvedLive,
                            .events = std::move(batch.events),
                            .diagnostics = std::move(parent->sseDiagnostics),
                        });
    }
    return true;
}

bool queueStreamBatch(const std::shared_ptr<detail::ApiOperation> &operation,
                      StreamBatch batch)
{
    if (!operation || operation->cancelled || operation->terminalQueued ||
        operation->delivered || !operation->streamCallbacks.onEvents)
    {
        return false;
    }
    if (const auto parent = operation->bootstrapParent.lock();
        parent && !parent->cancelled && !parent->terminalQueued)
    {
        return deliverBootstrapBatch(parent, std::move(batch));
    }
    if (operation->streamDeliveryQueue.size() >= MAX_STREAM_HANDOFFS)
        return false;
    operation->streamDeliveryQueue.push_back(std::move(batch));
    queueStreamPump(operation);
    return true;
}

void finishStream(const std::shared_ptr<detail::ApiOperation> &operation,
                  StreamTerminal terminal)
{
    if (!operation || operation->cancelled || operation->terminalQueued ||
        operation->delivered)
    {
        return;
    }
    operation->terminalQueued = true;
    if (operation->transportHandle)
    {
        if (operation->transportHandle->active())
        {
            operation->transportHandle->cancel();
        }
        operation->transportHandle.reset();
    }

    operation->streamTerminal = std::move(terminal);
    queueStreamPump(operation);
}

Error makeError(Outcome outcome, QString code, int status = 0,
                RetryMetadata retry = RetryMetadata{})
{
    return {
        .outcome = outcome,
        .httpStatus = status,
        .retry = std::move(retry),
        .code = std::move(code),
    };
}

void finishFailure(const std::shared_ptr<detail::ApiOperation> &operation,
                   Outcome outcome, const QString &code, int status = 0,
                   RetryMetadata retry = RetryMetadata{})
{
    auto error = makeError(outcome, code, status, std::move(retry));
    if (operation->kind == detail::ApiOperation::Kind::Resolve)
    {
        finishResolve(operation, {
                                     .outcome = outcome,
                                     .locator = operation->locator,
                                     .error = std::move(error),
                                 });
    }
    else if (operation->kind == detail::ApiOperation::Kind::Bootstrap)
    {
        finishBootstrap(operation,
                        {
                            .outcome = outcome,
                            .diagnostics = std::move(operation->sseDiagnostics),
                            .error = std::move(error),
                        });
    }
    else if (operation->kind == detail::ApiOperation::Kind::EmoteCatalog)
    {
        finishEmoteCatalog(operation, {
                                          .outcome = outcome,
                                          .error = std::move(error),
                                      });
    }
    else
    {
        finishStream(operation,
                     {.outcome = outcome, .error = std::move(error)});
    }
}

RetryMetadata retryFor(const std::shared_ptr<detail::ApiOperation> &operation,
                       const ResponseHead &head, bool retryable)
{
    RetryMetadata retry{.retryable = retryable};
    const auto header = headerValue(head, QByteArrayLiteral("Retry-After"));
    if (!header.isEmpty())
    {
        try
        {
            retry.after =
                RumbleApi::parseRetryAfter(header, operation->clock());
        }
        catch (...)
        {
            // A caller-supplied clock cannot be allowed to break exactly-once
            // completion or surface exception text through this boundary.
        }
    }
    return retry;
}

bool mapHttpFailure(const std::shared_ptr<detail::ApiOperation> &operation,
                    const ResponseHead &head)
{
    if (head.status >= 200 && head.status < 300)
    {
        return false;
    }
    if (head.status == 429)
    {
        finishFailure(operation, Outcome::RateLimited,
                      QStringLiteral("http_rate_limited"), head.status,
                      retryFor(operation, head, true));
    }
    else if (head.status == 404)
    {
        finishFailure(operation, Outcome::NotFound,
                      QStringLiteral("http_not_found"), head.status);
    }
    else if (head.status == 401 || head.status == 403)
    {
        finishFailure(operation, Outcome::AccessInterstitial,
                      QStringLiteral("http_access_interstitial"), head.status);
    }
    else
    {
        const bool retryable =
            head.status == 408 || (head.status >= 500 && head.status <= 599);
        finishFailure(operation, Outcome::HttpError,
                      QStringLiteral("http_failure"), head.status,
                      retryFor(operation, head, retryable));
    }
    return true;
}

using RequestCompletion =
    std::function<void(const std::shared_ptr<detail::ApiOperation> &,
                       const ResponseHead &, const QByteArray &)>;
using RequestProgress = std::function<void(
    const std::shared_ptr<detail::ApiOperation> &, const ResponseHead &)>;

void startRequest(const std::shared_ptr<detail::ApiOperation> &operation,
                  TransportRequest request, RequestCompletion completion,
                  RequestProgress progress = RequestProgress{})
{
    if (!operation || operation->cancelled || operation->terminalQueued)
    {
        return;
    }

    const auto generation = ++operation->generation;
    operation->requestFinished = false;
    operation->head.reset();
    operation->body.clear();
    operation->request = request;
    operation->sseConsumedBytes = 0;
    operation->sseDiagnostics.clear();

    const std::weak_ptr weak = operation;
    TransportCallbacks callbacks;
    callbacks.onHead = [weak, generation, progress](const ResponseHead &head) {
        const auto locked = weak.lock();
        if (!locked || locked->cancelled || locked->terminalQueued ||
            locked->generation != generation || locked->requestFinished)
        {
            return;
        }
        if (locked->head || head.status < 100 || head.status > 599)
        {
            locked->requestFinished = true;
            finishFailure(locked, Outcome::TransportError,
                          QStringLiteral("invalid_response_head"));
            return;
        }
        if (!headersWithinLimits(head, locked->request))
        {
            locked->requestFinished = true;
            finishFailure(locked, Outcome::LimitExceeded,
                          QStringLiteral("header_limit"));
            return;
        }
        const bool successfulStatus = head.status >= 200 && head.status < 300;
        if (successfulStatus &&
            !mediaTypeMatches(head, locked->request.expectedMediaType))
        {
            locked->requestFinished = true;
            finishFailure(locked, Outcome::InvalidMediaType,
                          QStringLiteral("invalid_media_type"), head.status);
            return;
        }
        locked->head = head;
        if (progress && !locked->body.isEmpty() &&
            (locked->request.expectedMediaType !=
                 ExpectedMediaType::EventStream ||
             locked->body.contains('\0') ||
             locked->body.size() > MAX_SSE_EVENT_BYTES ||
             hasCompleteSseBoundary(locked->body, 0)))
        {
            progress(locked, head);
        }
    };
    callbacks.onBodyChunk = [weak, generation,
                             progress](const QByteArray &chunk) {
        const auto locked = weak.lock();
        if (!locked || locked->cancelled || locked->terminalQueued ||
            locked->generation != generation || locked->requestFinished)
        {
            return;
        }
        const bool exceedsBodyLimit =
            locked->request.bodyLimitScope == BodyLimitScope::Cumulative
                ? chunk.size() >
                      locked->request.maxBodyBytes - locked->body.size()
                : chunk.size() > locked->request.maxBodyBytes;
        if (exceedsBodyLimit)
        {
            locked->requestFinished = true;
            finishFailure(locked, Outcome::LimitExceeded,
                          bodyLimitCode(locked->request.expectedMediaType));
            return;
        }
        const auto previousSize = locked->body.size();
        locked->body.append(chunk);
        if (progress && locked->head &&
            (locked->request.expectedMediaType !=
                 ExpectedMediaType::EventStream ||
             chunk.contains('\0') ||
             locked->body.size() > MAX_SSE_EVENT_BYTES ||
             hasCompleteSseBoundary(locked->body, previousSize)))
        {
            progress(locked, *locked->head);
        }
    };
    callbacks.onComplete = [weak, generation,
                            completion = std::move(completion)]() mutable {
        const auto locked = weak.lock();
        if (!locked || locked->cancelled || locked->terminalQueued ||
            locked->generation != generation || locked->requestFinished)
        {
            return;
        }
        locked->requestFinished = true;
        if (!locked->head)
        {
            finishFailure(locked, Outcome::TransportError,
                          QStringLiteral("missing_response_head"));
            return;
        }
        completion(locked, *locked->head, locked->body);
    };
    callbacks.onFailure = [weak, generation](TransportFailure failure) {
        const auto locked = weak.lock();
        if (!locked || locked->cancelled || locked->terminalQueued ||
            locked->generation != generation || locked->requestFinished)
        {
            return;
        }
        locked->requestFinished = true;
        // Preserve every complete typed record which preceded a disconnect.
        // Bootstrap stops after its first batch; a persistent stream delivers
        // that batch and then reports the retryable terminal.
        if ((locked->kind == detail::ApiOperation::Kind::Bootstrap ||
             locked->kind == detail::ApiOperation::Kind::Stream) &&
            locked->request.expectedMediaType ==
                ExpectedMediaType::EventStream &&
            failure == TransportFailure::Network && locked->head &&
            locked->head->status == 200 && !locked->body.isEmpty())
        {
            auto consumed = consumeCompleteSse(locked);
            if (consumed.failure)
            {
                finishFailure(locked, outcomeForSseFailure(*consumed.failure),
                              codeForSseFailure(*consumed.failure));
                return;
            }
            if (!consumed.events.empty() &&
                locked->kind == detail::ApiOperation::Kind::Bootstrap)
            {
                finishBootstrap(
                    locked,
                    {
                        .outcome = Outcome::ResolvedLive,
                        .events = std::move(consumed.events),
                        .diagnostics = std::move(locked->sseDiagnostics),
                    });
                return;
            }
            if (locked->kind == detail::ApiOperation::Kind::Stream &&
                (!consumed.events.empty() || !locked->sseDiagnostics.empty()))
            {
                StreamBatch batch{
                    .events = std::move(consumed.events),
                    .diagnostics = std::move(locked->sseDiagnostics),
                };
                if (!queueStreamBatch(locked, std::move(batch)))
                {
                    finishFailure(locked, Outcome::LimitExceeded,
                                  QStringLiteral("stream_handoff_limit"));
                    return;
                }
            }
        }
        finishFailure(
            locked, outcomeForFailure(failure),
            codeForFailure(failure, locked->request.expectedMediaType), 0,
            retryForFailure(failure));
    };

    try
    {
        auto handle = operation->transport->start(std::move(request),
                                                  std::move(callbacks));
        if (!handle)
        {
            if (operation->generation != generation || operation->cancelled ||
                operation->terminalQueued || operation->requestFinished)
            {
                return;
            }
            operation->requestFinished = true;
            finishFailure(operation, Outcome::TransportError,
                          QStringLiteral("transport_handle_missing"));
            return;
        }

        // A synchronous completion may have started the next request before
        // this start() returns. Never overwrite that newer generation's handle.
        if (operation->generation != generation)
        {
            if (handle->active())
            {
                handle->cancel();
            }
            return;
        }

        operation->transportHandle = std::move(handle);
        if (operation->cancelled || operation->terminalQueued ||
            operation->requestFinished)
        {
            if (operation->transportHandle->active())
            {
                operation->transportHandle->cancel();
            }
            operation->transportHandle.reset();
        }
    }
    catch (...)
    {
        if (operation->generation != generation || operation->cancelled ||
            operation->terminalQueued || operation->requestFinished)
        {
            return;
        }
        operation->requestFinished = true;
        finishFailure(operation, Outcome::TransportError,
                      QStringLiteral("transport_start_failure"));
    }
}

void resolveEmbed(const std::shared_ptr<detail::ApiOperation> &operation,
                  const QString &embedId);

void handleParsedPage(const std::shared_ptr<detail::ApiOperation> &operation,
                      const ResponseHead &head, const PageParse &parsed)
{
    if (parsed.state == PageParse::State::Interstitial)
    {
        finishFailure(operation, Outcome::AccessInterstitial,
                      QStringLiteral("page_interstitial"), head.status);
        return;
    }
    if (parsed.state == PageParse::State::Malformed)
    {
        finishFailure(operation, Outcome::MalformedSchema,
                      QStringLiteral("page_schema"));
        return;
    }

    if (operation->locator.kind == LocatorKind::Channel)
    {
        if (operation->metadata.channelIdentity.isEmpty())
        {
            operation->metadata.channelIdentity = operation->locator.value;
        }
    }
    else
    {
        operation->metadata.videoTitle = parsed.title;
    }

    if (parsed.state == PageParse::State::Offline)
    {
        finishResolve(operation, {
                                     .outcome = Outcome::ValidOffline,
                                     .locator = operation->locator,
                                     .metadata = operation->metadata,
                                 });
        return;
    }

    operation->metadata.embedId = parsed.embedId;
    resolveEmbed(operation, parsed.embedId);
}

void handlePage(const std::shared_ptr<detail::ApiOperation> &operation,
                const ResponseHead &head, const QByteArray &body)
{
    if (mapHttpFailure(operation, head))
    {
        return;
    }
    if (head.status != 200)
    {
        finishFailure(operation, Outcome::MalformedSchema,
                      QStringLiteral("unexpected_page_status"), head.status);
        return;
    }

    handleParsedPage(operation, head, parsePage(body));
}

void handlePagePrefix(const std::shared_ptr<detail::ApiOperation> &operation,
                      const ResponseHead &head)
{
    if (head.status != 200)
    {
        return;
    }

    const auto parsed = parsePage(
        operation->body, operation->locator.kind == LocatorKind::Channel
                             ? PageParseMode::ChannelPrefix
                             : PageParseMode::VideoPrefix);
    if (parsed.state == PageParse::State::Interstitial)
    {
        handleParsedPage(operation, head, parsed);
        return;
    }

    if (operation->locator.kind == LocatorKind::Channel)
    {
        // Channel pages are authoritative only after the first video tile has
        // supplied its duration. A positive duration is enough to conclude
        // offline; a zero duration also needs the embed reference used to
        // enter the chat API.
        const bool offline =
            parsed.state == PageParse::State::Offline &&
            parsed.firstVideo == PageParse::FirstVideoState::Offline;
        const bool live = parsed.state == PageParse::State::Live &&
                          parsed.firstVideo == PageParse::FirstVideoState::Live;
        if (!offline && !live)
        {
            return;
        }
    }
    else if (parsed.state != PageParse::State::Live)
    {
        // A video page's embed metadata is the only decisive prefix. Its
        // offline state comes from the bounded embed JSON request.
        return;
    }

    handleParsedPage(operation, head, parsed);
}

void requestLegacyPage(const std::shared_ptr<detail::ApiOperation> &operation)
{
    operation->legacyFallback = true;
    const auto path = QStringLiteral("/user/%1/live/")
                          .arg(encodedSegment(operation->locator.value));
    startRequest(
        operation,
        pageRequest(QUrl(QStringLiteral("https://rumble.com") + path)),
        [](const auto &locked, const auto &head, const auto &body) {
            handlePage(locked, head, body);
        },
        [](const auto &locked, const auto &head) {
            handlePagePrefix(locked, head);
        });
}

void requestPrimaryChannelPage(
    const std::shared_ptr<detail::ApiOperation> &operation)
{
    const auto path = QStringLiteral("/c/%1/live/")
                          .arg(encodedSegment(operation->locator.value));
    startRequest(
        operation,
        pageRequest(QUrl(QStringLiteral("https://rumble.com") + path)),
        [](const auto &locked, const auto &head, const auto &body) {
            if (head.status == 404 && !locked->legacyFallback)
            {
                requestLegacyPage(locked);
                return;
            }
            handlePage(locked, head, body);
        },
        [](const auto &locked, const auto &head) {
            handlePagePrefix(locked, head);
        });
}

void resolveEmbed(const std::shared_ptr<detail::ApiOperation> &operation,
                  const QString &embedId)
{
    operation->metadata.embedId = embedId;
    startRequest(
        operation, embedRequest(embedId),
        [](const auto &locked, const auto &head, const auto &body) {
            if (mapHttpFailure(locked, head))
            {
                return;
            }
            if (head.status != 200)
            {
                finishFailure(locked, Outcome::MalformedSchema,
                              QStringLiteral("unexpected_embed_status"),
                              head.status);
                return;
            }

            const auto parsed = parseEmbed(body);
            if (parsed.state == EmbedParse::State::Malformed)
            {
                finishFailure(locked, Outcome::MalformedSchema,
                              QStringLiteral("embed_schema"));
                return;
            }
            if (parsed.state == EmbedParse::State::Offline)
            {
                finishResolve(locked, {
                                          .outcome = Outcome::ValidOffline,
                                          .locator = locked->locator,
                                          .metadata = locked->metadata,
                                      });
                return;
            }

            locked->metadata.streamId = parsed.streamId;
            locked->metadata.videoTitle = parsed.title;
            if (!parsed.channelIdentity.isEmpty())
            {
                locked->metadata.channelIdentity = parsed.channelIdentity;
            }
            if (!parsed.channelTitle.isEmpty())
            {
                locked->metadata.channelTitle = parsed.channelTitle;
            }
            finishResolve(locked, {
                                      .outcome = Outcome::ResolvedLive,
                                      .locator = locked->locator,
                                      .metadata = locked->metadata,
                                  });
        });
}

}  // namespace

Cancellation::Cancellation(std::shared_ptr<detail::ApiOperation> operation)
    : operation_(std::move(operation))
{
}

Cancellation::Cancellation(Cancellation &&other) noexcept
    : operation_(std::move(other.operation_))
{
}

Cancellation &Cancellation::operator=(Cancellation &&other) noexcept
{
    if (this != &other)
    {
        this->cancel();
        this->operation_ = std::move(other.operation_);
    }
    return *this;
}

Cancellation::~Cancellation()
{
    this->cancel();
}

void Cancellation::cancel() noexcept
{
    if (this->operation_)
    {
        this->operation_->cancel();
        this->operation_.reset();
    }
}

bool Cancellation::active() const noexcept
{
    return this->operation_ && !this->operation_->cancelled &&
           !this->operation_->delivered;
}

RumbleApi::RumbleApi(Transport &transport, Defer defer, Clock clock)
    : transport_(transport)
    , defer_(std::move(defer))
    , clock_(std::move(clock))
    , alive_(std::make_shared<bool>(true))
{
    if (!this->defer_)
    {
        throw std::invalid_argument("RumbleApi requires a deferred dispatcher");
    }
    if (!this->clock_)
    {
        this->clock_ = [] {
            return QDateTime::currentDateTimeUtc();
        };
    }
}

RumbleApi::~RumbleApi()
{
    *this->alive_ = false;
    for (auto &weak : this->operations_)
    {
        if (const auto operation = weak.lock())
        {
            operation->cancel();
        }
    }
    this->operations_.clear();
}

std::optional<Locator> RumbleApi::normalizeLocator(const QString &input)
{
    const auto trimmed = input.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 4096 ||
        trimmed.contains(QChar::Null))
    {
        return std::nullopt;
    }

    if (isDecimalId(trimmed))
    {
        return Locator{.kind = LocatorKind::Stream, .value = trimmed};
    }
    // A bare decimal-looking locator is always a stream-ID attempt. Do not
    // reinterpret invalid forms such as zero or a leading-zero ID as channel
    // slugs merely because the slug grammar also permits digits.
    if (isDecimalText(trimmed))
    {
        return std::nullopt;
    }
    if (isEmbedId(trimmed))
    {
        return Locator{.kind = LocatorKind::Video, .value = trimmed};
    }

    if (!trimmed.contains(QStringLiteral("://")))
    {
        if (!isChannelSlug(trimmed))
        {
            return std::nullopt;
        }
        return Locator{.kind = LocatorKind::Channel, .value = trimmed};
    }

    const auto encoded = trimmed.toUtf8();
    if (!hasValidPercentEncoding(encoded))
    {
        return std::nullopt;
    }

    // QUrl canonicalizes an explicit empty port (`rumble.com:`) away. Inspect
    // the source authority first so ports, userinfo, encoded hosts, and other
    // non-canonical spellings cannot disappear before policy validation.
    const auto schemeEnd = encoded.indexOf(QByteArrayLiteral("://"));
    const auto authorityStart = schemeEnd + 3;
    auto authorityEnd = encoded.size();
    for (const char delimiter : {'/', '?', '#'})
    {
        const auto position = encoded.indexOf(delimiter, authorityStart);
        if (position >= 0)
        {
            authorityEnd = std::min(authorityEnd, position);
        }
    }
    const auto rawAuthority =
        encoded.mid(authorityStart, authorityEnd - authorityStart);
    if (schemeEnd < 0 ||
        (rawAuthority.compare(QByteArrayLiteral("rumble.com"),
                              Qt::CaseInsensitive) != 0 &&
         rawAuthority.compare(QByteArrayLiteral("www.rumble.com"),
                              Qt::CaseInsensitive) != 0))
    {
        return std::nullopt;
    }
    const auto url = QUrl::fromEncoded(encoded, QUrl::StrictMode);
    const auto host = url.host().toLower();
    const auto authority = url.authority(QUrl::FullyEncoded);
    if (!url.isValid() || url.scheme().toLower() != QStringLiteral("https") ||
        (host != QStringLiteral("rumble.com") &&
         host != QStringLiteral("www.rumble.com")) ||
        !url.userInfo().isEmpty() || authority.contains(u'@') ||
        authority.contains(u':') || url.port(-1) != -1)
    {
        return std::nullopt;
    }

    const auto segments = decodedPathSegments(url);
    if (!segments)
    {
        return std::nullopt;
    }

    if (segments->size() == 3 &&
        (*segments)[0].compare(QStringLiteral("chat"), Qt::CaseInsensitive) ==
            0 &&
        (*segments)[1].compare(QStringLiteral("popup"), Qt::CaseInsensitive) ==
            0 &&
        isDecimalId((*segments)[2]))
    {
        return Locator{
            .kind = LocatorKind::Stream,
            .value = (*segments)[2],
        };
    }

    if (segments->size() == 2 &&
        (*segments)[0].compare(QStringLiteral("embed"), Qt::CaseInsensitive) ==
            0 &&
        isEmbedId((*segments)[1]))
    {
        return Locator{
            .kind = LocatorKind::Video,
            .value = (*segments)[1],
        };
    }

    if ((segments->size() == 2 || segments->size() == 3) &&
        ((*segments)[0].compare(QStringLiteral("c"), Qt::CaseInsensitive) ==
             0 ||
         (*segments)[0].compare(QStringLiteral("user"), Qt::CaseInsensitive) ==
             0) &&
        isChannelSlug((*segments)[1]) &&
        (segments->size() == 2 ||
         (*segments)[2].compare(QStringLiteral("live"), Qt::CaseInsensitive) ==
             0))
    {
        return Locator{
            .kind = LocatorKind::Channel,
            .value = (*segments)[1],
        };
    }

    if (segments->size() == 1)
    {
        static const QRegularExpression videoPage(
            QStringLiteral("^(v[a-z0-9]+)(?:-[^/]*)?\\.html$"));
        const auto match = videoPage.match((*segments)[0]);
        if (match.hasMatch() && isEmbedId(match.captured(1)))
        {
            return Locator{
                .kind = LocatorKind::VideoPage,
                .value = match.captured(1),
                .pagePath =
                    QStringLiteral("/") + encodedSegment((*segments)[0]),
            };
        }
    }
    return std::nullopt;
}

std::optional<std::chrono::seconds> RumbleApi::parseRetryAfter(
    const QByteArray &value, const QDateTime &now)
{
    if (!now.isValid() || value.size() > 128 || value.contains('\0') ||
        value.contains('\r') || value.contains('\n'))
    {
        return std::nullopt;
    }
    const auto trimmed = QString::fromLatin1(value).trimmed();
    static const QRegularExpression delta(QStringLiteral("^[0-9]{1,10}$"));
    if (delta.match(trimmed).hasMatch())
    {
        bool ok = false;
        const auto seconds = trimmed.toLongLong(&ok);
        if (ok && seconds <= std::numeric_limits<int>::max())
        {
            return std::chrono::seconds(seconds);
        }
        return std::nullopt;
    }

    static const QRegularExpression datePattern(
        QStringLiteral("^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), ([0-9]{2}) "
                       "(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) "
                       "([0-9]{4}) ([0-9]{2}):([0-9]{2}):([0-9]{2}) GMT$"));
    const auto match = datePattern.match(trimmed);
    if (!match.hasMatch())
    {
        return std::nullopt;
    }

    static const std::array weekdays = {
        QStringLiteral("Mon"), QStringLiteral("Tue"), QStringLiteral("Wed"),
        QStringLiteral("Thu"), QStringLiteral("Fri"), QStringLiteral("Sat"),
        QStringLiteral("Sun"),
    };
    static const std::array months = {
        QStringLiteral("Jan"), QStringLiteral("Feb"), QStringLiteral("Mar"),
        QStringLiteral("Apr"), QStringLiteral("May"), QStringLiteral("Jun"),
        QStringLiteral("Jul"), QStringLiteral("Aug"), QStringLiteral("Sep"),
        QStringLiteral("Oct"), QStringLiteral("Nov"), QStringLiteral("Dec"),
    };

    const auto monthIt = std::ranges::find(months, match.captured(3));
    const auto weekdayIt = std::ranges::find(weekdays, match.captured(1));
    if (monthIt == months.end() || weekdayIt == weekdays.end())
    {
        return std::nullopt;
    }

    const int month =
        static_cast<int>(std::distance(months.begin(), monthIt)) + 1;
    const int expectedWeekday =
        static_cast<int>(std::distance(weekdays.begin(), weekdayIt)) + 1;
    const QDate date(match.captured(4).toInt(), month,
                     match.captured(2).toInt());
    const QTime time(match.captured(5).toInt(), match.captured(6).toInt(),
                     match.captured(7).toInt());
    if (!date.isValid() || !time.isValid() ||
        date.dayOfWeek() != expectedWeekday)
    {
        return std::nullopt;
    }

    const QDateTime target(date, time, Qt::UTC);
    const auto seconds = std::max<qint64>(0, now.toUTC().secsTo(target));
    if (seconds > std::numeric_limits<int>::max())
    {
        return std::nullopt;
    }
    return std::chrono::seconds(seconds);
}

Cancellation RumbleApi::resolve(QString input, ResolveCallback callback)
{
    std::erase_if(this->operations_, [](const auto &operation) {
        return operation.expired();
    });
    auto operation = std::make_shared<detail::ApiOperation>();
    operation->kind = detail::ApiOperation::Kind::Resolve;
    operation->transport = &this->transport_;
    operation->defer = this->defer_;
    operation->clock = this->clock_;
    operation->apiAlive = this->alive_;
    operation->resolveCallback = std::move(callback);
    this->operations_.push_back(operation);

    const auto locator = normalizeLocator(input);
    if (!locator || !operation->resolveCallback)
    {
        operation->locator = locator.value_or(Locator{});
        finishFailure(operation, Outcome::UnsupportedInput,
                      QStringLiteral("unsupported_locator"));
        return Cancellation(operation);
    }

    operation->locator = *locator;
    if (locator->kind == LocatorKind::Stream)
    {
        operation->metadata.streamId = locator->value;
        finishResolve(operation, {
                                     .outcome = Outcome::ResolvedLive,
                                     .locator = *locator,
                                     .metadata = operation->metadata,
                                 });
    }
    else if (locator->kind == LocatorKind::Video)
    {
        operation->metadata.embedId = locator->value;
        resolveEmbed(operation, locator->value);
    }
    else if (locator->kind == LocatorKind::VideoPage)
    {
        startRequest(
            operation,
            pageRequest(
                QUrl(QStringLiteral("https://rumble.com") + locator->pagePath)),
            [](const auto &locked, const auto &head, const auto &body) {
                handlePage(locked, head, body);
            },
            [](const auto &locked, const auto &head) {
                handlePagePrefix(locked, head);
            });
    }
    else
    {
        operation->metadata.channelIdentity = locator->value;
        requestPrimaryChannelPage(operation);
    }

    return Cancellation(operation);
}

Cancellation RumbleApi::bootstrap(QString streamId, BootstrapCallback callback)
{
    std::erase_if(this->operations_, [](const auto &operation) {
        return operation.expired();
    });
    auto operation = std::make_shared<detail::ApiOperation>();
    operation->kind = detail::ApiOperation::Kind::Bootstrap;
    operation->transport = &this->transport_;
    operation->defer = this->defer_;
    operation->clock = this->clock_;
    operation->apiAlive = this->alive_;
    operation->bootstrapCallback = std::move(callback);
    this->operations_.push_back(operation);

    if (!isDecimalId(streamId) || !operation->bootstrapCallback)
    {
        finishFailure(operation, Outcome::UnsupportedInput,
                      QStringLiteral("unsupported_stream_id"));
        return Cancellation(operation);
    }

    const std::weak_ptr weak = operation;
    StreamCallbacks callbacks;
    callbacks.onEvents = [weak](StreamBatch batch) mutable {
        const auto locked = weak.lock();
        if (!locked || locked->cancelled || locked->terminalQueued ||
            batch.events.empty())
            return;
        finishBootstrap(locked, {
                                    .outcome = Outcome::ResolvedLive,
                                    .events = std::move(batch.events),
                                    .diagnostics = std::move(batch.diagnostics),
                                });
    };
    callbacks.onTerminal = [weak](StreamTerminal terminal) mutable {
        const auto locked = weak.lock();
        if (!locked || locked->cancelled || locked->terminalQueued)
            return;
        if (terminal.error &&
            terminal.error->code == QStringLiteral("stream_eof"))
        {
            finishBootstrap(
                locked, {
                            .outcome = Outcome::MalformedSchema,
                            .diagnostics = std::move(locked->sseDiagnostics),
                            .error =
                                Error{
                                    .outcome = Outcome::MalformedSchema,
                                    .code = QStringLiteral("sse_schema"),
                                },
                        });
            return;
        }
        finishBootstrap(locked,
                        {
                            .outcome = terminal.outcome,
                            .diagnostics = std::move(locked->sseDiagnostics),
                            .error = std::move(terminal.error),
                        });
    };
    auto child = this->stream(std::move(streamId), std::move(callbacks));
    auto childOperation = child.operation_;
    if (operation->cancelled || operation->terminalQueued || !childOperation)
        child.cancel();
    else
    {
        operation->wrappedCancellation =
            std::make_unique<Cancellation>(std::move(child));
        childOperation->bootstrapParent = operation;
        // A conforming transport may queue a typed batch and its terminal
        // before stream() returns. The child terminal guard still rejects new
        // batches, so replay batches it already accepted directly to the
        // wrapper parent. The first batch cancels the persistent reader while
        // the user callback itself remains deferred.
        while (!childOperation->streamDeliveryQueue.empty() &&
               !operation->terminalQueued)
        {
            auto batch = std::move(childOperation->streamDeliveryQueue.front());
            childOperation->streamDeliveryQueue.pop_front();
            std::ignore = deliverBootstrapBatch(operation, std::move(batch));
        }
    }

    return Cancellation(operation);
}

Cancellation RumbleApi::emoteCatalog(QString streamId,
                                     EmoteCatalogCallback callback)
{
    std::erase_if(this->operations_, [](const auto &operation) {
        return operation.expired();
    });
    auto operation = std::make_shared<detail::ApiOperation>();
    operation->kind = detail::ApiOperation::Kind::EmoteCatalog;
    operation->transport = &this->transport_;
    operation->defer = this->defer_;
    operation->clock = this->clock_;
    operation->apiAlive = this->alive_;
    operation->emoteCatalogCallback = std::move(callback);
    this->operations_.push_back(operation);

    if (!isDecimalId(streamId) || !operation->emoteCatalogCallback)
    {
        finishFailure(operation, Outcome::UnsupportedInput,
                      QStringLiteral("unsupported_stream_id"));
        return Cancellation(operation);
    }

    startRequest(
        operation, emoteCatalogRequest(streamId),
        [](const auto &locked, const auto &head, const auto &body) {
            if (mapHttpFailure(locked, head))
            {
                return;
            }
            if (head.status != 200)
            {
                finishFailure(locked, Outcome::MalformedSchema,
                              QStringLiteral("unexpected_emote_status"),
                              head.status);
                return;
            }
            auto parsed = parseEmoteCatalog(QByteArrayView(body));
            if (!parsed.catalog)
            {
                finishEmoteCatalog(
                    locked, {
                                .outcome = Outcome::MalformedSchema,
                                .diagnostics = std::move(parsed.diagnostics),
                                .error = makeError(
                                    Outcome::MalformedSchema,
                                    QStringLiteral("emote_catalog_schema")),
                            });
                return;
            }
            finishEmoteCatalog(locked,
                               {
                                   .outcome = Outcome::ResolvedLive,
                                   .catalog = std::move(parsed.catalog),
                                   .diagnostics = std::move(parsed.diagnostics),
                               });
        });

    return Cancellation(operation);
}

Cancellation RumbleApi::stream(QString streamId, StreamCallbacks callbacks)
{
    std::erase_if(this->operations_, [](const auto &operation) {
        return operation.expired();
    });
    auto operation = std::make_shared<detail::ApiOperation>();
    operation->kind = detail::ApiOperation::Kind::Stream;
    operation->transport = &this->transport_;
    operation->defer = this->defer_;
    operation->clock = this->clock_;
    operation->apiAlive = this->alive_;
    operation->streamCallbacks = std::move(callbacks);
    this->operations_.push_back(operation);

    if (!isDecimalId(streamId) || !operation->streamCallbacks.onEvents ||
        !operation->streamCallbacks.onTerminal)
    {
        finishFailure(operation, Outcome::UnsupportedInput,
                      QStringLiteral("unsupported_stream_id"));
        return Cancellation(operation);
    }

    const auto deliverAvailable = [](const auto &locked) {
        auto consumed = consumeCompleteSse(locked);
        if (consumed.failure)
        {
            finishFailure(locked, outcomeForSseFailure(*consumed.failure),
                          codeForSseFailure(*consumed.failure));
            return false;
        }
        if (locked->body.size() > MAX_SSE_EVENT_BYTES)
        {
            finishFailure(locked, Outcome::LimitExceeded,
                          QStringLiteral("sse_event_size_limit"));
            return false;
        }
        if (consumed.events.empty() && locked->sseDiagnostics.empty())
        {
            return true;
        }
        auto diagnostics = std::move(locked->sseDiagnostics);
        locked->sseDiagnostics.clear();
        StreamBatch batch{
            .events = std::move(consumed.events),
            .diagnostics = std::move(diagnostics),
        };
        if (!queueStreamBatch(locked, std::move(batch)))
        {
            finishFailure(locked, Outcome::LimitExceeded,
                          QStringLiteral("stream_handoff_limit"));
            return false;
        }
        return true;
    };

    startRequest(
        operation, sseRequest(streamId),
        [deliverAvailable](const auto &locked, const auto &head, const auto &) {
            if (head.status == 204)
            {
                finishStream(locked, {.outcome = Outcome::ValidOffline});
                return;
            }
            if (mapHttpFailure(locked, head))
            {
                return;
            }
            if (head.status != 200)
            {
                finishFailure(locked, Outcome::MalformedSchema,
                              QStringLiteral("unexpected_sse_status"),
                              head.status);
                return;
            }
            if (!deliverAvailable(locked))
            {
                return;
            }
            finishStream(locked, {
                                     .outcome = Outcome::TransportError,
                                     .error = makeError(
                                         Outcome::TransportError,
                                         QStringLiteral("stream_eof"), 0,
                                         RetryMetadata{.retryable = true}),
                                 });
        },
        [deliverAvailable](const auto &locked, const auto &head) {
            if (head.status == 200)
            {
                std::ignore = deliverAvailable(locked);
            }
        });

    return Cancellation(operation);
}

}  // namespace chatterino::rumble
