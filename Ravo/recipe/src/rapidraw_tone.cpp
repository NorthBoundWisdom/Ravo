#include "ravo/recipe/rapidraw_tone.h"

namespace ravo
{

Result<void> validate_rapidraw_basic_tone_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    if (parameters.size() != 1U)
    {
        return make_error(ErrorCode::kValidation,
                          "RapidRAW Basic tone parameters are incomplete or unknown",
                          {{"reason", "invalid_rapidraw_basic_tone_parameters"}});
    }
    const auto found = parameters.find("working_space");
    const auto *working_space = found == parameters.end() ?
                                    nullptr :
                                    std::get_if<std::string>(&found->second.value);
    if (working_space == nullptr || *working_space != kRapidRawBasicToneWorkingSpace)
    {
        return make_error(ErrorCode::kValidation,
                          "RapidRAW Basic tone working space is unsupported",
                          {{"reason", "invalid_rapidraw_basic_tone_parameters"}});
    }
    return {};
}

} // namespace ravo
