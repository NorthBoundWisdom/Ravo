import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var commands
    property real viewRectX: 0
    property real viewRectY: 0
    property real viewRectW: 1
    property real viewRectH: 1
    color: Theme.railSurfaceColor

    readonly property int treeColumnWidth: Fonts.size20
    readonly property int treeGuideX: Math.floor(treeColumnWidth / 2)
    readonly property bool developOpen: presenter && presenter.browseMode === "develop"
    signal viewportSeeked(real nx, real ny)

    function backupTime(unixMs) {
        if (!unixMs || unixMs <= 0)
            return qsTr("Never");
        return new Date(unixMs).toLocaleString(Qt.locale(), Locale.ShortFormat);
    }

    function backupBytes(bytes) {
        if (!bytes || bytes <= 0)
            return qsTr("0 B");
        if (bytes < 1024)
            return qsTr("%1 B").arg(bytes);
        if (bytes < 1024 * 1024)
            return qsTr("%1 KiB").arg((bytes / 1024).toFixed(1));
        return qsTr("%1 MiB").arg((bytes / (1024 * 1024)).toFixed(1));
    }

    Connections {
        target: root.presenter
        function onSelectionChanged() {
            if (!root.presenter || root.presenter.selectedAssetId.length === 0)
                return;
            if (root.presenter.selectedThumbnailUrl.toString().length > 0)
                return;
            root.presenter.ensureThumbnail(root.presenter.selectedAssetId);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        CustomLabel {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.topMargin: Fonts.size12
            Layout.bottomMargin: Fonts.size8
            visible: !root.developOpen
            text: qsTr("Library")
            font.bold: true
        }

        ColumnLayout {
            id: libraryWork
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.rightMargin: Fonts.standardMargin
            Layout.bottomMargin: Fonts.size8
            spacing: Fonts.size6
            visible: !root.developOpen && root.presenter && (root.presenter.importWorkActive || root.presenter.importPreviewWorkActive || root.presenter.previewWorkActive || root.presenter.catalogOperationActive || (root.presenter.importWorkTotal > 0 && root.presenter.importWorkCompleted < root.presenter.importWorkTotal) || (root.presenter.previewWorkTotal > 0 && root.presenter.previewWorkCompleted < root.presenter.previewWorkTotal))

            function meterVisible(active, completed, total) {
                return active || (total > 0 && completed < total);
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                visible: libraryWork.meterVisible(root.presenter.importWorkActive, root.presenter.importWorkCompleted, root.presenter.importWorkTotal)

                RowLayout {
                    Layout.fillWidth: true
                    CustomLabel {
                        text: qsTr("Import")
                        font.pixelSize: Fonts.size10
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    CustomLabel {
                        text: root.presenter.importWorkTotal > 0 ? (root.presenter.importWorkCompleted + " / " + root.presenter.importWorkTotal) : qsTr("Scanning…")
                        color: Theme.placeholderTextColor
                        font.pixelSize: Fonts.size10
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 6
                    radius: 3
                    color: Theme.midlightColor
                    Rectangle {
                        width: parent.width * (root.presenter.importWorkTotal > 0 ? Math.min(1, root.presenter.importWorkCompleted / root.presenter.importWorkTotal) : 0.35)
                        height: parent.height
                        radius: 3
                        color: Theme.accentColor
                    }
                }
                CustomButton {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("Cancel")
                    visible: root.presenter.importWorkActive
                    onClicked: if (root.commands)
                        root.commands.run(root.commands.ids.libraryCancelOperation)
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                visible: libraryWork.meterVisible(root.presenter.previewWorkActive, root.presenter.previewWorkCompleted, root.presenter.previewWorkTotal)

                RowLayout {
                    Layout.fillWidth: true
                    CustomLabel {
                        text: qsTr("Previews")
                        font.pixelSize: Fonts.size10
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    CustomLabel {
                        text: root.presenter.previewWorkCompleted + " / " + root.presenter.previewWorkTotal
                        color: Theme.placeholderTextColor
                        font.pixelSize: Fonts.size10
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 6
                    radius: 3
                    color: Theme.midlightColor
                    Rectangle {
                        width: parent.width * (root.presenter.previewWorkTotal > 0 ? Math.min(1, root.presenter.previewWorkCompleted / root.presenter.previewWorkTotal) : 0)
                        height: parent.height
                        radius: 3
                        color: Theme.highlightColor
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                visible: root.presenter.importPreviewWorkActive
                RowLayout {
                    Layout.fillWidth: true
                    CustomLabel {
                        text: qsTr("Import previews")
                        font.pixelSize: Fonts.size10
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    CustomLabel {
                        text: root.presenter.importPreviewWorkCompleted + " / " + root.presenter.importPreviewWorkTotal
                        color: Theme.placeholderTextColor
                        font.pixelSize: Fonts.size10
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 6
                    radius: 3
                    color: Theme.midlightColor
                    Rectangle {
                        width: parent.width * (root.presenter.importPreviewWorkTotal > 0 ? Math.min(1, root.presenter.importPreviewWorkCompleted / root.presenter.importPreviewWorkTotal) : 0)
                        height: parent.height
                        radius: 3
                        color: Theme.highlightColor
                    }
                }
                CustomButton {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("Cancel")
                    onClicked: root.presenter.cancelImportPreviews()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                visible: root.presenter.catalogOperationActive

                RowLayout {
                    Layout.fillWidth: true
                    CustomLabel {
                        Layout.fillWidth: true
                        text: root.presenter.catalogOperationStage
                        elide: Text.ElideRight
                        font.pixelSize: Fonts.size10
                    }
                    CustomLabel {
                        visible: root.presenter.catalogOperationTotal > 0
                        text: root.presenter.catalogOperationCompleted + " / " + root.presenter.catalogOperationTotal
                        color: Theme.placeholderTextColor
                        font.pixelSize: Fonts.size10
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 6
                    radius: 3
                    color: Theme.midlightColor
                    Rectangle {
                        width: parent.width * (root.presenter.catalogOperationTotal > 0 ? Math.min(1, root.presenter.catalogOperationCompleted / root.presenter.catalogOperationTotal) : 0.35)
                        height: parent.height
                        radius: 3
                        color: Theme.accentColor
                    }
                }
                CustomButton {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("Cancel")
                    onClicked: if (root.commands)
                        root.commands.run(root.commands.ids.libraryCancelOperation)
                }
            }
        }

        ColumnLayout {
            id: backupStatus
            readonly property var value: root.presenter ? root.presenter.backupScheduleStatus : ({})
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.rightMargin: Fonts.standardMargin
            Layout.bottomMargin: Fonts.size8
            spacing: 2
            visible: !root.developOpen && root.presenter && root.presenter.catalogOpen && value.loaded

            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Scheduled backups")
                    font.pixelSize: Fonts.size10
                    font.bold: true
                }
                CustomLabel {
                    text: backupStatus.value.enabled ? qsTr("On") : qsTr("Off")
                    color: backupStatus.value.enabled ? Theme.accentColor : Theme.placeholderTextColor
                    font.pixelSize: Fonts.size10
                }
            }
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Last verified: %1 · %2").arg(root.backupTime(backupStatus.value.lastSuccessUnixMs)).arg(root.backupBytes(backupStatus.value.lastBackupBytes))
                color: Theme.placeholderTextColor
                font.pixelSize: Fonts.size10
                elide: Text.ElideRight
            }
            CustomLabel {
                Layout.fillWidth: true
                visible: backupStatus.value.enabled === true
                text: qsTr("Next: %1 · Keep %2").arg(root.backupTime(backupStatus.value.nextRunUnixMs)).arg(backupStatus.value.retentionCount)
                color: Theme.placeholderTextColor
                font.pixelSize: Fonts.size10
                elide: Text.ElideRight
            }
            CustomLabel {
                Layout.fillWidth: true
                visible: String(backupStatus.value.destination || "").length > 0
                text: String(backupStatus.value.destination || "")
                color: Theme.placeholderTextColor
                font.pixelSize: Fonts.size10
                elide: Text.ElideMiddle
            }
            CustomLabel {
                Layout.fillWidth: true
                visible: String(backupStatus.value.lastError || "").length > 0
                text: qsTr("Last failure: %1").arg(String(backupStatus.value.lastError || ""))
                color: Theme.errorColor
                font.pixelSize: Fonts.size10
                wrapMode: Text.WordWrap
            }
        }

        Item {
            id: navigator
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            Layout.preferredHeight: {
                const iw = Math.max(1, navImage.implicitWidth);
                const ih = Math.max(1, navImage.implicitHeight);
                const maxH = Fonts.scaledUiSize(200);
                return Math.max(Fonts.scaledUiSize(88), Math.min(maxH, width * ih / iw));
            }
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: 4
                color: Theme.pageSurfaceColor
                border.width: 1
                border.color: Theme.dividerColor
            }

            Image {
                id: navImage
                anchors.fill: parent
                anchors.margins: 1
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                source: {
                    if (!root.presenter)
                        return "";
                    if (root.presenter.previewUrl.toString().length)
                        return root.presenter.previewUrl;
                    return root.presenter.selectedThumbnailUrl;
                }
                visible: source.toString().length > 0
            }

            CustomLabel {
                anchors.centerIn: parent
                visible: navImage.status !== Image.Ready
                text: qsTr("No photo")
                color: Theme.placeholderTextColor
                font.pixelSize: Fonts.size10
            }

            Rectangle {
                id: viewBox
                readonly property real imgX: (navImage.width - navImage.paintedWidth) / 2
                readonly property real imgY: (navImage.height - navImage.paintedHeight) / 2
                visible: navImage.status === Image.Ready && navImage.paintedWidth > 1 && navImage.paintedHeight > 1
                x: viewBox.imgX + root.viewRectX * navImage.paintedWidth
                y: viewBox.imgY + root.viewRectY * navImage.paintedHeight
                width: Math.max(6, root.viewRectW * navImage.paintedWidth)
                height: Math.max(6, root.viewRectH * navImage.paintedHeight)
                color: "transparent"
                border.width: 1
                border.color: "#f4f4f4"
                z: 2
            }

            MouseArea {
                anchors.fill: parent
                enabled: navImage.status === Image.Ready && root.presenter && root.presenter.browseMode !== "grid"
                cursorShape: enabled ? Qt.OpenHandCursor : Qt.ArrowCursor
                function seekTo(px, py) {
                    const imgX = (navImage.width - navImage.paintedWidth) / 2;
                    const imgY = (navImage.height - navImage.paintedHeight) / 2;
                    const fx = navImage.paintedWidth > 0 ? (px - imgX) / navImage.paintedWidth : 0;
                    const fy = navImage.paintedHeight > 0 ? (py - imgY) / navImage.paintedHeight : 0;
                    root.viewportSeeked(fx - root.viewRectW / 2, fy - root.viewRectH / 2);
                }
                onPressed: function (mouse) {
                    seekTo(mouse.x, mouse.y);
                }
                onPositionChanged: function (mouse) {
                    if (pressed)
                        seekTo(mouse.x, mouse.y);
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            spacing: Fonts.smallSpacing

            Rectangle {
                id: zoomModeBar
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
                implicitHeight: ControlState.minInputHeight
                radius: ControlState.radiusSmall
                color: Theme.baseColor
                border.color: Theme.midColor
                border.width: ControlState.borderThin
                enabled: root.presenter && root.presenter.browseMode !== "grid" && root.presenter.previewUrl.toString().length > 0
                readonly property int currentIndex: root.presenter && root.presenter.zoomMode === "fill" ? 1 : (root.presenter && root.presenter.zoomMode === "actual" ? 2 : 0)
                function activate(index) {
                    if (!root.presenter || !root.commands)
                        return;
                    root.commands.run(root.commands.ids.viewSetZoomMode, index === 1 ? "fill" : (index === 2 ? "actual" : "fit"));
                }

                Item {
                    anchors.fill: parent
                    anchors.margins: zoomModeBar.border.width
                    clip: true

                    RowLayout {
                        anchors.fill: parent
                        spacing: 0

                        SegmentedButton {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 0
                            text: qsTr("Fit")
                            selected: zoomModeBar.currentIndex === 0
                            onClicked: zoomModeBar.activate(0)
                        }
                        Rectangle {
                            Layout.preferredWidth: ControlState.borderThin
                            Layout.fillHeight: true
                            Layout.topMargin: Fonts.size2
                            Layout.bottomMargin: Fonts.size2
                            color: Theme.midColor
                        }
                        SegmentedButton {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 0
                            text: qsTr("Fill")
                            selected: zoomModeBar.currentIndex === 1
                            onClicked: zoomModeBar.activate(1)
                        }
                        Rectangle {
                            Layout.preferredWidth: ControlState.borderThin
                            Layout.fillHeight: true
                            Layout.topMargin: Fonts.size2
                            Layout.bottomMargin: Fonts.size2
                            color: Theme.midColor
                        }
                        SegmentedButton {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 0
                            text: qsTr("1:1")
                            selected: zoomModeBar.currentIndex === 2
                            onClicked: zoomModeBar.activate(2)
                        }
                    }
                }
            }
            CustomLabel {
                Layout.alignment: Qt.AlignVCenter
                visible: root.presenter && root.presenter.browseMode !== "grid" && root.presenter.previewLoading
                text: qsTr("Loading…")
                color: Theme.placeholderTextColor
            }
        }

        CustomTextField {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.rightMargin: Fonts.standardMargin
            Layout.bottomMargin: Fonts.size8
            Layout.preferredHeight: Fonts.inputFieldHeight
            Layout.maximumHeight: Fonts.inputFieldHeight
            visible: !root.developOpen
            showEmptyIndicator: false
            showClipIndicator: false
            alignRightWhenFocused: false
            leftPadding: Fonts.size6
            rightPadding: Fonts.size6
            placeholderText: qsTr("Filter by tag")
            text: root.presenter ? root.presenter.tagFilter : ""
            onEditingFinished: if (root.commands)
                root.commands.setTagFilter(text)
        }

        Item {
            id: lastImportRow
            objectName: "lastImportGroup"
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.preferredHeight: visible ? Fonts.listItemHeight : 0
            visible: !root.developOpen && root.presenter && root.presenter.lastImportAvailable

            Rectangle {
                anchors.fill: parent
                color: root.presenter && root.presenter.lastImportSelected ? Theme.buttonHoveredColor : lastImportMouse.containsMouse ? Theme.buttonHoveredColor : Theme.alternateBaseColor
                border.color: root.presenter && root.presenter.lastImportSelected ? Theme.highlightColor : Theme.dividerColor
                border.width: root.presenter && root.presenter.lastImportSelected ? ControlState.borderFocus : ControlState.borderThin
                radius: ControlState.radiusSmall
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.size8
                anchors.rightMargin: Fonts.size8
                spacing: Fonts.size8

                CustomLabel {
                    text: "◷"
                    color: Theme.highlightColor
                    font.pixelSize: Fonts.size16
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Last Imported Photos")
                    font.bold: root.presenter && root.presenter.lastImportSelected
                    elide: Text.ElideRight
                }
                CustomLabel {
                    text: root.presenter ? String(root.presenter.lastImportCount) : "0"
                    color: Theme.placeholderTextColor
                }
            }

            MouseArea {
                id: lastImportMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.librarySelectLastImport)
            }
        }

        ColumnLayout {
            id: librarySetsPanel
            objectName: "librarySetsPanel"
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            spacing: Fonts.size6
            visible: !root.developOpen && root.presenter && root.presenter.catalogOpen

            CustomLabel {
                text: qsTr("Collections")
                font.bold: true
            }

            CustomTextField {
                id: newSetNameField
                objectName: "newSetNameField"
                Layout.fillWidth: true
                Layout.preferredHeight: Fonts.inputFieldHeight
                Layout.maximumHeight: Fonts.inputFieldHeight
                showEmptyIndicator: false
                showClipIndicator: false
                alignRightWhenFocused: false
                leftPadding: Fonts.size6
                rightPadding: Fonts.size6
                placeholderText: qsTr("Collection name")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Fonts.size6
                CustomButton {
                    objectName: "createManualSetButton"
                    Layout.fillWidth: true
                    text: qsTr("Collection")
                    onClicked: if (root.commands) {
                        const name = newSetNameField.text.trim().length > 0 ? newSetNameField.text.trim() : qsTr("Collection")
                        root.commands.run(root.commands.ids.libraryCreateManualSet, name)
                    }
                }
                CustomButton {
                    objectName: "createSmartSetButton"
                    Layout.fillWidth: true
                    text: qsTr("Smart")
                    onClicked: if (root.commands) {
                        const name = newSetNameField.text.trim().length > 0 ? newSetNameField.text.trim() : qsTr("Smart Collection")
                        root.commands.run(root.commands.ids.libraryCreateSmartSet, name)
                    }
                }
            }

            ListView {
                id: librarySetList
                objectName: "librarySetList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, Fonts.listItemHeight * 6)
                clip: true
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                model: root.presenter ? root.presenter.librarySets : null

                delegate: Item {
                    id: setRow
                    required property string setId
                    required property string kind
                    required property string name
                    required property int assetCount
                    required property bool selected
                    required property int index
                    width: ListView.view.width
                    height: Fonts.listItemHeight

                    Rectangle {
                        anchors.fill: parent
                        color: setRow.selected ? Theme.buttonHoveredColor : setMouse.containsMouse ? Theme.buttonHoveredColor : setRow.index % 2 === 0 ? Theme.alternateBaseColor : Theme.railSurfaceColor
                        border.color: setRow.selected ? Theme.highlightColor : Theme.dividerColor
                        border.width: setRow.selected ? ControlState.borderFocus : ControlState.borderThin
                        radius: ControlState.radiusSmall
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Fonts.size8
                        anchors.rightMargin: Fonts.size8
                        spacing: Fonts.size8
                        CustomLabel {
                            text: setRow.kind === "smart" ? "◎" : "◻"
                            color: Theme.highlightColor
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: setRow.name
                            font.bold: setRow.selected
                            elide: Text.ElideRight
                        }
                        CustomLabel {
                            text: String(setRow.assetCount)
                            color: Theme.placeholderTextColor
                        }
                    }

                    MouseArea {
                        id: setMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        cursorShape: Qt.PointingHandCursor
                        onClicked: function (mouse) {
                            if (!root.commands)
                                return
                            if (mouse.button === Qt.RightButton) {
                                setMenu.popup()
                                return
                            }
                            root.commands.run(root.commands.ids.librarySelectSet, setRow.setId)
                        }
                    }

                    Menu {
                        id: setMenu
                        Action {
                            text: qsTr("Add selected photos")
                            enabled: root.presenter && root.presenter.selectedCount > 0 && setRow.kind === "manual"
                            onTriggered: root.commands.run(root.commands.ids.libraryAddSelectionToSet, setRow.setId)
                        }
                        Action {
                            text: qsTr("Remove selected photos")
                            enabled: root.presenter && root.presenter.selectedCount > 0 && setRow.kind === "manual"
                            onTriggered: root.commands.run(root.commands.ids.libraryRemoveSelectionFromSet, setRow.setId)
                        }
                        Action {
                            text: qsTr("Delete collection")
                            onTriggered: root.commands.run(root.commands.ids.libraryDeleteSet, setRow.setId)
                        }
                    }
                }
            }
        }

        ListView {
            id: folderList
            Layout.fillWidth: true
            Layout.fillHeight: !root.developOpen
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            visible: !root.developOpen
            clip: true
            spacing: 0
            boundsBehavior: Flickable.StopAtBounds
            model: root.presenter ? root.presenter.folders : null

            delegate: Item {
                id: folderRow
                required property string folderId
                required property string folderUri
                required property string displayName
                required property int depth
                required property int assetCount
                required property bool hasChildren
                required property bool hasNextSibling
                required property var ancestorLineContinues
                required property bool collapsed
                required property bool missing
                required property int index

                readonly property int treeDepth: Math.max(0, folderRow.depth)
                readonly property int junctionY: Math.round(height / 2)
                readonly property color treeGuideColor: Theme.placeholderTextColor

                width: ListView.view.width
                height: Fonts.listItemHeight
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: {
                        if (root.presenter && !root.presenter.lastImportSelected && root.presenter.selectedLibrarySetId.length === 0 && folderRow.folderUri === root.presenter.selectedFolderUri)
                            return Theme.buttonHoveredColor;
                        if (folderMouse.containsMouse)
                            return Theme.buttonHoveredColor;
                        return folderRow.index % 2 === 0 ? Theme.alternateBaseColor : Theme.railSurfaceColor;
                    }
                }

                Item {
                    id: treeGuide
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: (folderRow.treeDepth + 1) * root.treeColumnWidth
                    visible: folderRow.treeDepth > 0 || folderRow.hasChildren

                    Repeater {
                        model: Math.max(0, folderRow.treeDepth - 1)
                        delegate: Rectangle {
                            required property int index
                            readonly property int lineX: index * root.treeColumnWidth + root.treeGuideX
                            x: lineX
                            y: 0
                            width: 1
                            height: parent.height
                            visible: Boolean(folderRow.ancestorLineContinues[index])
                            color: folderRow.treeGuideColor
                            opacity: 0.7
                        }
                    }

                    Rectangle {
                        readonly property int lineX: (folderRow.treeDepth - 1) * root.treeColumnWidth + root.treeGuideX
                        x: lineX
                        y: 0
                        width: 1
                        height: folderRow.hasNextSibling ? parent.height : folderRow.junctionY + 1
                        visible: folderRow.treeDepth > 0
                        color: folderRow.treeGuideColor
                        opacity: 0.7
                    }

                    Rectangle {
                        readonly property int startX: (folderRow.treeDepth - 1) * root.treeColumnWidth + root.treeGuideX
                        readonly property int endX: folderRow.treeDepth * root.treeColumnWidth + root.treeGuideX
                        x: startX
                        y: folderRow.junctionY
                        width: Math.max(1, endX - startX + 1)
                        height: 1
                        visible: folderRow.treeDepth > 0
                        color: folderRow.treeGuideColor
                        opacity: 0.7
                    }

                    Rectangle {
                        readonly property int tipY: folderRow.junctionY + Math.ceil(Fonts.size12 / 2) - 1
                        x: folderRow.treeDepth * root.treeColumnWidth + root.treeGuideX
                        y: tipY
                        width: 1
                        height: Math.max(0, parent.height - tipY)
                        visible: folderRow.hasChildren && !folderRow.collapsed
                        color: folderRow.treeGuideColor
                        opacity: 0.7
                    }
                }

                Item {
                    id: disclosure
                    visible: folderRow.hasChildren
                    x: folderRow.treeDepth * root.treeColumnWidth
                    width: root.treeColumnWidth
                    height: parent.height
                    z: 3

                    Canvas {
                        id: disclosureMark
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        width: Fonts.size12
                        height: Fonts.size12
                        Component.onCompleted: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d");
                            const mid = Math.floor(width / 2);
                            ctx.reset();
                            ctx.fillStyle = Theme.textColor;
                            ctx.beginPath();
                            if (folderRow.collapsed) {
                                ctx.moveTo(2, 1);
                                ctx.lineTo(width - 1, mid);
                                ctx.lineTo(2, height - 1);
                            } else {
                                ctx.moveTo(1, 2);
                                ctx.lineTo(width - 1, 2);
                                ctx.lineTo(mid, height - 1);
                            }
                            ctx.closePath();
                            ctx.fill();
                        }
                    }

                    Connections {
                        target: folderRow
                        function onCollapsedChanged() {
                            disclosureMark.requestPaint();
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (root.presenter)
                            root.presenter.folders.toggleCollapsed(folderRow.folderUri)
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Fonts.size8 + (folderRow.treeDepth + 1) * root.treeColumnWidth
                    anchors.rightMargin: Fonts.size8
                    spacing: Fonts.size8

                    CustomLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        wrapMode: Text.NoWrap
                        maximumLineCount: 1
                        text: {
                            if (folderRow.folderUri.length === 0)
                                return qsTr("All Photographs");
                            return folderRow.missing ? qsTr("%1 (missing — click to locate)").arg(folderRow.displayName) : folderRow.displayName;
                        }
                        color: folderRow.missing ? Theme.errorColor : Theme.textColor
                    }
                    CustomLabel {
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                        Layout.preferredWidth: implicitWidth
                        text: String(folderRow.assetCount)
                        color: Theme.placeholderTextColor
                    }
                }

                MouseArea {
                    id: folderMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.commands) {
                        if (folderRow.missing && folderRow.folderId.length > 0)
                            root.commands.run(root.commands.ids.libraryFolderRelink, folderRow.folderId);
                        else
                            root.commands.run(root.commands.ids.librarySelectFolder, folderRow.folderUri);
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            visible: !root.developOpen
            color: Theme.dividerColor
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.topMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            visible: !root.developOpen
            spacing: Fonts.smallSpacing

            CustomButton {
                Layout.fillWidth: true
                text: qsTr("Import…")
                enabled: root.commands && root.presenter && root.presenter.catalogOpen && !root.presenter.busy && !root.presenter.importWorkActive
                onClicked: if (root.commands)
                    root.commands.importPhotos.trigger()
            }
            CustomButton {
                Layout.fillWidth: true
                text: qsTr("Export…")
                enabled: root.commands && root.presenter && root.presenter.catalogOpen && !root.presenter.busy && !root.presenter.importWorkActive && root.presenter.selectedAssetId.length > 0
                onClicked: if (root.commands)
                    root.commands.exportPhoto.trigger()
            }
        }

        DevelopPresetPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.scaledUiSize(176)
            Layout.minimumHeight: Fonts.scaledUiSize(120)
            visible: root.developOpen
            presenter: root.presenter
            commands: root.commands
        }

        DevelopHistoryPanel {
            Layout.fillWidth: true
            Layout.fillHeight: root.developOpen
            visible: root.developOpen
            presenter: root.presenter
            commands: root.commands
        }
    }
}
