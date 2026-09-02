import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var commands
    property var colorChoices: []
    property var swatchColor: function (name) {
        return Theme.midColor;
    }

    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    readonly property bool gridOpen: hasPresenter && presenter.browseMode === "grid"
    readonly property bool developOpen: hasPresenter && presenter.browseMode === "develop"

    color: Theme.toolbarSurfaceColor
    implicitHeight: Math.max(Fonts.toolbarHeight, Fonts.inputFieldHeight + Fonts.size12)
    clip: true

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.dividerColor
        z: 2
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Fonts.standardMargin
        anchors.rightMargin: Fonts.standardMargin
        spacing: Fonts.smallSpacing

        Flickable {
            id: leadingTools
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick
            contentWidth: Math.max(width, leadingRow.implicitWidth)
            contentHeight: height
            interactive: contentWidth > width + 1
            ScrollBar.horizontal: ScrollBar {
                policy: leadingTools.contentWidth > leadingTools.width + 1 ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                implicitHeight: 8
            }

            WheelHandler {
                target: leadingTools
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function (event) {
                    const horizontal = event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.angleDelta.x;
                    const vertical = event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y;
                    const delta = horizontal !== 0 ? horizontal : vertical;
                    if (delta === 0) {
                        event.accepted = false;
                        return;
                    }
                    const maximum = Math.max(0, leadingTools.contentWidth - leadingTools.width);
                    const next = Math.max(0, Math.min(maximum, leadingTools.contentX - delta));
                    if (next === leadingTools.contentX) {
                        event.accepted = false;
                        return;
                    }
                    leadingTools.contentX = next;
                    event.accepted = true;
                }
            }

            RowLayout {
                id: leadingRow
                height: leadingTools.height
                spacing: Fonts.smallSpacing

                CustomButton {
                    id: comparisonButton
                    objectName: "beforeAfterComparisonButton"
                    Layout.alignment: Qt.AlignVCenter
                    visible: root.developOpen
                    action: root.commands ? root.commands.comparison : null
                    text: qsTr("Y|Y")
                    tooltipText: action ? action.text : ""
                    Accessible.name: tooltipText
                }

                CustomButton {
                    Layout.alignment: Qt.AlignVCenter
                    visible: root.gridOpen || (root.hasPresenter && root.presenter.browseMode === "survey")
                    text: qsTr("Survey")
                    enabled: root.hasPresenter && root.presenter.selectedCount >= 2
                    onClicked: if (root.commands)
                        root.commands.run(root.commands.ids.viewSurvey)
                }
                CustomButton {
                    Layout.alignment: Qt.AlignVCenter
                    visible: root.gridOpen
                    text: qsTr("Virtual Copy")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.run(root.commands.ids.photoCreateVersion)
                }
                CustomButton {
                    Layout.alignment: Qt.AlignVCenter
                    visible: root.gridOpen
                    text: qsTr("Stack")
                    enabled: root.hasPresenter && root.presenter.selectedCount >= 2
                    onClicked: if (root.commands)
                        root.commands.run(root.commands.ids.photoStackSelection)
                }

                RowLayout {
                    visible: root.gridOpen
                    spacing: Fonts.smallSpacing
                    Layout.alignment: Qt.AlignVCenter

                    CustomLabel {
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("Size")
                    }
                    Item {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 160
                        Layout.minimumWidth: 96
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        implicitWidth: 160
                        implicitHeight: Fonts.inputFieldHeight
                        clip: true
                        CustomSlider {
                            anchors.fill: parent
                            from: 120
                            to: 320
                            stepSize: 10
                            value: root.hasPresenter ? root.presenter.thumbnailSize : 180
                            showTitle: false
                            showStepButton: false
                            showValueLabel: false
                            delayedCommit: false
                            onValueEdited: function (v) {
                                if (root.commands)
                                    root.commands.run(root.commands.ids.viewSetThumbnailSize, Math.round(v));
                            }
                            onValueCommitted: function (v) {
                                if (root.commands)
                                    root.commands.run(root.commands.ids.viewSetThumbnailSize, Math.round(v));
                            }
                        }
                    }
                }
            }
        }

        RatingControl {
            Layout.alignment: Qt.AlignVCenter
            enabled: root.hasSelection
            rating: root.hasPresenter ? root.presenter.selectedRating : 0
            onRatingChangedByUser: function (value) {
                if (root.commands)
                    root.commands.setRating(value);
            }
        }

        Repeater {
            model: ["none"].concat(root.colorChoices)
            delegate: Rectangle {
                required property string modelData
                Layout.alignment: Qt.AlignVCenter
                width: 18
                height: 18
                radius: 9
                color: root.swatchColor(modelData)
                border.width: root.hasPresenter && root.presenter.selectedColorLabel === modelData ? 2 : 1
                border.color: Theme.textColor
                opacity: root.hasSelection ? 1 : 0.45
                MouseArea {
                    anchors.fill: parent
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.setColorLabel(modelData)
                }
            }
        }

        SegmentedControl {
            Layout.alignment: Qt.AlignVCenter
            enabled: root.hasSelection
            model: [qsTr("Keep"), qsTr("Reject")]
            currentIndex: root.hasPresenter && root.presenter.selectedRejected ? 1 : 0
            onActivated: function (index) {
                if (!root.commands || !root.hasPresenter)
                    return;
                const rejected = root.presenter.selectedRejected;
                if ((index === 1 && !rejected) || (index === 0 && rejected))
                    root.commands.reject.trigger();
            }
        }

        CustomButton {
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("Previous")
            enabled: root.hasSelection && root.commands
            onClicked: if (root.commands)
                root.commands.previousPhoto.trigger()
        }
        CustomButton {
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("Next")
            enabled: root.hasSelection && root.commands
            onClicked: if (root.commands)
                root.commands.nextPhoto.trigger()
        }
    }
}
