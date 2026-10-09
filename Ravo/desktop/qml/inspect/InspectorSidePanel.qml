pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var commands
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool developOpen: hasPresenter && presenter.browseMode === "develop"
    property bool liveReady: false
    readonly property bool metadataEditing: photoInfo.metadataEditing
    readonly property real minimumToolWidth: toolBar.implicitWidth + 2 * Fonts.standardMargin
    property bool localWorkspaceOpen: false
    property string pendingTool: ""
    readonly property string selectedAsset: hasPresenter ? presenter.selectedAssetId : ""
    onSelectedAssetChanged: {
        pendingTool = "";
        localWorkspaceOpen = false;
    }
    readonly property string activeTool: developPanel.cropPinned ? "crop" : developPanel.localEditing || localWorkspaceOpen ? "local" : "edit"

    function selectTool(tool) {
        if (developPanel.localEditing && tool !== "local") {
            pendingTool = tool;
            const result = commands.localAdjustment("done", {});
            if (!result || !result.ok)
                pendingTool = "";
            return;
        }
        pendingTool = "";
        localWorkspaceOpen = tool === "local";
        commands.run(commands.ids.editCropTool, tool === "crop");
        developScroller.contentY = 0;
    }

    Connections {
        target: root.hasPresenter ? root.presenter.develop : null
        function onEditingScopeChanged() {
            if (!developPanel.localEditing && root.pendingTool.length > 0)
                Qt.callLater(function () {
                    if (!developPanel.localEditing && root.pendingTool.length > 0)
                        root.selectTool(root.pendingTool);
                });
        }
    }

    Component.onCompleted: liveReady = true

    color: Theme.railSurfaceColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ScopePanel {
            objectName: "cropScopePanel"
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.scaledUiSize(128)
            Layout.minimumHeight: Fonts.scaledUiSize(96)
            presenter: root.presenter
            commands: root.commands
        }

        RowLayout {
            id: toolBar
            objectName: "developToolBar"
            visible: root.developOpen
            Layout.fillWidth: true
            Layout.margins: Fonts.standardMargin
            spacing: Fonts.smallSpacing

            Repeater {
                model: [
                    {
                        tool: "edit",
                        label: qsTr("Edit")
                    },
                    {
                        tool: "crop",
                        label: qsTr("Crop")
                    },
                    {
                        tool: "local",
                        label: qsTr("Local adjustments")
                    }
                ]
                delegate: SegmentedButton {
                    required property var modelData
                    objectName: "developTool_" + modelData.tool
                    Layout.fillWidth: true
                    Layout.preferredWidth: implicitWidth
                    Layout.minimumWidth: implicitWidth
                    text: modelData.label
                    borderWidth: Fonts.size1
                    selected: root.activeTool === modelData.tool
                    enabled: developPanel.hasSelection && !root.presenter.develop.localDonePending
                    onClicked: root.selectTool(modelData.tool)
                }
            }
        }

        Flickable {
            objectName: "pinnedCropPanel"
            visible: root.developOpen && developPanel.cropPinned
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(cropControls.implicitHeight + 2 * Fonts.standardMargin, root.height * 0.55)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            contentWidth: width
            contentHeight: cropControls.implicitHeight + 2 * Fonts.standardMargin
            ScrollBar.vertical: CustomScrollBar {}
            DevelopCropControls {
                id: cropControls
                x: Fonts.standardMargin
                y: Fonts.standardMargin
                width: parent.width - 2 * Fonts.standardMargin
                panel: developPanel
                pinned: true
            }
        }

        Flickable {
            id: localScroller
            objectName: "pinnedLocalPanel"
            visible: root.developOpen && root.activeTool === "local"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Math.max(0, root.height - y) * 0.5)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            contentWidth: width
            contentHeight: localControls.implicitHeight + 2 * Fonts.standardMargin
            ScrollBar.vertical: CustomScrollBar {}

            LocalAdjustmentWorkspace {
                id: localControls
                x: Fonts.standardMargin
                y: Fonts.standardMargin
                width: localScroller.width - 2 * Fonts.standardMargin
                panel: developPanel
            }
        }

        Flickable {
            id: developScroller
            objectName: "developPanelScroller"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            contentWidth: width
            contentHeight: column.implicitHeight
            ScrollBar.vertical: CustomScrollBar {}

            ColumnLayout {
                id: column
                width: parent.width
                spacing: Fonts.smallSpacing

                PhotoInfoPanel {
                    id: photoInfo
                    visible: !root.developOpen
                    Layout.fillWidth: true
                    presenter: root.presenter
                    commands: root.commands
                }

                DevelopPanel {
                    id: developPanel
                    scrollViewport: developScroller
                    workspace: root.activeTool
                    visible: root.developOpen
                    Layout.fillWidth: true
                    presenter: root.presenter
                    commands: root.commands
                    liveReady: root.liveReady
                }
            }
        }
    }
}
