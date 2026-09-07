#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"

namespace ravo::studio_import_keyboard_harness
{

struct ImportKeyboardHarness
{
    // Declared so unique_ptr<root> destroys before window/engine (reverse order).
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QQuickItem> root;
    QQuickItem *grid = nullptr;
    ImportCandidateListModel *model = nullptr;

    ImportKeyboardHarness() = default;
    ImportKeyboardHarness(const ImportKeyboardHarness &) = delete;
    ImportKeyboardHarness &operator=(const ImportKeyboardHarness &) = delete;

    ~ImportKeyboardHarness()
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

    [[nodiscard]] bool load(ImportCandidateListModel *candidates)
    {
        reset();
        model = candidates;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(
                                             RAVO_IMPORT_CANDIDATE_KEYBOARD_HARNESS_QML)));
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
        root->setProperty("importCandidates", QVariant::fromValue(model));
        window.resize(800, 600);
        window.show();
        QGuiApplication::processEvents();
        for (int attempt = 0; attempt < 50 && !grid; ++attempt)
        {
            grid = root->findChild<QQuickItem *>(QStringLiteral("importCandidateKeyboardGrid"));
            if (!grid)
                QGuiApplication::processEvents();
        }
        if (!grid)
        {
            reset();
            return false;
        }
        QMetaObject::invokeMethod(root.get(), "focusCandidateGrid", Qt::DirectConnection);
        QGuiApplication::processEvents();
        if (!grid->hasActiveFocus())
        {
            reset();
            return false;
        }
        return true;
    }

    void key(const int key, const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        ASSERT_NE(grid, nullptr);
        grid->forceActiveFocus();
        QGuiApplication::processEvents();
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers);
        QCoreApplication::sendEvent(grid, &press);
        QCoreApplication::sendEvent(grid, &release);
        QGuiApplication::processEvents();
    }

    void keyThroughWindowFocus(const int key,
                               const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        ASSERT_NE(grid, nullptr);
        window.requestActivate();
        grid->forceActiveFocus();
        QGuiApplication::processEvents();
        ASSERT_TRUE(grid->hasActiveFocus());
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers);
        auto *target = window.focusObject() ? window.focusObject() : static_cast<QObject *>(grid);
        QCoreApplication::sendEvent(target, &press);
        QCoreApplication::sendEvent(target, &release);
        QGuiApplication::processEvents();
    }

    [[nodiscard]] int currentIndex() const
    {
        return grid ? grid->property("currentIndex").toInt() : -1;
    }

    [[nodiscard]] int selectionAnchor() const
    {
        return root ? root->property("selectionAnchor").toInt() : -1;
    }

    [[nodiscard]] bool gridHasFocus() const
    {
        return grid && grid->hasActiveFocus();
    }

    [[nodiscard]] qreal contentY() const
    {
        return grid ? grid->property("contentY").toReal() : 0;
    }

    [[nodiscard]] int columnCount() const
    {
        QVariant columns;
        QMetaObject::invokeMethod(root.get(), "keyboardColumnCount", Qt::DirectConnection,
                                  Q_RETURN_ARG(QVariant, columns));
        return std::max(1, columns.toInt());
    }

    [[nodiscard]] int pageStep() const
    {
        QVariant step;
        QMetaObject::invokeMethod(root.get(), "keyboardPageStep", Qt::DirectConnection,
                                  Q_RETURN_ARG(QVariant, step));
        return std::max(1, step.toInt());
    }
};

[[nodiscard]] inline std::vector<ImportCandidate> make_candidates(const int count,
                                                                  const int duplicate_row = -1)
{
    std::vector<ImportCandidate> candidates(static_cast<std::size_t>(count));
    for (int row = 0; row < count; ++row)
    {
        candidates[static_cast<std::size_t>(row)].source_path =
            "/candidate-" + std::to_string(row) + ".png";
        candidates[static_cast<std::size_t>(row)].display_name =
            "candidate-" + std::to_string(row) + ".png";
        candidates[static_cast<std::size_t>(row)].size_bytes =
            static_cast<std::uint64_t>((row + 1) * 10);
    }
    if (duplicate_row >= 0 && duplicate_row < count)
    {
        candidates[static_cast<std::size_t>(duplicate_row)].duplicate = true;
        candidates[static_cast<std::size_t>(duplicate_row)].duplicate_reason = "catalog_content";
    }
    return candidates;
}

} // namespace ravo::studio_import_keyboard_harness
