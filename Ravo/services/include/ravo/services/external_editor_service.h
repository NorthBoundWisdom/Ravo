#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/services/external_editor.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class ExportService;
class ImportService;
class LibraryService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class ExternalEditorService
{
public:
    ExternalEditorService(const ExternalEditorService &) = delete;
    ExternalEditorService &operator=(const ExternalEditorService &) = delete;
    ExternalEditorService(ExternalEditorService &&) = delete;
    ExternalEditorService &operator=(ExternalEditorService &&) = delete;

    [[nodiscard]] Result<ExternalEditorRegisterResult>
    register_external_editor_output(const ExternalEditorRegisterRequest &request);
    [[nodiscard]] Result<ExternalEditorProvenance>
    external_editor_provenance(std::string_view derived_asset_id) const;
    [[nodiscard]] Result<ExternalEditorOpenResult>
    prepare_external_editor_open(const ExternalEditorOpenRequest &request);
    [[nodiscard]] Result<ExternalEditorWorkingCopyResult>
    create_external_editor_working_copy(const ExternalEditorWorkingCopyRequest &request);
    [[nodiscard]] Result<ExternalEditorWorkingCopySession>
    external_editor_working_copy_session(std::string_view working_copy_id) const;
    [[nodiscard]] Result<ExternalEditorWorkingCopyStatus>
    external_editor_working_copy_status(std::string_view working_copy_id) const;
    [[nodiscard]] Result<std::vector<ExternalEditorWorkingCopySession>>
    list_external_editor_working_copies(
        std::optional<std::string_view> source_asset_id = std::nullopt) const;
    [[nodiscard]] Result<ExternalEditorAbandonResult>
    abandon_external_editor_working_copy(const ExternalEditorAbandonRequest &request);
    [[nodiscard]] Result<ExternalEditorReopenResult>
    reopen_external_editor_working_copy(const ExternalEditorReopenRequest &request);
    [[nodiscard]] Result<ExternalEditorCheckReturnedResult>
    check_external_editor_returned(const ExternalEditorCheckReturnedRequest &request);

private:
    friend class CatalogService;
    ExternalEditorService(const std::unique_ptr<CatalogRepository> &repository,
                          const std::unique_ptr<RasterDecoder> &raster,
                          const EngineFacade *const &engine, ExportService &exports_service,
                          ImportService &import_service, LibraryService &library_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const std::unique_ptr<RasterDecoder> &raster_;
    const EngineFacade *const &engine_;
    ExportService &exports_service_;
    ImportService &import_service_;
    LibraryService &library_service_;
};

} // namespace ravo
