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

namespace
{

QString history_field_label(const std::string_view field)
{
    if (field == "exposure")
        return QCoreApplication::translate("DevelopPanel", "Exposure");
    if (field == "exposureInstances")
        return QCoreApplication::translate("DevelopPanel", "Exposure instances");
    if (field == "colorBalanceRgbInstances")
        return QCoreApplication::translate("DevelopPanel", "Color Balance RGB instances");
    if (field == "masks")
        return QCoreApplication::translate("DevelopPanel", "Masks");
    if (field == "black")
        return QCoreApplication::translate("DevelopPanel", "Exposure black");
    if (field == "contrast")
        return QCoreApplication::translate("DevelopPanel", "Contrast");
    if (field == "highlights")
        return QCoreApplication::translate("DevelopPanel", "Highlights");
    if (field == "shadows")
        return QCoreApplication::translate("DevelopPanel", "Shadows");
    if (field == "whites")
        return QCoreApplication::translate("DevelopPanel", "Whites");
    if (field == "blacks")
        return QCoreApplication::translate("DevelopPanel", "Blacks");
    if (field == "vibrance")
        return QCoreApplication::translate("DevelopPanel", "Vibrance");
    if (field == "saturation")
        return QCoreApplication::translate("DevelopPanel", "Saturation");
    if (field == "velvia" || field == "velviaEnabled")
        return QCoreApplication::translate("DevelopPanel", "Velvia");
    if (field == "velviaStrength")
        return QCoreApplication::translate("DevelopPanel", "Velvia strength");
    if (field == "velviaBias")
        return QCoreApplication::translate("DevelopPanel", "Velvia mid-tones bias");
    if (field == "lut3d")
        return QCoreApplication::translate("DevelopPanel", "3D LUT");
    if (field == "gamma")
        return QCoreApplication::translate("DevelopPanel", "Gamma");
    if (field == "rgbLevels")
        return QCoreApplication::translate("DevelopPanel", "RGB levels");
    if (field == "rgbCurve")
        return QCoreApplication::translate("DevelopPanel", "RGB curve");
    if (field == "sharpen")
        return QCoreApplication::translate("DevelopPanel", "Sharpen");
    if (field == "texture")
        return QCoreApplication::translate("DevelopPanel", "Texture");
    if (field == "textureDetailThreshold")
        return QCoreApplication::translate("DevelopPanel", "Texture scale");
    if (field == "textureIterations")
        return QCoreApplication::translate("DevelopPanel", "Texture iterations");
    if (field == "retouch")
        return QCoreApplication::translate("DevelopPanel", "Retouch");
    if (field == "clarity")
        return QCoreApplication::translate("DevelopPanel", "Clarity");
    if (field == "vignette")
        return QCoreApplication::translate("DevelopPanel", "Vignette");
    if (field == "grain")
        return QCoreApplication::translate("DevelopPanel", "Grain");
    if (field == "bloom")
        return QCoreApplication::translate("DevelopPanel", "Bloom");
    if (field == "soften")
        return QCoreApplication::translate("DevelopPanel", "Soften");
    if (field == "dehaze")
        return QCoreApplication::translate("DevelopPanel", "Dehaze");
    if (field == "outputDither")
        return QCoreApplication::translate("DevelopPanel", "Output Dither");
    if (field == "outputFrame")
        return QCoreApplication::translate("DevelopPanel", "Output Frame");
    if (field == "watermark")
        return QCoreApplication::translate("DevelopPanel", "Watermark");
    if (field == "monochrome")
        return QCoreApplication::translate("DevelopPanel", "Monochrome");
    if (field == "denoise")
        return QCoreApplication::translate("DevelopPanel", "Denoise");
    if (field == "rawHighlights")
        return QCoreApplication::translate("DevelopPanel", "RAW highlights");
    if (field == "hotPixels")
        return QCoreApplication::translate("DevelopPanel", "Hot pixels");
    if (field == "rawChromaticAberration")
        return QCoreApplication::translate("DevelopPanel", "Chromatic aberration");
    if (field == "rawDenoise")
        return QCoreApplication::translate("DevelopPanel", "RAW denoise");
    if (field == "demosaic")
        return QCoreApplication::translate("DevelopPanel", "Demosaicing");
    if (field == "straighten")
        return QCoreApplication::translate("DevelopPanel", "Angle");
    if (field == "perspective")
        return QCoreApplication::translate("DevelopPanel", "Perspective");
    if (field == "toneEqBlacks")
        return QCoreApplication::translate("DevelopPanel", "Blacks");
    if (field == "toneEqShadows")
        return QCoreApplication::translate("DevelopPanel", "Shadows");
    if (field == "toneEqMidtones")
        return QCoreApplication::translate("DevelopPanel", "Midtones");
    if (field == "toneEqHighlights")
        return QCoreApplication::translate("DevelopPanel", "Highlights");
    if (field == "toneEqWhites")
        return QCoreApplication::translate("DevelopPanel", "Whites");
    if (field == "colorEqualizer")
        return QCoreApplication::translate("DevelopPanel", "Color Equalizer");
    if (field == "graduated")
        return QCoreApplication::translate("DevelopPanel", "Graduated ND");
    if (field == "rotate")
        return QCoreApplication::translate("DevelopPanel", "Rotate");
    if (field == "flip")
        return QCoreApplication::translate("DevelopPanel", "Flip");
    if (field == "crop")
        return QCoreApplication::translate("DevelopPanel", "Crop");
    if (field == "canvas")
        return QCoreApplication::translate("DevelopPanel", "Canvas");
    if (field == "lens")
        return QCoreApplication::translate("DevelopPanel", "Lens Correction");
    if (field == "toneCurve")
        return QCoreApplication::translate("DevelopPanel", "Tone curve");
    if (field == "whiteBalance")
        return QCoreApplication::translate("DevelopPanel", "White Balance");
    if (field == "inputProfile")
        return QCoreApplication::translate("DevelopPanel", "Input Profile");
    if (field == "outputProfile")
        return QCoreApplication::translate("DevelopPanel", "Output Profile");
    if (field == "primaries")
        return QCoreApplication::translate("DevelopPanel", "RGB Primaries");
    if (field == "mixer")
        return QCoreApplication::translate("DevelopPanel", "Channel Mixer");
    if (field == "calibration")
        return QCoreApplication::translate("DevelopPanel", "Camera Calibration");
    if (field == "colorBalance")
        return QCoreApplication::translate("DevelopPanel", "Color Balance");
    if (field == "colorChecker")
        return QCoreApplication::translate("DevelopPanel", "Color Checker");
    if (field == "colorBalanceRgb")
        return QCoreApplication::translate("DevelopPanel", "Color Balance RGB");
    if (field == "colorCorrection")
        return QCoreApplication::translate("DevelopPanel", "Color Correction");
    if (field == "colorContrast")
        return QCoreApplication::translate("DevelopPanel", "Color Contrast");
    if (field == "colorReconstruction")
        return QCoreApplication::translate("DevelopPanel", "Color Reconstruction");
    if (field == "colorZones")
        return QCoreApplication::translate("DevelopPanel", "Color Zones");
    if (field == "colorHarmonizer")
        return QCoreApplication::translate("DevelopPanel", "Color Harmonizer");
    if (field == "splitToning")
        return QCoreApplication::translate("DevelopPanel", "Split Toning");
    if (field == "profileGamma")
        return QCoreApplication::translate("DevelopPanel", "Unbreak input profile");
    if (field == "sigmoid")
        return QCoreApplication::translate("DevelopPanel", "Sigmoid");
    if (field == "light")
        return QCoreApplication::translate("DevelopPanel", "Light");
    if (field == "color")
        return QCoreApplication::translate("DevelopPanel", "Color");
    if (field == "detail")
        return QCoreApplication::translate("DevelopPanel", "Detail");
    if (field == "effects")
        return QCoreApplication::translate("DevelopPanel", "Effects");
    if (field == "geometry")
        return QCoreApplication::translate("DevelopPanel", "Geometry");
    if (field == "masks")
        return QCoreApplication::translate("DevelopPanel", "Masks");
    if (field.ends_with("SectionState"))
        return QCoreApplication::translate("DevelopPanel", "Section bypass state");
    if (field == "reset")
        return QCoreApplication::translate("StudioCommands", "Reset All Edits");
    return qstring_from_utf8(field);
}

QString preset_field_group(const std::string_view field)
{
    if (field == "whiteBalance" || field == "whiteBalanceSectionState")
        return QCoreApplication::translate("DevelopPanel", "White Balance");
    if (field == "profileGamma" || field == "inputProfile" || field == "inputProfileSectionState")
        return QCoreApplication::translate("DevelopPanel", "Input Profile");
    if (field == "outputProfile" || field == "outputProfileSectionState")
        return QCoreApplication::translate("DevelopPanel", "Output Profile");
    if (field == "primaries" || field == "calibration" || field == "primariesSectionState" ||
        field == "calibrationSectionState")
        return QCoreApplication::translate("DevelopPanel", "Camera Calibration");
    if (field == "exposure" || field == "contrast" || field == "highlights" || field == "shadows" ||
        field == "whites" || field == "blacks" || field == "gamma" || field == "rgbLevels" ||
        field == "sigmoid" || field == "toneEqual" || field == "lightSectionState" ||
        field == "toneEqualSectionState")
        return QCoreApplication::translate("DevelopPanel", "Light");
    if (field == "rgbCurve" || field == "toneCurve" || field == "curvesSectionState")
        return QCoreApplication::translate("DevelopPanel", "Curves");
    if (field == "vibrance" || field == "saturation" || field == "velvia" || field == "lut3d" ||
        field == "colorBalance" || field == "colorChecker" || field == "colorBalanceRgb" ||
        field == "colorCorrection" || field == "colorContrast" || field == "colorReconstruction" ||
        field == "colorZones" || field == "colorHarmonizer" || field == "monochrome" ||
        field == "splitToning" || field == "colorEqualizer" || field == "colorSectionState" ||
        field == "colorEqualizerSectionState")
        return QCoreApplication::translate("DevelopPanel", "Color");
    if (field == "sharpen" || field == "texture" || field == "retouch" || field == "clarity" ||
        field == "denoise" || field == "grain" || field == "detailSectionState")
        return QCoreApplication::translate("DevelopPanel", "Detail");
    if (field == "demosaic" || field == "rawHighlights" || field == "hotPixels" ||
        field == "rawChromaticAberration" || field == "rawDenoise" || field == "rawSectionState")
        return QCoreApplication::translate("DevelopPanel", "RAW");
    if (field == "rotate" || field == "flip" || field == "straighten" || field == "perspective" ||
        field == "crop" || field == "canvas" || field == "lens" || field == "geometrySectionState")
        return QCoreApplication::translate("DevelopPanel", "Geometry");
    if (field == "vignette" || field == "bloom" || field == "soften" || field == "dehaze" ||
        field == "outputDither" || field == "graduated" || field == "outputFrame" ||
        field == "watermark" || field == "effectsSectionState" || field == "graduatedSectionState")
        return QCoreApplication::translate("DevelopPanel", "Effects");
    return QCoreApplication::translate("DevelopPanel", "Other");
}

QString format_history_summary(const std::vector<DevelopChange> &changes)
{
    if (changes.empty())
    {
        return QCoreApplication::translate("DevelopHistoryPanel", "Original");
    }
    QStringList parts;
    constexpr int kMaxParts = 4;
    for (const auto &change : changes)
    {
        if (static_cast<int>(parts.size()) >= kMaxParts)
        {
            parts.push_back(QStringLiteral("…"));
            break;
        }
        QString part = history_field_label(change.field);
        if (change.value == "on")
        {
            part += QLatin1Char(' ') + QCoreApplication::translate("DevelopHistoryPanel", "on");
        }
        else if (change.value == "off")
        {
            part += QLatin1Char(' ') + QCoreApplication::translate("DevelopHistoryPanel", "off");
        }
        else if (!change.value.empty())
        {
            part += QLatin1Char(' ') + qstring_from_utf8(change.value);
        }
        parts.push_back(std::move(part));
    }
    return parts.join(QStringLiteral(", "));
}

DevelopParams develop_from_history_json(const std::string &recipe_json)
{
    if (recipe_json.empty())
    {
        return {};
    }
    auto recipe = parse_recipe_json(recipe_json);
    if (!recipe)
    {
        return {};
    }
    auto params = develop_from_recipe(recipe.value());
    if (!params)
    {
        return {};
    }
    return std::move(params).value();
}

} // namespace

