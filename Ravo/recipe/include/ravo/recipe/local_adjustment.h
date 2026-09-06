#pragma once

#include "ravo/recipe/develop.h"

namespace ravo
{
struct LocalMaskPoint
{
    double x = 0;
    double y = 0;
};
[[nodiscard]] Result<void> add_local_mask_component(DevelopParams &local, std::int64_t kind,
                                                    std::int64_t combine);
[[nodiscard]] Result<void> author_local_mask_gesture(DevelopParams &local,
                                                     const std::vector<LocalMaskPoint> &points,
                                                     std::string_view handle, double source_aspect);

[[nodiscard]] bool local_adjustment_operation_allowed(std::string_view id) noexcept;
[[nodiscard]] Result<void> validate_local_adjustment(const OperationInstance &operation,
                                                     const OperationRegistry &registry);
[[nodiscard]] Result<DevelopParams> local_adjustment_develop(const DevelopParams &params,
                                                             std::string_view id);
[[nodiscard]] Result<void> set_local_adjustment_develop(DevelopParams &params, std::string_view id,
                                                        const DevelopParams &local);
[[nodiscard]] Result<std::string>
create_local_adjustment(DevelopParams &params, std::string_view id, std::int64_t mask_kind);
[[nodiscard]] Result<void> duplicate_local_adjustment(DevelopParams &params,
                                                      std::string_view source_id,
                                                      std::string_view new_id);
[[nodiscard]] Result<void> delete_local_adjustment(DevelopParams &params, std::string_view id);
[[nodiscard]] Result<void> insert_local_adjustments(Recipe &recipe, const DevelopParams &params);
[[nodiscard]] Result<std::optional<std::string>>
clone_develop_mask_subgraph(DevelopParams &params, const std::optional<std::string> &root,
                            std::string_view prefix);
[[nodiscard]] Result<std::optional<std::string>>
copy_develop_mask_subgraph(const DevelopParams &source, DevelopParams &destination,
                           const std::optional<std::string> &root, std::string_view prefix);
void collect_unreferenced_develop_mask(DevelopParams &params, std::string_view root,
                                       std::string_view prefix);
[[nodiscard]] Result<void> promote_legacy_local_adjustments(DevelopParams &params);

} // namespace ravo
