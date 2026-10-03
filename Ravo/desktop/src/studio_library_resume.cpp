#include "studio_library_resume.h"

#include <limits>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSettings>

#include "ravo/desktop/studio_presenter.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{
constexpr auto kRoot = "desktop/library-resume/v1/";
constexpr auto kLastCatalog = "desktop/library-resume/v1/lastCatalog";
QString canonical_path(const QString &path)
{
    const QFileInfo file(path);
    return file.exists() ? file.canonicalFilePath() : QDir::cleanPath(file.absoluteFilePath());
}
QString record_key(const QString &path)
{
    return QLatin1String(kRoot) + QStringLiteral("catalogs/") +
           QString::fromLatin1(
               QCryptographicHash::hash(canonical_path(path).toUtf8(), QCryptographicHash::Sha256)
                   .toHex());
}
TaskError invalid_position()
{
    return make_error(ErrorCode::kValidation, "Saved library view is invalid",
                      {{"reason", "invalid_library_resume"}});
}
Result<std::optional<StudioLibraryPosition>> load_position(const QString &path)
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    const auto value = settings.value(record_key(path));
    if (settings.status() != QSettings::NoError)
        return make_error(ErrorCode::kIo, "Cannot read saved library view");
    if (!value.isValid())
        return std::optional<StudioLibraryPosition>{};
    const auto bytes = value.toByteArray();
    if (bytes.isEmpty() || bytes.size() > 65536)
        return invalid_position();
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    const auto object = document.object();
    const auto mode = object.value("mode").toString();
    const auto asset = object.value("assetId").toString();
    bool count_valid = false;
    const auto count = object.value("lastImportCount").toString().toULongLong(&count_valid);
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        object.value("schema").toString() != QLatin1String("ravo.studio.library-resume/v1") ||
        object.value("catalog").toString() != canonical_path(path) ||
        !object.value("assetId").isString() || asset.size() > 180 ||
        !object.value("collapseStacks").isBool() || !object.value("query").isString() ||
        !object.value("lastImportSelected").isBool() || !count_valid ||
        count > std::numeric_limits<std::size_t>::max() ||
        (mode != QLatin1String("grid") && mode != QLatin1String("loupe") &&
         mode != QLatin1String("develop")))
        return invalid_position();
    auto query = parse_library_query_document(utf8_from_qstring(object.value("query").toString()));
    if (!query)
        return query.error();
    const auto last_import = object.value("lastImportSelected").toBool();
    if (last_import && (!query.value().imported_after_unix_ms ||
                        !query.value().imported_before_unix_ms || count == 0))
        return invalid_position();
    return std::optional<StudioLibraryPosition>{StudioLibraryPosition{
        asset, mode, std::move(query).value(), object.value("collapseStacks").toBool(), last_import,
        static_cast<std::size_t>(count)}};
}
Result<void> save_position(const QString &path, const StudioLibraryPosition &position)
{
    auto query = serialize_library_query_document(position.query);
    if (!query)
        return query.error();
    const auto catalog = canonical_path(path);
    if (catalog.isEmpty())
        return make_error(ErrorCode::kIo, "Cannot resolve library path for view persistence");
    const QJsonObject record{
        {"schema", "ravo.studio.library-resume/v1"},
        {"catalog", catalog},
        {"assetId", position.asset_id},
        {"mode", position.mode},
        {"collapseStacks", position.collapse_stacks},
        {"lastImportSelected", position.last_import_selected},
        {"lastImportCount", QString::number(static_cast<qulonglong>(position.last_import_count))},
        {"query", QString::fromStdString(query.value())}};
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (bytes.size() > 65536)
        return invalid_position();
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.setValue(record_key(catalog), bytes);
    settings.setValue(QLatin1String(kLastCatalog), catalog);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return make_error(ErrorCode::kIo, "Cannot save library view");
    return {};
}
} // namespace