DevelopParams StudioPresenter::baseline_develop() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset && is_raw_media_type(asset->media_type))
        return develop_raw_import_baseline();
    return {};
}

QVariantList StudioPresenter::modifiedParameterChoices() const
{
    QVariantList result;
    if (selected_asset_id_.isEmpty() || !develop_loaded_)
        return result;
    const auto changes = develop_modified_fields(baseline_develop(), develop_);
    result.reserve(static_cast<qsizetype>(changes.size()));
    for (const auto &change : changes)
    {
        result.push_back(QVariantMap{{QStringLiteral("field"), qstring_from_utf8(change.field)},
                                     {QStringLiteral("label"), history_field_label(change.field)},
                                     {QStringLiteral("group"), preset_field_group(change.field)}});
    }
    return result;
}

DevelopParams StudioPresenter::develop_from_history_entry(const RecipeHistoryEntry &entry) const
{
    if (entry.recipe_json.empty())
    {
        return baseline_develop();
    }
    return develop_from_history_json(entry.recipe_json);
}

void StudioPresenter::sync_active_history()
{
    if (develop_ == baseline_develop())
    {
        active_history_id_ = 0;
        active_history_seq_ = 0;
        return;
    }
    if (recipe_history_entries_.empty())
    {
        active_history_id_ = 0;
        active_history_seq_ = 0;
        return;
    }
    for (const auto &entry : recipe_history_entries_)
    {
        if (entry.id == active_history_id_)
        {
            active_history_seq_ = entry.seq;
            return;
        }
    }
    for (const auto &entry : recipe_history_entries_)
    {
        if (develop_from_history_entry(entry) == develop_)
        {
            active_history_id_ = entry.id;
            active_history_seq_ = entry.seq;
            return;
        }
    }
    active_history_id_ = recipe_history_entries_.front().id;
    active_history_seq_ = recipe_history_entries_.front().seq;
}

