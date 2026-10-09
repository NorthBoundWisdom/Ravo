#include <memory>

#include <QColor>
#include <QAccessible>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "studio_import_production_window.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;
using studio_import_production_window::ScopedQuickItem;

void add_control_imports(QQmlEngine &engine)
{
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(QStringLiteral(RAVO_GEOCONTROLS_QML_IMPORT_ROOT));
    engine.addImportPath(QStringLiteral(RAVO_GEOCONTROLS_APPSHELL_QML_IMPORT_ROOT));
}

void send_key(QQuickWindow &window, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    QCoreApplication::processEvents();
}

QQuickItem *visual_text_field(QQuickItem *item)
{
    if (!item)
        return nullptr;
    if (item->inherits("QQuickTextField"))
        return item;
    for (auto *child : item->childItems())
        if (auto *field = visual_text_field(child))
            return field;
    return nullptr;
}

TEST(StudioControls, ProductionWidgetsAlwaysUseOwnedVisuals)
{
    const QRegularExpression trivia(
        QStringLiteral(R"qml(//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')qml"));
    const QRegularExpression default_control(QStringLiteral(
        R"(\b(?:Button|ToolButton|RoundButton|DelayButton|Slider|RangeSlider|Dial|SpinBox|ComboBox|CheckBox|RadioButton|Switch|Menu|MenuBar|MenuBarItem|MenuItem|MenuSeparator|ToolTip|ScrollView|ScrollBar|ScrollIndicator|BusyIndicator|ProgressBar|Popup|Drawer|Dialog|DialogButtonBox|TextField|TextArea|TabBar|TabButton|SplitView|ItemDelegate|SwipeDelegate|CheckDelegate|RadioDelegate|SwitchDelegate|TreeViewDelegate|GroupBox|ToolBar|ToolSeparator|Page|PageIndicator|Label|Pane|Frame|Tumbler|HorizontalHeaderView|VerticalHeaderView)\s*\{|\bToolTip\.(?:visible|text)\s*:)"));
    QDirIterator files(QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml"),
                       {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
    int scanned = 0;
    while (files.hasNext())
    {
        const auto path = files.next();
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text)) << path.toStdString();
        auto source = QString::fromUtf8(file.readAll());
        source.remove(trivia);
        const auto match = default_control.match(source);
        EXPECT_FALSE(match.hasMatch())
            << path.toStdString() << ": use the GeoControls visual owner for "
            << match.captured().toStdString();
        ++scanned;
    }
    EXPECT_GT(scanned, 70);
}

