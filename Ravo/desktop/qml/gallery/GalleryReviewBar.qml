import QtQuick
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

    color: Theme.toolbarSurfaceColor
    implicitHeight: Math.max(Fonts.toolbarHeight, Fonts.inputFieldHeight + Fonts.size12)

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.dividerColor
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Fonts.standardMargin
        anchors.rightMargin: Fonts.standardMargin
        spacing: Fonts.smallSpacing

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
                        if (root.hasPresenter)
                            root.presenter.thumbnailSize = Math.round(v);
                    }
                    onValueCommitted: function (v) {
                        if (root.hasPresenter)
                            root.presenter.thumbnailSize = Math.round(v);
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
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
