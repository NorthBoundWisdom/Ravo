#include "ravo/desktop/studio_presenter.h"

#include "studio_develop_internal.h"

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

namespace ravo::studio_develop_internal
{
namespace
{
constexpr double kDevelopMaskRadiusSoftMin = 0.01;
}

[[nodiscard]] QString develop_mask_field_prefix(const DevelopMaskTarget target)
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return QStringLiteral("colorHarmonizerMask");
    case DevelopMaskTarget::kGraduatedNd:
        return QStringLiteral("graduatedMask");
    case DevelopMaskTarget::kColorBalanceRgb:
        return QStringLiteral("colorBalanceRgbMask");
    case DevelopMaskTarget::kExposure:
        return QStringLiteral("exposureMask");
    }
    return QStringLiteral("colorHarmonizerMask");
}

[[nodiscard]] QString develop_mask_kind_label(const std::string_view name)
{
    if (name == "none")
        return QCoreApplication::translate("DevelopPanel", "None");
    if (name == "all")
        return QCoreApplication::translate("DevelopPanel", "All");
    if (name == "linear_gradient")
        return QCoreApplication::translate("DevelopPanel", "Linear gradient");
    if (name == "circle")
        return QCoreApplication::translate("DevelopPanel", "Circle");
    if (name == "ellipse")
        return QCoreApplication::translate("DevelopPanel", "Ellipse");
    if (name == "parametric")
        return QCoreApplication::translate("DevelopPanel", "Parametric");
    if (name == "group")
        return QCoreApplication::translate("DevelopPanel", "Group");
    if (name == "path")
        return QCoreApplication::translate("DevelopPanel", "Path");
    if (name == "brush")
        return QCoreApplication::translate("DevelopPanel", "Brush");
    return QCoreApplication::translate("DevelopPanel", "Unknown");
}

[[nodiscard]] QString develop_mask_status_label(const DevelopMaskAttachmentStatus status)
{
    switch (status)
    {
    case DevelopMaskAttachmentStatus::kNoMask:
        return QCoreApplication::translate("DevelopPanel", "No mask attached");
    case DevelopMaskAttachmentStatus::kEditable:
        return QCoreApplication::translate("DevelopPanel", "Mask is editable");
    case DevelopMaskAttachmentStatus::kExternalReadOnly:
        return QCoreApplication::translate("DevelopPanel", "External mask is read-only");
    case DevelopMaskAttachmentStatus::kSharedReadOnly:
        return QCoreApplication::translate("DevelopPanel", "Shared mask is read-only");
    case DevelopMaskAttachmentStatus::kGroupReadOnly:
        return QCoreApplication::translate("DevelopPanel", "Group mask is read-only");
    case DevelopMaskAttachmentStatus::kInvalid:
        return QCoreApplication::translate("DevelopPanel", "Mask attachment is invalid");
    }
    return QCoreApplication::translate("DevelopPanel", "Mask attachment is invalid");
}

[[nodiscard]] QVariantMap develop_mask_control(const QString &title, const QString &key,
                                               const QString &field, const double minimum,
                                               const double maximum, const double step,
                                               const double reset, const int decimals,
                                               const bool visible)
{
    return {{QStringLiteral("title"), title},    {QStringLiteral("key"), key},
            {QStringLiteral("field"), field},    {QStringLiteral("min"), minimum},
            {QStringLiteral("max"), maximum},    {QStringLiteral("step"), step},
            {QStringLiteral("reset"), reset},    {QStringLiteral("decimals"), decimals},
            {QStringLiteral("visible"), visible}};
}

