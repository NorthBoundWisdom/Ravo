#include "ravo/recipe/develop_patch.h"

#include <set>
#include "ravo/foundation/parse_number.h"
#include "ravo/recipe/local_adjustment.h"

namespace ravo
{
Result<DevelopParams> apply_develop_patch(const DevelopParams &base, const JsonValue &fields,
                                          const std::string_view local_id)
{
    const auto *items = fields.array_if();
    if (!items || items->empty() || items->size() > 256)
        return make_error(ErrorCode::kInvalidArgument,
                          "Develop patch requires 1 through 256 fields");
    DevelopParams next = base;
    if (!local_id.empty())
    {
        auto local = local_adjustment_develop(base, local_id);
        if (!local)
            return local.error();
        next = std::move(local).value();
    }
    std::set<std::string, std::less<>> names;
    for (const auto &item : *items)
    {
        const auto *object = item.object_if();
        const auto *name = item.find("name");
        const auto *value = item.find("value");
        if (!object || object->size() != 2 || !name || !name->string_if() ||
            name->string_if()->empty() || name->string_if()->size() > 256 || !value ||
            !names.insert(*name->string_if()).second)
            return make_error(ErrorCode::kInvalidArgument, "Invalid or duplicate Develop field",
                              {{"reason", "invalid_develop_field"}});
        Result<void> applied;
        if (const auto *text = value->string_if())
            applied = apply_develop_text_field_strict(next, *name->string_if(), *text);
        else if (const auto *number = value->number_if())
        {
            double parsed = 0;
            if (!parse_ascii_double(number->text, parsed))
                return make_error(ErrorCode::kInvalidArgument, "Develop value must be finite");
            applied = apply_develop_field_strict(next, *name->string_if(), parsed);
        }
        else
            return make_error(ErrorCode::kInvalidArgument, "Develop value must be numeric or text");
        if (!applied)
        {
            auto error = applied.error();
            error.context.insert_or_assign("reason", "invalid_develop_field");
            return error;
        }
    }
    if (local_id.empty())
        return next;
    auto result = base;
    auto attached = set_local_adjustment_develop(result, local_id, next);
    if (!attached)
        return attached.error();
    return result;
}
} // namespace ravo
