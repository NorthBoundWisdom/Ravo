import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property real viewRectX: 0
    property real viewRectY: 0
    property real viewRectW: 1
    property real viewRectH: 1
    color: Theme.railSurfaceColor

    readonly property int treeColumnWidth: Fonts.size16
    signal viewportSeeked(real nx, real ny)

    Connections {
        target: root.presenter
        function onSelectionChanged() {
            if (root.presenter && root.presenter.selectedAssetId.length > 0)
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
            text: qsTr("Library")
            font.bold: true
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

            SegmentedControl {
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
                model: [qsTr("Fit"), qsTr("Fill"), qsTr("100%")]
                currentIndex: root.presenter && root.presenter.zoomMode === "fill" ? 1 : (root.presenter && root.presenter.zoomMode === "actual" ? 2 : 0)
                enabled: root.presenter && root.presenter.browseMode !== "grid" && root.presenter.previewUrl.toString().length > 0
                onActivated: function (index) {
                    if (!root.presenter)
                        return;
                    root.presenter.setZoomMode(index === 1 ? "fill" : (index === 2 ? "actual" : "fit"));
                }
            }
            CustomLabel {
                Layout.alignment: Qt.AlignVCenter
                visible: root.presenter && root.presenter.browseMode !== "grid"
                text: root.presenter && root.presenter.previewLoading ? qsTr("Loading…") : (root.presenter ? (Math.round(root.presenter.zoomFactor * 100) + "%") : "")
                color: Theme.placeholderTextColor
            }
        }

        TextField {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.rightMargin: Fonts.standardMargin
            Layout.bottomMargin: Fonts.size8
            placeholderText: qsTr("Filter by tag")
            text: root.presenter ? root.presenter.tagFilter : ""
            onEditingFinished: if (root.presenter)
                root.presenter.setTagFilter(text)
        }

        ListView {
            id: folderList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            clip: true
            spacing: Fonts.size2
            boundsBehavior: Flickable.StopAtBounds
            model: root.presenter ? root.presenter.folders : null

            delegate: Item {
                id: folderRow
                required property string folderUri
                required property string displayName
                required property int depth
                required property int assetCount
                required property bool hasChildren
                required property bool hasNextSibling
                required property var ancestorLineContinues
                required property int index

                readonly property int treeDepth: Math.max(0, folderRow.depth)
                readonly property int junctionY: Math.round(height / 2)
                readonly property color treeGuideColor: Theme.placeholderTextColor

                width: ListView.view.width
                height: Fonts.listItemHeight
                clip: true

                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: {
                        if (root.presenter && folderRow.folderUri === root.presenter.selectedFolderUri)
                            return Theme.buttonHoveredColor;
                        if (folderMouse.containsMouse)
                            return Theme.buttonHoveredColor;
                        return folderRow.index % 2 === 0 ? Theme.baseColor : "transparent";
                    }
                }

                Item {
                    id: treeGuide
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: (folderRow.treeDepth + (folderRow.hasChildren ? 1 : 0)) * root.treeColumnWidth
                    visible: folderRow.treeDepth > 0

                    Repeater {
                        model: folderRow.treeDepth
                        delegate: Item {
                            required property int index
                            readonly property bool currentLevel: index === folderRow.treeDepth - 1
                            readonly property int lineX: Math.floor(root.treeColumnWidth / 2)

                            x: index * root.treeColumnWidth
                            width: root.treeColumnWidth
                            height: treeGuide.height

                            Rectangle {
                                x: parent.lineX
                                y: 0
                                width: 1
                                height: parent.currentLevel && !folderRow.hasNextSibling ? folderRow.junctionY + 1 : parent.height
                                visible: parent.currentLevel || Boolean(folderRow.ancestorLineContinues[parent.index])
                                color: folderRow.treeGuideColor
                                opacity: 0.7
                            }

                            Rectangle {
                                x: parent.lineX
                                y: folderRow.junctionY
                                width: parent.currentLevel ? root.treeColumnWidth - parent.lineX : 0
                                height: 1
                                visible: parent.currentLevel
                                color: folderRow.treeGuideColor
                                opacity: 0.7
                            }
                        }
                    }

                    Rectangle {
                        x: folderRow.treeDepth * root.treeColumnWidth + Math.floor(root.treeColumnWidth / 2)
                        y: folderRow.junctionY
                        width: 1
                        height: treeGuide.height - folderRow.junctionY
                        visible: folderRow.hasChildren
                        color: folderRow.treeGuideColor
                        opacity: 0.7
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Fonts.size8 + folderRow.treeDepth * root.treeColumnWidth
                    anchors.rightMargin: Fonts.size8
                    spacing: Fonts.size8

                    CustomLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        wrapMode: Text.NoWrap
                        maximumLineCount: 1
                        text: folderRow.folderUri.length === 0 ? qsTr("All Photographs") : folderRow.displayName
                        color: Theme.textColor
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
                    onClicked: if (root.presenter)
                        root.presenter.selectFolder(folderRow.folderUri)
                }
            }
        }
    }
}