[[nodiscard]] QVariantMap develop_mask_editor_map(const DevelopMaskEditorState &state,
                                                  const DevelopMaskTarget target)
{
    const auto prefix = develop_mask_field_prefix(target);
    const auto shape_kind = state.kind_index == 6 ? state.child_kind_index : state.kind_index;
    const auto kind_is = [shape_kind](const std::int64_t index) { return shape_kind == index; };
    const bool attached = state.attached;
    const double threshold0_min = kCanonicalMaskUnitMin;
    const double threshold0_max = state.threshold1;
    const double threshold1_min = state.threshold0;
    const double threshold1_max = state.threshold2;
    const double threshold2_min = state.threshold1;
    const double threshold2_max = state.threshold3;
    const double threshold3_min = state.threshold2;
    const double threshold3_max = kCanonicalMaskUnitMax;
    const double radius_min = std::min(kDevelopMaskRadiusSoftMin, state.radius);
    const double radius_x_min = std::min(kDevelopMaskRadiusSoftMin, state.radius_x);
    const double radius_y_min = std::min(kDevelopMaskRadiusSoftMin, state.radius_y);
    const QVariantList controls{
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Opacity"),
                             QStringLiteral("opacity"), prefix + QStringLiteral("Opacity"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 1.0, 2, attached),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Anchor X"),
                             QStringLiteral("anchorX"), prefix + QStringLiteral("AnchorX"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(2)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Anchor Y"),
                             QStringLiteral("anchorY"), prefix + QStringLiteral("AnchorY"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(2)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Rotation"),
                             QStringLiteral("rotationDegrees"),
                             prefix + QStringLiteral("RotationDegrees"), kCanonicalMaskAngleMin,
                             kCanonicalMaskAngleMax, 1.0, 0.0, 0, kind_is(2) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Transition"),
                             QStringLiteral("transition"), prefix + QStringLiteral("Transition"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.1, 2,
                             kind_is(2)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Center X"),
                             QStringLiteral("centerX"), prefix + QStringLiteral("CenterX"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(3) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Center Y"),
                             QStringLiteral("centerY"), prefix + QStringLiteral("CenterY"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(3) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Radius"),
                             QStringLiteral("radius"), prefix + QStringLiteral("Radius"),
                             radius_min, kCanonicalMaskUnitMax, 0.01, 0.25, 2, kind_is(3)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Radius X"),
                             QStringLiteral("radiusX"), prefix + QStringLiteral("RadiusX"),
                             radius_x_min, kCanonicalMaskUnitMax, 0.01, 0.25, 2, kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Radius Y"),
                             QStringLiteral("radiusY"), prefix + QStringLiteral("RadiusY"),
                             radius_y_min, kCanonicalMaskUnitMax, 0.01, 0.25, 2, kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Feather"),
                             QStringLiteral("feather"), prefix + QStringLiteral("Feather"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.0, 2,
                             kind_is(3) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 1"),
                             QStringLiteral("threshold0"), prefix + QStringLiteral("Threshold0"),
                             threshold0_min, threshold0_max, 0.01, 0.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 2"),
                             QStringLiteral("threshold1"), prefix + QStringLiteral("Threshold1"),
                             threshold1_min, threshold1_max, 0.01, 0.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 3"),
                             QStringLiteral("threshold2"), prefix + QStringLiteral("Threshold2"),
                             threshold2_min, threshold2_max, 0.01, 1.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 4"),
                             QStringLiteral("threshold3"), prefix + QStringLiteral("Threshold3"),
                             threshold3_min, threshold3_max, 0.01, 1.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Path feather"),
                             QStringLiteral("pathFeather"), prefix + QStringLiteral("PathFeather"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.05, 2,
                             kind_is(7)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point X"),
                             QStringLiteral("pointX"), prefix + QStringLiteral("PointX"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(7) || kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point Y"),
                             QStringLiteral("pointY"), prefix + QStringLiteral("PointY"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(7) || kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point radius"),
                             QStringLiteral("pointRadius"), prefix + QStringLiteral("PointRadius"),
                             radius_min, kCanonicalMaskUnitMax, 0.01, 0.05, 2, kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point hardness"),
                             QStringLiteral("pointHardness"),
                             prefix + QStringLiteral("PointHardness"), kCanonicalMaskUnitMin,
                             kCanonicalMaskUnitMax, 0.01, 0.5, 2, kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point density"),
                             QStringLiteral("pointDensity"),
                             prefix + QStringLiteral("PointDensity"), kCanonicalMaskUnitMin,
                             kCanonicalMaskUnitMax, 0.01, 1.0, 2, kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Child opacity"),
                             QStringLiteral("childOpacity"),
                             prefix + QStringLiteral("ChildOpacity"), kCanonicalMaskUnitMin,
                             kCanonicalMaskUnitMax, 0.01, 1.0, 2, state.kind_index == 6)};

    const QStringList kind_choices{develop_mask_kind_label("none"),
                                   develop_mask_kind_label("all"),
                                   develop_mask_kind_label("linear_gradient"),
                                   develop_mask_kind_label("circle"),
                                   develop_mask_kind_label("ellipse"),
                                   develop_mask_kind_label("parametric"),
                                   develop_mask_kind_label("group"),
                                   develop_mask_kind_label("path"),
                                   develop_mask_kind_label("brush")};
    const QStringList child_kind_choices{
        develop_mask_kind_label("all"),        develop_mask_kind_label("linear_gradient"),
        develop_mask_kind_label("circle"),     develop_mask_kind_label("ellipse"),
        develop_mask_kind_label("parametric"), develop_mask_kind_label("path"),
        develop_mask_kind_label("brush")};
    const QStringList operator_choices{QCoreApplication::translate("DevelopPanel", "Replace"),
                                       QCoreApplication::translate("DevelopPanel", "Union"),
                                       QCoreApplication::translate("DevelopPanel", "Intersection"),
                                       QCoreApplication::translate("DevelopPanel", "Difference"),
                                       QCoreApplication::translate("DevelopPanel", "Exclusion")};
    const QStringList source_choices{
        QCoreApplication::translate("DevelopPanel", "Input"),
        QCoreApplication::translate("DevelopPanel", "Operation output")};
    const QStringList channel_choices{QCoreApplication::translate("DevelopPanel", "Luminance"),
                                      QCoreApplication::translate("DevelopPanel", "Red"),
                                      QCoreApplication::translate("DevelopPanel", "Green"),
                                      QCoreApplication::translate("DevelopPanel", "Blue")};
    return {{QStringLiteral("target"), qstring_from_utf8(develop_mask_target_name(target))},
            {QStringLiteral("attached"), state.attached},
            {QStringLiteral("editable"), state.editable},
            {QStringLiteral("canDetach"), state.can_detach},
            {QStringLiteral("kindIndex"), static_cast<int>(state.kind_index)},
            {QStringLiteral("kindName"), qstring_from_utf8(state.kind_name)},
            {QStringLiteral("kindLabel"), develop_mask_kind_label(state.kind_name)},
            {QStringLiteral("kindChoices"), kind_choices},
            {QStringLiteral("status"), develop_mask_status_label(state.status)},
            {QStringLiteral("statusCode"),
             qstring_from_utf8(develop_mask_attachment_status_name(state.status))},
            {QStringLiteral("kindField"), prefix + QStringLiteral("Kind")},
            {QStringLiteral("detachField"), prefix},
            {QStringLiteral("invertedField"), prefix + QStringLiteral("Inverted")},
            {QStringLiteral("sourceField"), prefix + QStringLiteral("Source")},
            {QStringLiteral("channelField"), prefix + QStringLiteral("Channel")},
            {QStringLiteral("selectorsVisible"), kind_is(5)},
            {QStringLiteral("opacity"), state.opacity},
            {QStringLiteral("inverted"), state.inverted},
            {QStringLiteral("anchorX"), state.anchor_x},
            {QStringLiteral("anchorY"), state.anchor_y},
            {QStringLiteral("rotationDegrees"), state.rotation_degrees},
            {QStringLiteral("transition"), state.transition},
            {QStringLiteral("centerX"), state.center_x},
            {QStringLiteral("centerY"), state.center_y},
            {QStringLiteral("radius"), state.radius},
            {QStringLiteral("radiusX"), state.radius_x},
            {QStringLiteral("radiusY"), state.radius_y},
            {QStringLiteral("feather"), state.feather},
            {QStringLiteral("sourceIndex"), static_cast<int>(state.source_index)},
            {QStringLiteral("sourceChoices"), source_choices},
            {QStringLiteral("channelIndex"), static_cast<int>(state.channel_index)},
            {QStringLiteral("channelChoices"), channel_choices},
            {QStringLiteral("threshold0"), state.threshold0},
            {QStringLiteral("threshold1"), state.threshold1},
            {QStringLiteral("threshold2"), state.threshold2},
            {QStringLiteral("threshold3"), state.threshold3},
            {QStringLiteral("pathFeather"), state.path_feather},
            {QStringLiteral("pointX"), state.point_x},
            {QStringLiteral("pointY"), state.point_y},
            {QStringLiteral("pointRadius"), state.point_radius},
            {QStringLiteral("pointHardness"), state.point_hardness},
            {QStringLiteral("pointDensity"), state.point_density},
            {QStringLiteral("pointCount"), static_cast<int>(state.point_count)},
            {QStringLiteral("pointIndex"), static_cast<int>(state.point_index)},
            {QStringLiteral("pointIndexField"), prefix + QStringLiteral("PointIndex")},
            {QStringLiteral("addPointField"), prefix + QStringLiteral("AddPoint")},
            {QStringLiteral("removePointField"), prefix + QStringLiteral("RemovePoint")},
            {QStringLiteral("childCount"), static_cast<int>(state.child_count)},
            {QStringLiteral("childIndex"), static_cast<int>(state.child_index)},
            {QStringLiteral("childIndexField"), prefix + QStringLiteral("ChildIndex")},
            {QStringLiteral("childKindIndex"),
             state.child_kind_index <= 5 ? static_cast<int>(state.child_kind_index - 1) :
             state.child_kind_index >= 7 ? static_cast<int>(state.child_kind_index - 2) :
                                           0},
            {QStringLiteral("childKindField"), prefix + QStringLiteral("ChildKind")},
            {QStringLiteral("childKindChoices"), child_kind_choices},
            {QStringLiteral("childKindValues"), QVariantList{1, 2, 3, 4, 5, 7, 8}},
            {QStringLiteral("childOperatorIndex"), static_cast<int>(state.child_operator_index)},
            {QStringLiteral("childOperatorField"), prefix + QStringLiteral("ChildOperator")},
            {QStringLiteral("operatorChoices"), operator_choices},
            {QStringLiteral("childOpacity"), state.child_opacity},
            {QStringLiteral("childInverted"), state.child_inverted},
            {QStringLiteral("childInvertedField"), prefix + QStringLiteral("ChildInverted")},
            {QStringLiteral("addChildField"), prefix + QStringLiteral("AddChild")},
            {QStringLiteral("removeChildField"), prefix + QStringLiteral("RemoveChild")},
            {QStringLiteral("groupVisible"), state.kind_index == 6},
            {QStringLiteral("pointsVisible"), kind_is(7) || kind_is(8)},
            {QStringLiteral("numericControls"), controls}};
}

} // namespace ravo::studio_develop_internal
