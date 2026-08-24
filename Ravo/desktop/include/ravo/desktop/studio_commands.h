#pragma once

// Keep these string IDs in sync with StudioActions.ids in QML.

namespace ravo::command_id
{

inline constexpr auto kLibraryCreate = "library.create";
inline constexpr auto kLibraryOpen = "library.open";
inline constexpr auto kLibraryImportFiles = "library.importFiles";
inline constexpr auto kLibraryImportFolder = "library.importFolder";
inline constexpr auto kPhotoSelect = "photo.select";
inline constexpr auto kPhotoRate = "photo.rate";
inline constexpr auto kPhotoColor = "photo.color";
inline constexpr auto kPhotoReject = "photo.reject";
inline constexpr auto kPhotoRemove = "photo.remove";
inline constexpr auto kPhotoPrevious = "photo.previous";
inline constexpr auto kPhotoNext = "photo.next";
inline constexpr auto kViewGrid = "view.grid";
inline constexpr auto kViewLoupe = "view.loupe";
inline constexpr auto kViewDevelop = "view.develop";
inline constexpr auto kViewFit = "view.fit";
inline constexpr auto kViewFill = "view.fill";
inline constexpr auto kViewActual = "view.actual";
inline constexpr auto kEditUndo = "edit.undo";
inline constexpr auto kEditRedo = "edit.redo";
inline constexpr auto kEditResetAll = "edit.resetAll";
inline constexpr auto kEditRotateLeft = "edit.rotateLeft";
inline constexpr auto kEditRotateRight = "edit.rotateRight";
inline constexpr auto kEditBeforeAfter = "edit.beforeAfter";
inline constexpr auto kWindowSettings = "window.settings";
inline constexpr auto kWindowClose = "window.close";
inline constexpr auto kWindowQuit = "window.quit";
inline constexpr auto kWindowAbout = "window.about";

} // namespace ravo::command_id
