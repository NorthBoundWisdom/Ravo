#include "ravo/adapters/xmp_adjacent_metadata.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QLatin1String>
#include <QtCore/QString>
#include <QtCore/QStringView>
#include <QtCore/QXmlStreamReader>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/foundation/json.h"

namespace ravo
{
namespace
{

constexpr QLatin1String kDcNs("http://purl.org/dc/elements/1.1/");
constexpr QLatin1String kPhotoshopNs("http://ns.adobe.com/photoshop/1.0/");
constexpr QLatin1String kIptcCoreNs("http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/");
constexpr QLatin1String kLrNs("http://ns.adobe.com/lightroom/1.0/");
constexpr QLatin1String kRdfNs("http://www.w3.org/1999/02/22-rdf-syntax-ns#");
constexpr QLatin1String kMwgKwNs("http://www.metadataworkinggroup.com/schemas/keywords/");

[[nodiscard]] std::string utf8(const QStringView value)
{
    return value.toString().toUtf8().toStdString();
}

[[nodiscard]] std::string utf8(const QString &value)
{
    return value.toUtf8().toStdString();
}

[[nodiscard]] std::string_view trim_view(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::string xml_text_escape(const std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char raw : value)
    {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out.push_back(static_cast<char>(ch));
            break;
        }
    }
    return out;
}

[[nodiscard]] JsonValue optional_string_json(const std::optional<std::string> &value)
{
    if (!value)
        return JsonValue{nullptr};
    return JsonValue{*value};
}

// Read children of the current start element as an RDF Bag/Seq/Alt of plain
// rdf:li text values. Nested elements inside li fail closed.
[[nodiscard]] Result<std::vector<std::string>> read_rdf_li_texts(QXmlStreamReader &reader)
{
    std::vector<std::string> items;
    int depth = 1;
    while (!reader.atEnd() && depth > 0)
    {
        reader.readNext();
        if (reader.isStartElement())
        {
            ++depth;
            if (depth == 2 && reader.namespaceUri() == kRdfNs &&
                (reader.name() == QLatin1String("Bag") || reader.name() == QLatin1String("Seq") ||
                 reader.name() == QLatin1String("Alt")))
            {
                continue;
            }
            if (reader.namespaceUri() == kRdfNs && reader.name() == QLatin1String("li") &&
                depth == 3)
            {
                // Consume li: reject nested start elements.
                QString text;
                int li_depth = 1;
                bool nested = false;
                while (!reader.atEnd() && li_depth > 0)
                {
                    reader.readNext();
                    if (reader.isStartElement())
                    {
                        ++li_depth;
                        nested = true;
                    }
                    else if (reader.isEndElement())
                    {
                        --li_depth;
                    }
                    else if (reader.isCharacters() && !nested && text.isEmpty())
                    {
                        text += reader.text().toString();
                    }
                }
                --depth; // li end already consumed
                if (nested)
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "Adjacent XMP keyword item is structured",
                                      {{"reason", "unsupported_hierarchical_keyword_shape"}});
                }
                items.push_back(utf8(text.trimmed()));
                continue;
            }
            if (depth >= 2)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Adjacent XMP container has an unsupported child",
                                  {{"reason", "unsupported_hierarchical_keyword_shape"},
                                   {"element", utf8(reader.qualifiedName())}});
            }
        }
        else if (reader.isEndElement())
        {
            --depth;
        }
        else if (reader.isCharacters() && depth == 1)
        {
            // Plain text content (no Bag) — treat as a single value when non-empty.
            const auto text = utf8(reader.text().toString().trimmed());
            if (!text.empty())
                items.push_back(text);
        }
    }
    if (reader.hasError())
    {
        return make_error(ErrorCode::kUnsupported, "Adjacent XMP container is malformed",
                          {{"reason", "invalid_xmp_adjacent_metadata"}});
    }
    return items;
}

[[nodiscard]] Result<std::string> read_text_or_lang_alt(QXmlStreamReader &reader)
{
    auto items = read_rdf_li_texts(reader);
    if (!items)
        return items.error();
    if (items.value().empty())
        return std::string{};
    return items.value().front();
}

