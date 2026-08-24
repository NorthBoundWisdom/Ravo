import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    visible: true
    title: studio.catalogOpen ? ("Ravo Studio — " + studio.catalogPath) : "Ravo Studio"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 8

            ToolButton {
                text: "Create Library…"
                onClicked: openCreateLibraryDialog()
            }
            ToolButton {
                text: "Open Library…"
                onClicked: openOpenLibraryDialog()
            }
            ToolButton {
                text: "Import…"
                enabled: studio.catalogOpen && !studio.busy
                onClicked: importDialog.open()
            }
            ToolButton {
                text: "Import Folder…"
                enabled: studio.catalogOpen && !studio.busy
                onClicked: importFolderDialog.open()
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                text: studio.statusText
                elide: Text.ElideMiddle
                Layout.maximumWidth: 560
            }
        }
    }

    FileDialog {
        id: createDialog
        title: "Create Library"
        fileMode: FileDialog.SaveFile
        nameFilters: ["Ravo catalog (*.sqlite)"]
        defaultSuffix: "sqlite"
        currentFolder: studio.defaultCatalogFolder
        onAccepted: studio.createCatalog(selectedFile)
    }

    FileDialog {
        id: openDialog
        title: "Open Library"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Ravo catalog (*.sqlite)"]
        currentFolder: studio.defaultCatalogFolder
        onAccepted: studio.openCatalog(selectedFile)
    }

    FileDialog {
        id: importDialog
        title: "Import Photos"
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            "Photos (*.png *.jpg *.jpeg *.JPG *.JPEG *.tif *.tiff *.bmp *.gif *.webp *.arw *.ARW *.cr2 *.CR2 *.cr3 *.CR3 *.nef *.NEF *.dng *.DNG *.raf *.orf *.rw2)",
            "All files (*)"
        ]
        currentFolder: studio.defaultCatalogFolder
        onAccepted: studio.importFiles(selectedFiles)
    }

    FolderDialog {
        id: importFolderDialog
        title: "Import Folder"
        currentFolder: studio.defaultCatalogFolder
        onAccepted: studio.importFolder(selectedFolder)
    }

    function openCreateLibraryDialog() {
        createDialog.currentFolder = studio.defaultCatalogFolder
        createDialog.selectedFile = studio.defaultCatalogFile
        createDialog.open()
    }

    function openOpenLibraryDialog() {
        openDialog.currentFolder = studio.defaultCatalogFolder
        if (studio.defaultCatalogExists())
            openDialog.selectedFile = studio.defaultCatalogFile
        openDialog.open()
    }

    function startLibrarySession() {
        if (studio.catalogOpen)
            return
        if (studio.defaultCatalogExists())
            studio.openCatalog(studio.defaultCatalogFile)
        else
            openCreateLibraryDialog()
    }

    Component.onCompleted: Qt.callLater(startLibrarySession)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            visible: studio.errorText.length > 0
            text: studio.errorText
            color: "#8b1a1a"
            padding: 8
            Layout.fillWidth: true
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            Frame {
                SplitView.preferredWidth: 300
                SplitView.minimumWidth: 220

                ListView {
                    id: assetList
                    anchors.fill: parent
                    clip: true
                    model: studio.assets
                    delegate: ItemDelegate {
                        required property string assetId
                        required property string displayName
                        required property string mediaType
                        required property string importState
                        width: ListView.view.width
                        text: displayName + " · " + importState
                        highlighted: assetId === studio.selectedAssetId
                        onClicked: studio.selectAsset(assetId)
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: assetList.count === 0
                        text: studio.catalogOpen ? "No photos imported yet." : "No library open."
                        opacity: 0.7
                    }
                }
            }

            Page {
                footer: ToolBar {
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        Button {
                            text: "Fit"
                            enabled: studio.previewUrl.toString().length > 0
                            highlighted: studio.viewMode === "fit"
                            onClicked: studio.viewMode = "fit"
                        }
                        Button {
                            text: "100%"
                            enabled: studio.previewUrl.toString().length > 0
                            highlighted: studio.viewMode === "actual"
                            onClicked: studio.viewMode = "actual"
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Label {
                            text: studio.previewLoading ? "Loading preview…" : ""
                        }
                    }
                }

                Flickable {
                    id: scroller
                    anchors.fill: parent
                    clip: true
                    contentWidth: studio.viewMode === "fit" ? width : Math.max(width, previewImage.implicitWidth)
                    contentHeight: studio.viewMode === "fit" ? height : Math.max(height, previewImage.implicitHeight)

                    Image {
                        id: previewImage
                        width: studio.viewMode === "fit" ? scroller.width : implicitWidth
                        height: studio.viewMode === "fit" ? scroller.height : implicitHeight
                        fillMode: studio.viewMode === "fit" ? Image.PreserveAspectFit : Image.Pad
                        asynchronous: true
                        cache: false
                        source: studio.previewUrl
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: previewImage.source.toString().length === 0 && !studio.previewLoading
                        text: studio.catalogOpen ? "Select a photo to view it." : "Create or open a library."
                        opacity: 0.7
                    }
                }
            }
        }
    }
}
