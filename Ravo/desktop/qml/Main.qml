import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import GeoControls 1.0
import GeoControls.AppShell 1.0

Window {
    id: window
    width: 1280
    height: 800
    visible: true
    color: Theme.windowColor
    title: studio.catalogOpen ? ("Ravo Studio — " + studio.catalogPath) : "Ravo Studio"

    property int galleryWidth: 300

    function openCreateLibraryDialog() {
        createDialog.currentFolder = studio.defaultCatalogFolder
        createDialog.initialSelectedFile = studio.defaultCatalogFile
        createDialog.openDialog()
    }

    function openOpenLibraryDialog() {
        openDialog.currentFolder = studio.defaultCatalogFolder
        openDialog.initialSelectedFile = studio.defaultCatalogExists() ? studio.defaultCatalogFile : ""
        openDialog.openDialog()
    }

    function openImportDialog() {
        importDialog.currentFolder = studio.defaultCatalogFolder
        importDialog.openDialog()
    }

    function openImportFolderDialog() {
        importFolderDialog.currentFolder = studio.defaultCatalogFolder
        importFolderDialog.openDialog()
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

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Fonts.toolbarHeight
            color: Qt.lighter(Theme.windowColor, 1.1)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                spacing: Fonts.smallSpacing

                EqualActionButtonRow {
                    CustomButton {
                        text: qsTr("Create Library…")
                        icon.source: "qrc:/GeoControls/icons/Plus.svg"
                        tooltipText: qsTr("Create a new photo library")
                        onClicked: openCreateLibraryDialog()
                    }
                    CustomButton {
                        text: qsTr("Open Library…")
                        icon.source: "qrc:/GeoControls/icons/Open.svg"
                        tooltipText: qsTr("Open an existing photo library")
                        onClicked: openOpenLibraryDialog()
                    }
                    CustomButton {
                        text: qsTr("Import…")
                        icon.source: "qrc:/GeoControls/icons/Load.svg"
                        tooltipText: qsTr("Import photos")
                        enabled: studio.catalogOpen && !studio.busy
                        onClicked: openImportDialog()
                    }
                    CustomButton {
                        text: qsTr("Import Folder…")
                        icon.source: "qrc:/GeoControls/icons/Scene.svg"
                        tooltipText: qsTr("Import a folder of photos")
                        enabled: studio.catalogOpen && !studio.busy
                        onClicked: openImportFolderDialog()
                    }
                }

                Item {
                    Layout.fillWidth: true
                }

                CustomLabel {
                    text: studio.statusText
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 560
                }
            }
        }

        InfoBar {
            Layout.fillWidth: true
            Layout.margins: Fonts.size8
            visible: studio.errorText.length > 0
            severity: "error"
            title: qsTr("Error")
            message: studio.errorText
            closable: false
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.preferredWidth: window.galleryWidth
                Layout.minimumWidth: 220
                Layout.fillHeight: true
                color: Theme.contentSurfaceColor
                border.color: Theme.dividerColor
                border.width: ControlState.borderThin

                ListView {
                    id: assetList
                    anchors.fill: parent
                    anchors.margins: Fonts.size8
                    clip: true
                    spacing: Fonts.size4
                    model: studio.assets
                    delegate: ListTile {
                        required property string assetId
                        required property string displayName
                        required property string mediaType
                        required property string importState
                        width: ListView.view.width
                        title: displayName
                        subtitle: importState + (mediaType.length > 0 ? " · " + mediaType : "")
                        selected: assetId === studio.selectedAssetId
                        onClicked: studio.selectAsset(assetId)
                    }

                    CustomLabel {
                        anchors.centerIn: parent
                        visible: assetList.count === 0
                        text: studio.catalogOpen ? qsTr("No photos imported yet.") : qsTr("No library open.")
                        color: Theme.placeholderTextColor
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: ControlState.borderFocus
                Layout.fillHeight: true
                color: Theme.dividerColor

                MouseArea {
                    anchors.fill: parent
                    anchors.leftMargin: -Fonts.size4
                    anchors.rightMargin: -Fonts.size4
                    cursorShape: Qt.SplitHCursor
                    onPositionChanged: function (mouse) {
                        if (!pressed)
                            return
                        window.galleryWidth = Math.max(220, Math.min(window.width - 360,
                                                                     mapToItem(window.contentItem, mouse.x, 0).x))
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.pageSurfaceColor

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Fonts.toolbarHeight
                        color: Qt.lighter(Theme.windowColor, 1.1)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Fonts.standardMargin
                            anchors.rightMargin: Fonts.standardMargin
                            spacing: Fonts.smallSpacing

                            SegmentedControl {
                                id: viewModeControl
                                model: [qsTr("Fit"), qsTr("100%")]
                                currentIndex: studio.viewMode === "actual" ? 1 : 0
                                enabled: studio.previewUrl.toString().length > 0
                                onActivated: function (index) {
                                    studio.viewMode = index === 0 ? "fit" : "actual"
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                            }

                            CustomLabel {
                                text: studio.previewLoading ? qsTr("Loading preview…") : ""
                                color: Theme.placeholderTextColor
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

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
                        }

                        CustomLabel {
                            anchors.centerIn: parent
                            visible: previewImage.source.toString().length === 0 && !studio.previewLoading
                            text: studio.catalogOpen ? qsTr("Select a photo to view it.") : qsTr("Create or open a library.")
                            color: Theme.placeholderTextColor
                        }
                    }
                }
            }
        }

        MainStatusBar {
            Layout.fillWidth: true
            statusText: studio.statusText
            viewerText: studio.previewLoading ? qsTr("Loading preview…") : (studio.selectedAssetId.length > 0 ? studio.selectedAssetId : "")
        }
    }

    QmlFileDialogPage {
        id: createDialog
        dialogTitle: qsTr("Create Library")
        dialogMode: "save"
        nameFilters: ["Ravo catalog (*.sqlite)"]
        onFileAccepted: function (filePath) {
            studio.createCatalogFromPath(filePath)
        }
    }

    QmlFileDialogPage {
        id: openDialog
        dialogTitle: qsTr("Open Library")
        dialogMode: "open"
        nameFilters: ["Ravo catalog (*.sqlite)"]
        onFileAccepted: function (filePath) {
            studio.openCatalogFromPath(filePath)
        }
    }

    QmlFileDialogPage {
        id: importDialog
        dialogTitle: qsTr("Import Photos")
        dialogMode: "openFiles"
        nameFilters: [
            "Photos (*.png *.jpg *.jpeg *.JPG *.JPEG *.tif *.tiff *.bmp *.gif *.webp *.arw *.ARW *.cr2 *.CR2 *.cr3 *.CR3 *.nef *.NEF *.dng *.DNG *.raf *.orf *.rw2)",
            "All files (*)"
        ]
        onFileAccepted: function (filePath, selectedFilter, filePaths) {
            studio.importFilePaths(filePaths)
        }
    }

    FolderDialogPage {
        id: importFolderDialog
        dialogTitle: qsTr("Import Folder")
        onFolderAccepted: function (folderPath) {
            studio.importFolderFromPath(folderPath)
        }
    }
}