void StudioPresenter::apply_recipe_history(const std::vector<RecipeHistoryEntry> &entries)
{
    recipe_history_entries_ = entries;
    std::vector<DevelopParams> steps;
    steps.reserve(entries.size());
    for (const auto &entry : entries)
    {
        steps.push_back(develop_from_history_json(entry.recipe_json));
    }
    recipe_history_.clear();
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto &entry = entries[index];
        const DevelopParams &after = steps[index];
        const DevelopParams before = index + 1 < steps.size() ? steps[index + 1] : DevelopParams{};
        QString summary = format_history_summary(develop_change_summary(before, after));
        if (entry.kind == kRecipeHistoryKindSnapshot)
        {
            const QString label = entry.label ? qstring_from_utf8(*entry.label) : QString{};
            const QString snapshot = QCoreApplication::translate("DevelopHistoryPanel", "Snapshot");
            if (!label.isEmpty())
            {
                summary = snapshot + QStringLiteral(" · ") + label;
            }
            else
            {
                summary = snapshot + QStringLiteral(" · ") + summary;
            }
        }
        QVariantMap row;
        row.insert(QStringLiteral("id"), QVariant::fromValue(entry.id));
        row.insert(QStringLiteral("kind"), qstring_from_utf8(entry.kind));
        row.insert(QStringLiteral("label"),
                   entry.label ? qstring_from_utf8(*entry.label) : QString{});
        row.insert(QStringLiteral("seq"), QVariant::fromValue(entry.seq));
        row.insert(QStringLiteral("createdUnixMs"), QVariant::fromValue(entry.created_unix_ms));
        row.insert(QStringLiteral("summary"), summary);
        recipe_history_.push_back(row);
    }
}

void StudioPresenter::reload_recipe_history()
{
    if (selected_asset_id_.isEmpty())
    {
        recipe_history_.clear();
        recipe_history_entries_.clear();
        active_history_id_ = 0;
        active_history_seq_ = 0;
        emit editChanged();
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<std::vector<RecipeHistoryEntry>> history =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                history = service_->list_recipe_history(asset_id);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, history = std::move(history)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    if (history)
                    {
                        apply_recipe_history(history.value());
                    }
                    else
                    {
                        recipe_history_.clear();
                        recipe_history_entries_.clear();
                    }
                    sync_active_history();
                    emit editChanged();
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

bool StudioPresenter::cropToolActive() const noexcept
{
    return crop_tool_active_;
}

bool StudioPresenter::cropGuideReady() const noexcept
{
    return crop_guide_ready_;
}

} // namespace ravo