TEST(StudioControls, ImportThumbnailSliderPreservesPointerKeyboardAndDisabledBehavior)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    QStringList warnings;
    QQmlEngine engine;
    add_control_imports(engine);
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errors)
                     {
                         for (const auto &error : errors)
                             warnings.push_back(error.toString());
                     });
    QQuickWindow window;
    QQmlComponent actions_component(&engine,
                                    QUrl::fromLocalFile(QStringLiteral(RAVO_STUDIO_ACTIONS_QML)));
    std::unique_ptr<QObject> actions(actions_component.createWithInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(&commands)},
         {QStringLiteral("presenter"), QVariant::fromValue(&presenter)}}));
    ASSERT_NE(actions, nullptr) << actions_component.errorString().toStdString();
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QStringLiteral(RAVO_STUDIO_IMPORT_PAGE_QML)));
    ScopedQuickItem page(qobject_cast<QQuickItem *>(component.createWithInitialProperties(
        {{QStringLiteral("presenter"), QVariant::fromValue(&presenter)},
         {QStringLiteral("commands"), QVariant::fromValue(actions.get())}})));
    ASSERT_TRUE(page) << component.errorString().toStdString();
    page->setParentItem(window.contentItem());
    page->setSize(QSizeF(1280, 800));
    window.resize(1280, 800);
    window.show();
    window.requestActivate();
    ASSERT_TRUE(wait_until([&] { return window.isActive(); }));
    auto *control = page->findChild<QQuickItem *>(QStringLiteral("importThumbnailSize"));
    auto *grid = page->findChild<QQuickItem *>(QStringLiteral("importPhotoGrid"));
    ASSERT_NE(control, nullptr);
    ASSERT_NE(grid, nullptr);
    QQuickItem *slider = nullptr;
    for (auto *child : control->findChildren<QQuickItem *>())
        if (child->inherits("QQuickSlider"))
            slider = child;
    ASSERT_NE(slider, nullptr);
    auto *accessible = QAccessible::queryAccessibleInterface(slider);
    ASSERT_NE(accessible, nullptr);
    EXPECT_EQ(accessible->text(QAccessible::Name), QStringLiteral("Thumbnail size"));
    EXPECT_FALSE(control->property("showTitle").toBool());
    EXPECT_FALSE(control->property("showValueLabel").toBool());
    EXPECT_FALSE(control->property("showStepButton").toBool());
    EXPECT_DOUBLE_EQ(grid->property("preferredCell").toDouble(), 180);
    slider->forceActiveFocus(Qt::TabFocusReason);
    ASSERT_TRUE(slider->hasActiveFocus());
    send_key(window, Qt::Key_Right);
    EXPECT_DOUBLE_EQ(grid->property("preferredCell").toDouble(), 181);
    const auto start = slider->mapToScene(QPointF(slider->width() / 2, slider->height() / 2));
    const auto end = slider->mapToScene(QPointF(slider->width() * 0.8, slider->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, start, window.mapToGlobal(start.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, end, window.mapToGlobal(end.toPoint()), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, end, window.mapToGlobal(end.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &move);
    // Live sizing must not wait until release.
    EXPECT_GT(grid->property("preferredCell").toDouble(), 181);
    QCoreApplication::sendEvent(&window, &release);
    const auto resized = grid->property("preferredCell").toDouble();
    EXPECT_DOUBLE_EQ(control->property("value").toDouble(), resized);
    control->setEnabled(false);
    send_key(window, Qt::Key_Right);
    EXPECT_DOUBLE_EQ(grid->property("preferredCell").toDouble(), resized);
    page->setSize(QSizeF(440, 800));
    window.resize(440, 800);
    QCoreApplication::processEvents();
    EXPECT_GE(slider->x(), 0);
    EXPECT_LE(slider->mapToItem(control, QPointF(slider->width(), 0)).x(), control->width());
    QPointer<QQuickItem> watch(control);
    page.reset();
    EXPECT_TRUE(watch.isNull());
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}

TEST(StudioControls, SharedScrollingMenusAndBusyStateUseLiveTheme)
{
    ensure_qt_core();
    QStringList warnings;
    QQmlEngine engine;
    add_control_imports(engine);
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errors)
                     {
                         for (const auto &error : errors)
                             warnings.push_back(error.toString());
                     });
    QQuickWindow window;
    QQmlComponent component(&engine);
    component.setData(R"qml(
import QtQuick
import QtQuick.Controls
import GeoControls 1.0
Item {
    property int invocations: 0
    CustomScrollView {
        id: scroll
        objectName: "scroll"
        width: 160
        height: 100
        Column {
            Repeater {
                model: 20
                CustomLabel { text: "row"; width: 120; height: 30 }
            }
        }
    }
    CustomBusyIndicator { objectName: "busy"; x: 180; running: false }
    CustomMenu {
        id: menu
        objectName: "menu"
        popupType: Popup.Item
        focus: true
        CustomMenuItem { text: "Disabled"; enabled: false }
        CustomMenuItem { text: "Run"; onTriggered: ++invocations }
        CustomMenuSeparator {}
    }
    function recolor() {
        Theme.placeholderTextColor = "#e32145";
        Theme.popupSurfaceColor = "#17283b";
    }
    function showMenu() { menu.open(); }
}
)qml",
                      QUrl::fromLocalFile(QStringLiteral(RAVO_STUDIO_MAIN_QML)));
    ScopedQuickItem root(qobject_cast<QQuickItem *>(component.create()));
    ASSERT_TRUE(root) << component.errorString().toStdString();
    root->setParentItem(window.contentItem());
    root->setSize(QSizeF(400, 300));
    window.resize(400, 300);
    window.show();
    window.requestActivate();
    ASSERT_TRUE(wait_until([&] { return window.isActive(); }));
    auto *scroll = root->findChild<QQuickItem *>(QStringLiteral("scroll"));
    auto *busy = root->findChild<QQuickItem *>(QStringLiteral("busy"));
    auto *menu = root->findChild<QObject *>(QStringLiteral("menu"));
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(busy, nullptr);
    ASSERT_NE(menu, nullptr);
    QQuickItem *vertical = nullptr;
    for (auto *child : scroll->findChildren<QQuickItem *>())
        if (child->inherits("QQuickScrollBar") &&
            child->property("orientation").toInt() == Qt::Vertical)
            vertical = child;
    ASSERT_NE(vertical, nullptr);
    ASSERT_TRUE(wait_until([&] { return vertical->property("size").toDouble() < 1; }));
    EXPECT_TRUE(vertical->property("interactive").toBool());
    vertical->setProperty("position", 0.25);
    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    ASSERT_NE(flickable, nullptr);
    EXPECT_GT(flickable->property("contentY").toDouble(), 0);
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "recolor"));
    auto *thumb = vertical->property("contentItem").value<QQuickItem *>();
    ASSERT_NE(thumb, nullptr);
    EXPECT_EQ(thumb->property("color").value<QColor>(), QColor("#e32145"));
    auto *background = menu->property("background").value<QQuickItem *>();
    ASSERT_NE(background, nullptr);
    EXPECT_EQ(background->property("color").value<QColor>(), QColor("#17283b"));
    busy->setProperty("running", true);
    auto *ring = busy->property("contentItem").value<QQuickItem *>();
    ASSERT_NE(ring, nullptr);
    EXPECT_DOUBLE_EQ(ring->opacity(), 1);
    busy->setProperty("running", false);
    EXPECT_DOUBLE_EQ(ring->opacity(), 0);
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "showMenu"));
    ASSERT_TRUE(wait_until([&] { return menu->property("opened").toBool(); }));
    send_key(window, Qt::Key_Down);
    send_key(window, Qt::Key_Return);
    EXPECT_EQ(root->property("invocations").toInt(), 1);
    QPointer<QObject> menu_watch(menu);
    root.reset();
    EXPECT_TRUE(menu_watch.isNull());
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}
TEST(StudioControls, SharedInputDialogEscapeKeepsCancellationSemantics)
{
    ensure_qt_core();
    QStringList warnings;
    QQmlEngine engine;
    add_control_imports(engine);
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &errors)
                     {
                         for (const auto &error : errors)
                             warnings.push_back(error.toString());
                     });
    QQuickWindow window;
    QQmlComponent component(&engine);
    component.setData(R"qml(
import QtQuick
import GeoControls 1.0
Item {
    property int cancellations: 0
    QmlInputDialogPage {
        id: input
        objectName: "input"
        initialText: "original"
        onCancelled: ++cancellations
    }
    QmlKeyValueDialogPage {
        id: values
        objectName: "values"
        keyValueList: [{name: "Name", value: "original"}]
        onCancelled: ++cancellations
    }
    function showInput() { input.openDialog(); }
    function showValues() { values.openDialog(); }
}
)qml",
                      QUrl::fromLocalFile(QStringLiteral(RAVO_STUDIO_MAIN_QML)));
    ScopedQuickItem root(qobject_cast<QQuickItem *>(component.create()));
    ASSERT_TRUE(root) << component.errorString().toStdString();
    root->setParentItem(window.contentItem());
    root->setSize(QSizeF(800, 600));
    window.resize(800, 600);
    window.show();
    window.requestActivate();
    ASSERT_TRUE(wait_until([&] { return window.isActive(); }));
    auto *input = root->findChild<QObject *>(QStringLiteral("input"));
    auto *values = root->findChild<QObject *>(QStringLiteral("values"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(values, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "showInput"));
    ASSERT_TRUE(wait_until([&] { return input->property("opened").toBool(); }));
    auto *input_field = visual_text_field(input->property("bodyItem").value<QQuickItem *>());
    ASSERT_NE(input_field, nullptr);
    input_field->forceActiveFocus(Qt::TabFocusReason);
    ASSERT_EQ(window.activeFocusItem(), input_field);
    send_key(window, Qt::Key_Escape);
    EXPECT_TRUE(wait_until([&] { return !input->property("visible").toBool(); }));
    EXPECT_EQ(root->property("cancellations").toInt(), 1);
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "showValues"));
    ASSERT_TRUE(wait_until([&] { return values->property("opened").toBool(); }));
    auto *field = visual_text_field(values->property("bodyItem").value<QQuickItem *>());
    ASSERT_NE(field, nullptr);
    ASSERT_TRUE(field->inherits("QQuickTextField"));
    field->forceActiveFocus(Qt::TabFocusReason);
    ASSERT_EQ(window.activeFocusItem(), field);
    send_key(window, Qt::Key_Escape);
    EXPECT_TRUE(wait_until([&] { return !values->property("visible").toBool(); }));
    EXPECT_EQ(root->property("cancellations").toInt(), 2);
    root.reset();
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}
} // namespace
} // namespace ravo