[[nodiscard]] Result<std::vector<std::string>>
normalize_keyword_paths(const std::vector<std::string> &raw, const bool require_single_segment)
{
    std::vector<std::string> paths;
    paths.reserve(raw.size());
    for (const auto &item : raw)
    {
        const auto trimmed = std::string(trim_view(item));
        if (trimmed.empty())
            continue;
        if (require_single_segment && trimmed.find('|') != std::string::npos)
        {
            return make_error(
                ErrorCode::kUnsupported,
                "Flat dc:subject cannot carry hierarchical paths without lr:hierarchicalSubject",
                {{"reason", "unsupported_hierarchical_keyword_shape"}, {"path", trimmed}});
        }
        auto parsed = parse_keyword_path(trimmed);
        if (!parsed)
        {
            auto error = parsed.error();
            error.context.insert_or_assign("reason", "unsupported_hierarchical_keyword_shape");
            return error;
        }
        auto joined = join_keyword_path(parsed.value());
        if (!joined)
        {
            auto error = joined.error();
            error.context.insert_or_assign("reason", "unsupported_hierarchical_keyword_shape");
            return error;
        }
        paths.push_back(std::move(joined).value());
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

[[nodiscard]] std::string reason_of(const TaskError &error)
{
    const auto found = error.context.find("reason");
    return found == error.context.end() ? std::string{"invalid_xmp_adjacent_metadata"} :
                                          found->second;
}

} // namespace

std::string xmp_adjacent_metadata_fingerprint_sha256(const WritableMetadata &writable,
                                                     const std::vector<std::string> &keyword_paths)
{
    JsonValue::Object object;
    object.emplace("city", optional_string_json(writable.city));
    object.emplace("copyright", optional_string_json(writable.copyright));
    object.emplace("country", optional_string_json(writable.country));
    object.emplace("creator", optional_string_json(writable.creator));
    object.emplace("credit", optional_string_json(writable.credit));
    object.emplace("description", optional_string_json(writable.description));
    object.emplace("headline", optional_string_json(writable.headline));
    object.emplace("instructions", optional_string_json(writable.instructions));
    object.emplace("job_id", optional_string_json(writable.job_id));
    JsonValue::Array keywords;
    auto sorted = keyword_paths;
    std::sort(sorted.begin(), sorted.end());
    for (const auto &path : sorted)
        keywords.emplace_back(path);
    object.emplace("keywords", std::move(keywords));
    object.emplace("province_state", optional_string_json(writable.province_state));
    object.emplace("source", optional_string_json(writable.source));
    object.emplace("sublocation", optional_string_json(writable.sublocation));
    object.emplace("title", optional_string_json(writable.title));
    object.emplace("usage_terms", optional_string_json(writable.usage_terms));
    return sha256_utf8_hex(serialize_json(JsonValue{std::move(object)}));
}

bool xmp_adjacent_metadata_catalog_has_content(
    const WritableMetadata &writable, const std::vector<std::string> &keyword_paths) noexcept
{
    return writable != WritableMetadata{} || !keyword_paths.empty();
}

XmpAdjacentMetadataParseResult parse_xmp_adjacent_metadata(const std::string_view xmp_utf8)
{
    XmpAdjacentMetadataParseResult result;
    result.parse_ok = true;
    if (xmp_utf8.empty())
        return result;

    const QByteArray source(xmp_utf8.data(), static_cast<qsizetype>(xmp_utf8.size()));
    QXmlStreamReader reader(source);

    std::optional<std::vector<std::string>> hierarchical;
    std::optional<std::vector<std::string>> flat_subject;
    bool saw_mwg_keywords = false;

    auto fail = [&](const std::string_view reason)
    {
        result = {};
        result.parse_ok = false;
        result.parse_reason = std::string(reason);
    };

    auto assign_writable = [&](std::optional<std::string> &dest) -> bool
    {
        auto text = read_text_or_lang_alt(reader);
        if (!text)
        {
            fail(reason_of(text.error()));
            return false;
        }
        dest = std::move(text).value();
        result.metadata.has_any_writable_element = true;
        return true;
    };

    while (!reader.atEnd())
    {
        reader.readNext();
        if (!reader.isStartElement())
            continue;

        if (reader.namespaceUri() == kMwgKwNs || reader.name() == QLatin1String("KeywordInfo") ||
            reader.name() == QLatin1String("Hierarchy"))
        {
            saw_mwg_keywords = true;
            reader.skipCurrentElement();
            continue;
        }

        if (reader.name() == QLatin1String("Description") && reader.namespaceUri() == kRdfNs)
        {
            for (const auto &attribute : reader.attributes())
            {
                const auto ns = attribute.namespaceUri();
                const auto name = attribute.name();
                const auto value = utf8(attribute.value());
                auto set_field = [&](std::optional<std::string> &dest)
                {
                    dest = value;
                    result.metadata.has_any_writable_element = true;
                };
                if (ns == kPhotoshopNs && name == QLatin1String("Country"))
                    set_field(result.metadata.writable.country);
                else if (ns == kPhotoshopNs && name == QLatin1String("State"))
                    set_field(result.metadata.writable.province_state);
                else if (ns == kPhotoshopNs && name == QLatin1String("City"))
                    set_field(result.metadata.writable.city);
                else if (ns == kIptcCoreNs && name == QLatin1String("Location"))
                    set_field(result.metadata.writable.sublocation);
                else if (ns == kPhotoshopNs && name == QLatin1String("Headline"))
                    set_field(result.metadata.writable.headline);
                else if (ns == kPhotoshopNs && name == QLatin1String("Credit"))
                    set_field(result.metadata.writable.credit);
                else if (ns == kPhotoshopNs && name == QLatin1String("Source"))
                    set_field(result.metadata.writable.source);
                else if (ns == kPhotoshopNs && name == QLatin1String("Instructions"))
                    set_field(result.metadata.writable.instructions);
                else if (ns == kPhotoshopNs && name == QLatin1String("TransmissionReference"))
                    set_field(result.metadata.writable.job_id);
                else if (ns == QLatin1String("http://ns.adobe.com/xap/1.0/rights/") &&
                         name == QLatin1String("UsageTerms"))
                    set_field(result.metadata.writable.usage_terms);
            }
            continue;
        }

        if (reader.namespaceUri() == kDcNs && reader.name() == QLatin1String("title"))
        {
            if (!assign_writable(result.metadata.writable.title))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kDcNs && reader.name() == QLatin1String("description"))
        {
            if (!assign_writable(result.metadata.writable.description))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kDcNs && reader.name() == QLatin1String("creator"))
        {
            if (!assign_writable(result.metadata.writable.creator))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kDcNs && reader.name() == QLatin1String("rights"))
        {
            if (!assign_writable(result.metadata.writable.copyright))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs && reader.name() == QLatin1String("Country"))
        {
            if (!assign_writable(result.metadata.writable.country))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs && reader.name() == QLatin1String("State"))
        {
            if (!assign_writable(result.metadata.writable.province_state))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs && reader.name() == QLatin1String("City"))
        {
            if (!assign_writable(result.metadata.writable.city))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kIptcCoreNs && reader.name() == QLatin1String("Location"))
        {
            if (!assign_writable(result.metadata.writable.sublocation))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs && reader.name() == QLatin1String("Headline"))
        {
            if (!assign_writable(result.metadata.writable.headline))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs && reader.name() == QLatin1String("Credit"))
        {
            if (!assign_writable(result.metadata.writable.credit))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs && reader.name() == QLatin1String("Source"))
        {
            if (!assign_writable(result.metadata.writable.source))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs && reader.name() == QLatin1String("Instructions"))
        {
            if (!assign_writable(result.metadata.writable.instructions))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kPhotoshopNs &&
            reader.name() == QLatin1String("TransmissionReference"))
        {
            if (!assign_writable(result.metadata.writable.job_id))
                return result;
            continue;
        }
        if (reader.namespaceUri() == QLatin1String("http://ns.adobe.com/xap/1.0/rights/") &&
            reader.name() == QLatin1String("UsageTerms"))
        {
            if (!assign_writable(result.metadata.writable.usage_terms))
                return result;
            continue;
        }
        if (reader.namespaceUri() == kLrNs && reader.name() == QLatin1String("hierarchicalSubject"))
        {
            auto items = read_rdf_li_texts(reader);
            if (!items)
            {
                fail(reason_of(items.error()));
                return result;
            }
            hierarchical = std::move(items).value();
            continue;
        }
        if (reader.namespaceUri() == kDcNs && reader.name() == QLatin1String("subject"))
        {
            auto items = read_rdf_li_texts(reader);
            if (!items)
            {
                fail(reason_of(items.error()));
                return result;
            }
            flat_subject = std::move(items).value();
            continue;
        }
    }

    if (reader.hasError())
    {
        fail("invalid_xmp_adjacent_metadata");
        return result;
    }
    if (saw_mwg_keywords && !hierarchical)
    {
        fail("unsupported_hierarchical_keyword_shape");
        return result;
    }

    if (hierarchical)
    {
        auto paths = normalize_keyword_paths(*hierarchical, false);
        if (!paths)
        {
            fail(reason_of(paths.error()));
            return result;
        }
        result.metadata.keyword_paths = std::move(paths).value();
    }
    else if (flat_subject)
    {
        auto paths = normalize_keyword_paths(*flat_subject, true);
        if (!paths)
        {
            fail(reason_of(paths.error()));
            return result;
        }
        result.metadata.keyword_paths = std::move(paths).value();
    }

    return result;
}

Result<XmpAdjacentExportResult>
export_xmp_adjacent_interchange(const XmpAdjacentExportRequest &request)
{
    auto crs = export_crs_xmp({request.look, request.preset_name});
    if (!crs)
        return crs.error();

    XmpAdjacentExportResult result;
    result.omitted_catalog_fields = crs.value().omitted_catalog_fields;
    std::string xml = std::move(crs.value().xmp_utf8);

    const auto desc_open = xml.find("<rdf:Description");
    if (desc_open == std::string::npos)
    {
        return make_error(ErrorCode::kInternal, "CRS XMP export missing rdf:Description",
                          {{"reason", "xmp_adjacent_export_malformed_crs"}});
    }
    const auto desc_gt = xml.find('>', desc_open);
    if (desc_gt == std::string::npos)
    {
        return make_error(ErrorCode::kInternal, "CRS XMP export has unclosed rdf:Description",
                          {{"reason", "xmp_adjacent_export_malformed_crs"}});
    }
    const auto desc_close = xml.find("</rdf:Description>", desc_gt);
    if (desc_close == std::string::npos)
    {
        return make_error(ErrorCode::kInternal, "CRS XMP export missing rdf:Description end",
                          {{"reason", "xmp_adjacent_export_malformed_crs"}});
    }

    const std::string xmlns =
        "\n    xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
        "\n    xmlns:photoshop=\"http://ns.adobe.com/photoshop/1.0/\""
        "\n    xmlns:Iptc4xmpCore=\"http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/\""
        "\n    xmlns:xmpRights=\"http://ns.adobe.com/xap/1.0/rights/\""
        "\n    xmlns:lr=\"http://ns.adobe.com/lightroom/1.0/\"";

    std::ostringstream body;
    body.imbue(std::locale::classic());
    const auto append_lang_alt =
        [&](const std::string_view element, const std::optional<std::string> &value)
    {
        if (!value)
            return;
        body << "   <" << element << ">\n    <rdf:Alt>\n     <rdf:li xml:lang=\"x-default\">"
             << xml_text_escape(*value) << "</rdf:li>\n    </rdf:Alt>\n   </" << element << ">\n";
    };
    const auto append_simple =
        [&](const std::string_view element, const std::optional<std::string> &value)
    {
        if (!value)
            return;
        body << "   <" << element << ">" << xml_text_escape(*value) << "</" << element << ">\n";
    };

    append_lang_alt("dc:title", request.writable.title);
    append_lang_alt("dc:description", request.writable.description);
    if (request.writable.creator)
    {
        body << "   <dc:creator>\n    <rdf:Seq>\n     <rdf:li>"
             << xml_text_escape(*request.writable.creator)
             << "</rdf:li>\n    </rdf:Seq>\n   </dc:creator>\n";
    }
    append_lang_alt("dc:rights", request.writable.copyright);
    append_simple("photoshop:Country", request.writable.country);
    append_simple("photoshop:State", request.writable.province_state);
    append_simple("photoshop:City", request.writable.city);
    append_simple("Iptc4xmpCore:Location", request.writable.sublocation);
    append_simple("photoshop:Headline", request.writable.headline);
    append_simple("photoshop:Credit", request.writable.credit);
    append_simple("photoshop:Source", request.writable.source);
    append_simple("photoshop:Instructions", request.writable.instructions);
    append_simple("photoshop:TransmissionReference", request.writable.job_id);
    append_lang_alt("xmpRights:UsageTerms", request.writable.usage_terms);

    auto keywords = request.keyword_paths;
    std::sort(keywords.begin(), keywords.end());
    keywords.erase(std::unique(keywords.begin(), keywords.end()), keywords.end());
    if (!keywords.empty())
    {
        body << "   <lr:hierarchicalSubject>\n    <rdf:Bag>\n";
        for (const auto &path : keywords)
            body << "     <rdf:li>" << xml_text_escape(path) << "</rdf:li>\n";
        body << "    </rdf:Bag>\n   </lr:hierarchicalSubject>\n";
        body << "   <dc:subject>\n    <rdf:Bag>\n";
        for (const auto &path : keywords)
        {
            const auto sep = path.rfind('|');
            const auto leaf = sep == std::string::npos ? path : path.substr(sep + 1);
            body << "     <rdf:li>" << xml_text_escape(leaf) << "</rdf:li>\n";
        }
        body << "    </rdf:Bag>\n   </dc:subject>\n";
    }

    xml.insert(desc_close, body.str());
    xml.insert(desc_gt, xmlns);
    result.xmp_utf8 = std::move(xml);
    return result;
}

} // namespace ravo
