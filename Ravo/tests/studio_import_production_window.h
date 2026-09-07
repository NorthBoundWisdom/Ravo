#pragma once

#include <memory>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_import_keyboard_harness.h"

namespace ravo::studio_import_production_window
{

// Loads the production ImportCandidateGrid (formal QML module source) in a real
// QQuickWindow. ImportPhotoGrid+GeoControls is exercised by Studio smoke.
struct ImportCandidateGridWindow
{
    QQmlEngine engine;
    QQuickWindow window;
    ImportCandidateListModel model;
    std::unique_ptr<QQuickItem> root;
    QQuickItem *grid = nullptr;

    ~ImportCandidateGridWindow()
    {
        reset();
    }

    void reset()
    {
        grid = nullptr;
        if (root)
        {
            root->setParentItem(nullptr);
            root.reset();
        }
    }

    [[nodiscard]] bool load(const int candidate_count = 24, const int duplicate_row = -1)
    {
        reset();
        model.setCandidates(
            studio_import_keyboard_harness::make_candidates(candidate_count, duplicate_row));

        const QString harness = QString::fromUtf8(RAVO_IMPORT_CANDIDATE_KEYBOARD_HARNESS_QML);
        QQmlComponent component(&engine, QUrl::fromLocalFile(harness));
        if (component.isError())
        {
            for (const auto &error : component.errors())
                ADD_FAILURE() << error.toString().toStdString();
            return false;
        }
        auto *object = component.create();
        auto *item = qobject_cast<QQuickItem *>(object);
        if (!item)
        {
            delete object;
            return false;
        }
        root.reset(item);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(800, 600));
        root->setProperty("productionGridUrl",
                          QUrl::fromLocalFile(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_GRID_QML)));
        root->setProperty("importCandidates", QVariant::fromValue(&model));
        window.resize(800, 600);
        window.show();
        window.requestActivate();
        QGuiApplication::processEvents();
        for (int attempt = 0; attempt < 50 && !grid; ++attempt)
        {
            grid = root->findChild<QQuickItem *>(QStringLiteral("importCandidateKeyboardGrid"));
            if (!grid)
                QGuiApplication::processEvents();
        }
        if (!grid)
            return false;
        QMetaObject::invokeMethod(root.get(), "focusCandidateGrid", Qt::DirectConnection);
        QGuiApplication::processEvents();
        return grid->hasActiveFocus();
    }

    // Deliver keys only while the production grid already owns focus. Never
    // forceActiveFocus here and never fall back when focus has moved away.
    void key(const int key, const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        ASSERT_NE(grid, nullptr);
        window.requestActivate();
        QGuiApplication::processEvents();
        ASSERT_TRUE(window.isActive());
        ASSERT_TRUE(grid->hasActiveFocus())
            << "grid lost active focus; refuse to force or retarget";
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers);
        QCoreApplication::sendEvent(grid, &press);
        QCoreApplication::sendEvent(grid, &release);
        QGuiApplication::processEvents();
    }

    [[nodiscard]] int currentIndex() const
    {
        return grid ? grid->property("currentIndex").toInt() : -1;
    }
};

} // namespace ravo::studio_import_production_window
