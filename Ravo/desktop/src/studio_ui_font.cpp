#include "studio_ui_font.h"

#include <QFontDatabase>
#include <QGuiApplication>

namespace ravo::desktop_internal
{
namespace
{
bool generic_font_family(const QString &family)
{
    const QString name = family.trimmed().toLower();
    return name.isEmpty() || name == QLatin1String("sans serif") ||
           name == QLatin1String("sans-serif") || name == QLatin1String("serif") ||
           name == QLatin1String("monospace") || name == QLatin1String("cursive") ||
           name == QLatin1String("fantasy") || name == QLatin1String("system-ui") ||
           name == QLatin1String("ui-sans-serif") || name == QLatin1String("ui-serif") ||
           name == QLatin1String("ui-monospace") || name == QLatin1String("ui-rounded") ||
           name == QLatin1String("sans");
}

QString installed_family(const QStringList &installed, const QString &family)
{
    if (generic_font_family(family))
    {
        return {};
    }
    for (const QString &have : installed)
    {
        if (have.compare(family, Qt::CaseInsensitive) == 0)
        {
            return have;
        }
    }
    return {};
}

void append_family(QStringList &families, const QString &family)
{
    if (!family.isEmpty() && !families.contains(family))
    {
        families.push_back(family);
    }
}

QString first_public_family(QFontDatabase::WritingSystem writing_system)
{
    for (const QString &family : QFontDatabase::families(writing_system))
    {
        if (!generic_font_family(family) && !QFontDatabase::isPrivateFamily(family))
        {
            return family;
        }
    }
    return {};
}

} // namespace

// Offscreen QPA and Qt generic families request "Sans Serif", which is not a
// CoreText face. Never look up missing names; keep the platform system font
// when it is real, then match fallbacks against QFontDatabase::families().
QStringList studio_ui_font_families(const QFont &system_font, const QString &language)
{
    const QStringList installed = QFontDatabase::families();
    QStringList families;

    auto append_installed = [&](const QString &family)
    { append_family(families, installed_family(installed, family)); };

    for (const QString &family : system_font.families())
    {
        if (generic_font_family(family))
        {
            continue;
        }
        const QString canonical = installed_family(installed, family);
        append_family(families, canonical.isEmpty() ? family : canonical);
    }
    if (families.isEmpty() && !generic_font_family(system_font.family()))
    {
        const QString canonical = installed_family(installed, system_font.family());
        append_family(families, canonical.isEmpty() ? system_font.family() : canonical);
    }

    if (families.isEmpty())
    {
        for (const auto &family :
             {QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans"),
              QStringLiteral("DejaVu Sans"), QStringLiteral("Liberation Sans"),
              QStringLiteral("Ubuntu"), QStringLiteral("Cantarell"), QStringLiteral("FreeSans"),
              QStringLiteral("Lucida Grande"), QStringLiteral("Helvetica Neue"),
              QStringLiteral("Helvetica"), QStringLiteral("Arial")})
        {
            append_installed(family);
            if (!families.isEmpty())
            {
                break;
            }
        }
    }
    if (families.isEmpty())
    {
        append_family(families, first_public_family(QFontDatabase::Latin));
    }

    auto append_script =
        [&](const QStringList &candidates, const QFontDatabase::WritingSystem writing_system)
    {
        const qsizetype before = families.size();
        for (const auto &family : candidates)
            append_installed(family);
        if (families.size() == before)
            append_family(families, first_public_family(writing_system));
    };
    auto append_simplified = [&]()
    {
        append_script({QStringLiteral("PingFang SC"), QStringLiteral("Hiragino Sans GB"),
                       QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans SC"),
                       QStringLiteral("Source Han Sans SC"), QStringLiteral("Microsoft YaHei UI"),
                       QStringLiteral("Microsoft YaHei")},
                      QFontDatabase::SimplifiedChinese);
    };
    auto append_traditional = [&]()
    {
        append_script({QStringLiteral("PingFang TC"), QStringLiteral("Hiragino Sans CNS"),
                       QStringLiteral("Noto Sans CJK TC"), QStringLiteral("Noto Sans TC"),
                       QStringLiteral("Microsoft JhengHei UI"),
                       QStringLiteral("Microsoft JhengHei")},
                      QFontDatabase::TraditionalChinese);
    };
    auto append_japanese = [&]()
    {
        append_script({QStringLiteral("Hiragino Sans"), QStringLiteral("Yu Gothic UI"),
                       QStringLiteral("Yu Gothic"), QStringLiteral("Meiryo"),
                       QStringLiteral("Noto Sans CJK JP"), QStringLiteral("Noto Sans JP")},
                      QFontDatabase::Japanese);
    };
    auto append_korean = [&]()
    {
        append_script({QStringLiteral("Apple SD Gothic Neo"), QStringLiteral("Malgun Gothic"),
                       QStringLiteral("Noto Sans CJK KR"), QStringLiteral("Noto Sans KR")},
                      QFontDatabase::Korean);
    };

    if (language == QLatin1String("zh_TW"))
    {
        append_traditional();
        append_simplified();
        append_japanese();
        append_korean();
    }
    else if (language == QLatin1String("ja_JP"))
    {
        append_japanese();
        append_simplified();
        append_traditional();
        append_korean();
    }
    else if (language == QLatin1String("ko_KR"))
    {
        append_korean();
        append_simplified();
        append_traditional();
        append_japanese();
    }
    else
    {
        append_simplified();
        append_traditional();
        append_japanese();
        append_korean();
    }
    return families;
}

void apply_studio_ui_font(const QString &language)
{
    QFont ui_font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const auto families = studio_ui_font_families(ui_font, language);
    if (!families.isEmpty())
    {
        ui_font.setFamilies(families);
        ui_font.setStyleHint(QFont::AnyStyle);
    }
    QGuiApplication::setFont(ui_font);
}
} // namespace ravo::desktop_internal
