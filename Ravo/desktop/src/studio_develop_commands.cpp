#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>
#include <set>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "studio_debug_info.h"
#include "studio_qt.h"

namespace ravo
{

void StudioPresenter::setDevelopNumber(const QString &name, const double value)
{
    DevelopParams next = develop_;
    const auto field = utf8_from_qstring(name);
    capture_instance_front_for_field(next, field);
    if (is_develop_mask_field(field))
    {
        auto applied = apply_develop_field_strict(next, field, value);
        if (!applied)
        {
            const auto reason = applied.error().context.find("reason");
            const auto reason_text = reason == applied.error().context.end() ?
                                         QStringLiteral("unknown") :
                                         qstring_from_utf8(reason->second);
            setError(QCoreApplication::translate("DevelopPanel", "Mask edit was rejected") +
                     QStringLiteral(" [") + reason_text + QStringLiteral("]"));
            return;
        }
    }
    else if (!apply_develop_field(next, field, value))
    {
        return;
    }
    retarget_instance_edit_after_field(next, field);
    const bool keep_crop_guide =
        crop_tool_active_ && crop_guide_ready_ &&
        (name == QLatin1String("straighten") || name.startsWith(QLatin1String("perspective")));
    mutate_develop(std::move(next), DevelopEdit::Commit, !keep_crop_guide, field);
}

void StudioPresenter::setDevelopText(const QString &name, const QString &value)
{
    DevelopParams next = develop_;
    auto applied =
        apply_develop_text_field_strict(next, utf8_from_qstring(name), utf8_from_qstring(value));
    if (!applied)
    {
        setError(qstring_from_utf8(applied.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, utf8_from_qstring(name));
}

void StudioPresenter::saveStyleToPath(const QString &path)
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !engine_)
        return;
    QString output_path = path.trimmed();
    if (output_path.startsWith(QStringLiteral("file:")))
        output_path = QUrl(output_path).toLocalFile();
    if (!output_path.endsWith(QStringLiteral(".rstyle.json"), Qt::CaseInsensitive))
        output_path += QStringLiteral(".rstyle.json");
    if (output_path.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Style path must not be empty."));
        return;
    }
    auto recipe = recipe_from_develop(
        {asset->id, asset->normalized_uri, asset->content_fingerprint}, develop_);
    if (!recipe)
    {
        setError(qstring_from_utf8(recipe.error().message));
        return;
    }
    auto valid = engine_->validate(recipe.value());
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    QString style_name = QFileInfo(output_path).completeBaseName();
    if (style_name.endsWith(QStringLiteral(".rstyle"), Qt::CaseInsensitive))
        style_name.chop(7);
    auto style = recipe_style_from_recipe(utf8_from_qstring(style_name), {}, recipe.value());
    if (!style)
    {
        setError(qstring_from_utf8(style.error().message));
        return;
    }
    auto serialized = serialize_recipe_style(style.value());
    if (!serialized)
    {
        setError(qstring_from_utf8(serialized.error().message));
        return;
    }
    auto written =
        write_utf8_text_file_atomically(utf8_from_qstring(output_path), serialized.value());
    if (!written)
    {
        setError(qstring_from_utf8(written.error().message));
        return;
    }
    setStatus(QCoreApplication::translate("StudioPresenter", "Recipe style saved."));
}

void StudioPresenter::applyStyleFromPath(const QString &path)
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !engine_)
        return;
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    auto text = read_utf8_text_file(utf8_from_qstring(input_path), kRecipeStyleFileMaxBytes);
    if (!text)
    {
        setError(qstring_from_utf8(text.error().message));
        return;
    }
    if (is_crs_xmp_document(text.value()))
    {
        auto imported = import_crs_xmp(
            {text.value(), {asset->id, asset->normalized_uri, asset->content_fingerprint}});
        if (!imported)
        {
            setError(qstring_from_utf8(imported.error().message));
            return;
        }
        auto params = develop_;
        apply_crs_look(params, imported.value().look, imported.value().mask);
        mutate_develop(std::move(params), DevelopEdit::Commit);
        const auto name = imported.value().name.empty() ?
                              QString() :
                              QString::fromStdString(imported.value().name);
        setStatus(
            name.isEmpty() ?
                QCoreApplication::translate("StudioPresenter", "Lightroom preset applied.") :
                QCoreApplication::translate("StudioPresenter", "Lightroom preset “%1” applied.")
                    .arg(name));
        return;
    }
    auto style = parse_recipe_style_json(text.value());
    if (!style)
    {
        setError(qstring_from_utf8(style.error().message));
        return;
    }
    auto valid_template = engine_->validate(style.value().recipe);
    if (!valid_template)
    {
        setError(qstring_from_utf8(valid_template.error().message));
        return;
    }
    auto target_recipe = recipe_from_develop(
        {asset->id, asset->normalized_uri, asset->content_fingerprint}, develop_);
    if (!target_recipe)
    {
        setError(qstring_from_utf8(target_recipe.error().message));
        return;
    }
    auto recipe = apply_recipe_style(style.value(), std::move(target_recipe).value());
    if (!recipe)
    {
        setError(qstring_from_utf8(recipe.error().message));
        return;
    }
    auto valid = engine_->validate(recipe.value());
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    auto params = develop_from_recipe(recipe.value());
    if (!params)
    {
        setError(qstring_from_utf8(params.error().message));
        return;
    }
    mutate_develop(std::move(params).value(), DevelopEdit::Commit);
}

} // namespace ravo