Result<QString> last_studio_catalog()
{
    QSettings settings;
    settings.setFallbacksEnabled(false);
    const auto value = settings.value(QLatin1String(kLastCatalog));
    const auto path = value.toString();
    if (settings.status() != QSettings::NoError)
        return make_error(ErrorCode::kIo, "Cannot read last library path");
    if ((value.isValid() && value.metaType().id() != QMetaType::QString) || path.size() > 4096 ||
        (!path.isEmpty() && !QDir::isAbsolutePath(path)))
        return invalid_position();
    return path;
}

void StudioPresenter::initializeLibraryResume()
{
    library_resume_ = std::make_unique<StudioLibraryResume>();
    auto &timer = library_resume_->persist_timer;
    timer.setSingleShot(true);
    timer.setInterval(200);
    connect(&timer, &QTimer::timeout, this, &StudioPresenter::persistLibraryPosition);
    const auto changed = [this]
    {
        if (!library_resume_->suspended && !catalog_path_.isEmpty())
            library_resume_->persist_timer.start();
    };
    connect(this, &StudioPresenter::selectionChanged, this, changed);
    connect(this, &StudioPresenter::browseModeChanged, this, changed);
    connect(this, &StudioPresenter::filterChanged, this, changed);
}

void StudioPresenter::persistLibraryPosition()
{
    if (!library_resume_ || library_resume_->suspended || catalog_path_.isEmpty())
        return;
    library_resume_->persist_timer.stop();
    const auto mode =
        browse_mode_ == QLatin1String("survey") ? QStringLiteral("grid") : browse_mode_;
    StudioLibraryPosition position{selected_asset_id_, mode, current_query(), collapse_stacks_};
    position.last_import_selected = last_import_selected_;
    position.last_import_count = last_import_selected_ ? last_import_count_ : 0;
    auto saved = save_position(catalog_path_, position);
    if (!saved)
        setError(qstring_from_utf8(saved.error().message));
}

Result<LibraryQuery> StudioPresenter::beginLibraryResume(const QString &path, LibraryQuery query)
{
    persistLibraryPosition();
    auto position = load_position(path);
    if (!position)
        return position.error();
    library_resume_->persist_timer.stop();
    library_resume_->suspended = true;
    library_resume_->pending = std::move(position).value();
    library_resume_->initial_mode = catalog_path_.isEmpty() ? browse_mode_ : QStringLiteral("grid");
    if (!library_resume_->pending && !catalog_path_.isEmpty() &&
        canonical_path(path) != canonical_path(catalog_path_))
        query = LibraryQuery{};
    return library_resume_->pending ? library_resume_->pending->query : std::move(query);
}

std::optional<std::string> StudioPresenter::resumeAssetId() const
{
    if (!library_resume_->pending || library_resume_->pending->asset_id.isEmpty())
        return std::nullopt;
    return utf8_from_qstring(library_resume_->pending->asset_id);
}

bool StudioPresenter::resumeCollapseStacks() const
{
    return library_resume_->pending ? library_resume_->pending->collapse_stacks : collapse_stacks_;
}

void StudioPresenter::finishLibraryResume(const bool success)
{
    if (success)
    {
        const auto &position = library_resume_->pending;
        if (position && position->last_import_selected)
        {
            last_import_selected_ = true;
            last_import_count_ = position->last_import_count;
            last_import_after_unix_ms_ = position->query.imported_after_unix_ms;
            last_import_before_unix_ms_ = position->query.imported_before_unix_ms;
            emit folderChanged();
        }
        const auto id = position && assets_.indexOf(position->asset_id) >= 0 ? position->asset_id :
                                                                               assets_.assetIdAt(0);
        setBrowseMode(id.isEmpty() ? QStringLiteral("grid") :
                      position     ? position->mode :
                                     library_resume_->initial_mode);
        selectAsset(id);
    }
    library_resume_->pending.reset();
    library_resume_->suspended = false;
    if (success)
        persistLibraryPosition();
}
} // namespace ravo
