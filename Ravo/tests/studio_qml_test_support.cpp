#include "studio_qml_test_support.h"

#include <array>

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ravo
{

QString combined_develop_qml_source()
{
    const QFileInfo panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    const QDir directory = panel.dir();
    static constexpr std::array<const char *, 30> files{
        "DevelopPanel.qml",
        "DevelopLightSection.qml",
        "DevelopCurvesSection.qml",
        "DevelopColorCoreSection.qml",
        "DevelopColorEqualizerSection.qml",
        "DevelopColorAdvancedSection.qml",
        "DevelopAdvancedLooks.qml",
        "DevelopAdvancedToning.qml",
        "DevelopAdvancedColorOps.qml",
        "DevelopPrimariesSection.qml",
        "DevelopGeometrySection.qml",
        "DevelopToneEqualizerSection.qml",
        "DevelopGraduatedSection.qml",
        "DevelopEffectsSection.qml",
        "DevelopDetailSection.qml",
        "DevelopRawSection.qml",
        "DevelopCalibrationSection.qml",
        "DevelopInputProfileSection.qml",
        "DevelopProfileGammaSection.qml",
        "DevelopOutputProfileSection.qml",
        "DevelopSection.qml",
        "CurveOptionButton.qml",
        "MixerSlider.qml",
        "PrimariesSlider.qml",
        "MaskEditor.qml",
        "ColorCheckerNumberField.qml",
        "ColorContrastOffsetField.qml",
        "DevelopColorSlider.qml",
        "ToneCurveEditor.qml",
        "ColorGradeWheel.qml",
    };
    QString source;
    for (const auto *name : files)
    {
        QFile file(directory.filePath(QString::fromLatin1(name)));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            return {};
        }
        source += QStringLiteral("\n// ");
        source += QString::fromLatin1(name);
        source += QLatin1Char('\n');
        source += QString::fromUtf8(file.readAll());
    }
    return source;
}

} // namespace ravo
