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
    readonly property bool surveyWanted: root.gridOpen || (root.hasPresenter && root.presenter.browseMode === "survey")
    readonly property var collapseState: {
        const inner = Math.max(0, width - Fonts.standardMargin * 2);
        const spacing = Fonts.smallSpacing;
        const pack = function (widths) {
            let total = 0;
            let count = 0;
            for (let i = 0; i < widths.length; ++i) {
                const w = Number(widths[i]);
                if (w > 0) {
                    total += w;
                    count += 1;
                }
            }
            if (count > 1)
                total += spacing * (count - 1);
            return total;
        };
        const comparisonW = root.developOpen ? comparisonButton.implicitWidth : 0;
        const surveyW = root.surveyWanted ? surveyButton.implicitWidth : 0;
        const virtualCopyW = root.gridOpen ? virtualCopyButton.implicitWidth : 0;
        const stackW = root.gridOpen ? stackButton.implicitWidth : 0;
        const sizeW = root.gridOpen ? sizeRow.implicitWidth : 0;
        const navW = pack([previousButton.implicitWidth, nextButton.implicitWidth]);
        const overflowW = overflowButton.implicitWidth;
        const fullReviewW = pack([ratingControl.implicitWidth, colorSwatches.implicitWidth, keepReject.implicitWidth]);
        const compactReviewW = pack([compactRatingButton.implicitWidth, compactColorButton.implicitWidth, keepReject.implicitWidth]);
        const leadingW = function (showComparison, showSurvey, showVirtualCopy, showStack, showSize) {
            return pack([showComparison ? comparisonW : 0, showSurvey ? surveyW : 0, showVirtualCopy ? virtualCopyW : 0, showStack ? stackW : 0, showSize ? sizeW : 0]);
        };
        const fits = function (lead, showNav, showOverflow, compactReview) {
            return pack([showOverflow ? overflowW : 0, lead, showNav ? navW : 0, compactReview ? compactReviewW : fullReviewW]) <= inner;
        };
        const makeState = function (showComparison, showSurvey, showVirtualCopy, showStack, showSize, showNav, compactReview) {
            const overflow = (root.developOpen && !showComparison) || (root.surveyWanted && !showSurvey) || (root.gridOpen && (!showVirtualCopy || !showStack)) || !showNav;
            return {
                comparison: showComparison && root.developOpen,
                survey: showSurvey && root.surveyWanted,
                virtualCopy: showVirtualCopy && root.gridOpen,
                stack: showStack && root.gridOpen,
                size: showSize && root.gridOpen,
                navigation: showNav,
                compactReview: compactReview,
                overflow: overflow
            };
        };

        if (fits(leadingW(true, true, true, true, true), true, false, false))
            return makeState(true, true, true, true, true, true, false);
        if (fits(leadingW(true, true, true, true, true), true, false, true))
            return makeState(true, true, true, true, true, true, true);
        if (fits(leadingW(true, true, true, true, false), true, false, true))
            return makeState(true, true, true, true, false, true, true);
        if (fits(leadingW(true, true, false, false, false), true, true, true))
            return makeState(true, true, false, false, false, true, true);
        if (fits(leadingW(true, false, false, false, false), true, true, true))
            return makeState(true, false, false, false, false, true, true);
        if (fits(leadingW(true, false, false, false, false), false, true, true))
            return makeState(true, false, false, false, false, false, true);
        if (fits(leadingW(false, false, false, false, false), false, true, true))
            return makeState(false, false, false, false, false, false, true);
        return makeState(false, false, false, false, false, false, true);
    }
    readonly property bool showComparison: collapseState.comparison === true
    readonly property bool showSurvey: collapseState.survey === true
    readonly property bool showVirtualCopy: collapseState.virtualCopy === true
    readonly property bool showStack: collapseState.stack === true
    readonly property bool showSize: collapseState.size === true
    readonly property bool showNavigation: collapseState.navigation === true
    readonly property bool compactReview: collapseState.compactReview === true
    readonly property bool showOverflow: collapseState.overflow === true
    readonly property int selectedRating: root.hasPresenter ? root.presenter.selectedRating : 0
    readonly property string selectedColorLabel: root.hasPresenter ? root.presenter.selectedColorLabel : "none"

    color: Theme.toolbarSurfaceColor
    implicitHeight: Math.max(Fonts.toolbarHeight, Fonts.inputFieldHeight + Fonts.size12)
    clip: true

    onShowOverflowChanged: {
        if (!showOverflow)
            overflowMenu.close();
    }
    onCompactReviewChanged: {
        if (!compactReview) {
            compactRatingPopup.close();
            compactColorPopup.close();
        }
    }

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

        CustomButton {
            id: overflowButton
            objectName: "reviewBarOverflowButton"
            Layout.alignment: Qt.AlignVCenter
            visible: root.showOverflow
            text: "⋯"
            tooltipText: qsTr("More")
            Accessible.name: qsTr("More")
            onClicked: overflowMenu.popup()
        }

        RowLayout {
            id: leadingTools
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: false
            Layout.fillHeight: true
            spacing: Fonts.smallSpacing
            visible: comparisonButton.visible || surveyButton.visible || virtualCopyButton.visible || stackButton.visible || sizeRow.visible

            CustomButton {
                id: comparisonButton
                objectName: "beforeAfterComparisonButton"
                Layout.alignment: Qt.AlignVCenter
                visible: root.showComparison
                action: root.commands ? root.commands.comparison : null
                text: qsTr("Y|Y")
                tooltipText: action ? action.text : ""
                Accessible.name: tooltipText
            }

            CustomButton {
                id: surveyButton
                Layout.alignment: Qt.AlignVCenter
                visible: root.showSurvey
                text: qsTr("Survey")
                enabled: root.hasPresenter && root.presenter.selectedCount >= 2
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.viewSurvey)
            }
            CustomButton {
                id: virtualCopyButton
                Layout.alignment: Qt.AlignVCenter
                visible: root.showVirtualCopy
                text: qsTr("Virtual Copy")
                enabled: root.hasSelection
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.photoCreateVersion)
            }
            CustomButton {
                id: stackButton
                Layout.alignment: Qt.AlignVCenter
                visible: root.showStack
                text: qsTr("Stack")
                enabled: root.hasPresenter && root.presenter.selectedCount >= 2
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.photoStackSelection)
            }

            RowLayout {
                id: sizeRow
                visible: root.showSize
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

        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }

        RowLayout {
            id: reviewTools
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: false
            Layout.fillHeight: true
            spacing: Fonts.smallSpacing

            RatingControl {
                id: ratingControl
                Layout.alignment: Qt.AlignVCenter
                Layout.minimumWidth: implicitWidth
                visible: !root.compactReview
                enabled: root.hasSelection
                rating: root.selectedRating
                onRatingChangedByUser: function (value) {
                    if (root.commands)
                        root.commands.setRating(value);
                }
            }

            CustomButton {
                id: compactRatingButton
                Layout.alignment: Qt.AlignVCenter
                visible: root.compactReview
                enabled: root.hasSelection
                text: root.selectedRating > 0 ? "★" + root.selectedRating : "☆"
                tooltipText: qsTr("Rating")
                Accessible.name: qsTr("Rating %1").arg(root.selectedRating)
                onClicked: compactRatingPopup.open()
            }

            Row {
                id: colorSwatches
                Layout.alignment: Qt.AlignVCenter
                visible: !root.compactReview
                spacing: Fonts.smallSpacing

                Repeater {
                    model: ["none"].concat(root.colorChoices)
                    delegate: Rectangle {
                        required property string modelData
                        width: 18
                        height: 18
                        radius: 9
                        color: root.swatchColor(modelData)
                        border.width: root.selectedColorLabel === modelData ? 2 : 1
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
            }

            Rectangle {
                id: compactColorButton
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                visible: root.compactReview
                implicitWidth: 18
                implicitHeight: 18
                width: 18
                height: 18
                radius: 9
                color: root.swatchColor(root.selectedColorLabel)
                border.width: 1
                border.color: Theme.textColor
                opacity: root.hasSelection ? 1 : 0.45
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Color label")
                MouseArea {
                    anchors.fill: parent
                    enabled: root.hasSelection
                    onClicked: compactColorPopup.open()
                }
            }

            SegmentedControl {
                id: keepReject
                objectName: "cullReviewFlagControl"
                Layout.alignment: Qt.AlignVCenter
                Layout.minimumWidth: implicitWidth
                enabled: root.hasSelection
                model: [qsTr("Pick"), qsTr("Keep"), qsTr("Reject")]
                currentIndex: {
                    if (!root.hasPresenter)
                        return 1;
                    if (root.presenter.selectedPicked)
                        return 0;
                    if (root.presenter.selectedRejected)
                        return 2;
                    return 1;
                }
                onActivated: function (index) {
                    if (!root.commands || !root.hasPresenter)
                        return;
                    if (index === 0)
                        root.commands.pick.trigger();
                    else if (index === 2)
                        root.commands.reject.trigger();
                    else
                        root.commands.unflag.trigger();
                }
            }
        }

        CustomButton {
            id: previousButton
            Layout.alignment: Qt.AlignVCenter
            visible: root.showNavigation
            text: qsTr("Previous")
            enabled: root.hasSelection && root.commands
            onClicked: if (root.commands)
                root.commands.previousPhoto.trigger()
        }
        CustomButton {
            id: nextButton
            Layout.alignment: Qt.AlignVCenter
            visible: root.showNavigation
            text: qsTr("Next")
            enabled: root.hasSelection && root.commands
            onClicked: if (root.commands)
                root.commands.nextPhoto.trigger()
        }
    }

    StudioContextMenu {
        id: overflowMenu
        fitToContent: true
        StudioContextMenuItem {
            displayText: qsTr("Y|Y")
            visible: root.developOpen && !root.showComparison
            enabled: root.commands && root.commands.comparison && root.commands.comparison.enabled
            onTriggered: if (root.commands)
                root.commands.comparison.trigger()
        }
        StudioContextMenuItem {
            displayText: qsTr("Survey")
            visible: root.surveyWanted && !root.showSurvey
            enabled: root.hasPresenter && root.presenter.selectedCount >= 2
            onTriggered: if (root.commands)
                root.commands.run(root.commands.ids.viewSurvey)
        }
        StudioContextMenuItem {
            displayText: qsTr("Virtual Copy")
            visible: root.gridOpen && !root.showVirtualCopy
            enabled: root.hasSelection
            onTriggered: if (root.commands)
                root.commands.run(root.commands.ids.photoCreateVersion)
        }
        StudioContextMenuItem {
            displayText: qsTr("Stack")
            visible: root.gridOpen && !root.showStack
            enabled: root.hasPresenter && root.presenter.selectedCount >= 2
            onTriggered: if (root.commands)
                root.commands.run(root.commands.ids.photoStackSelection)
        }
        StudioContextMenuSeparator {
            visible: ((root.developOpen && !root.showComparison) || (root.surveyWanted && !root.showSurvey) || (root.gridOpen && (!root.showVirtualCopy || !root.showStack))) && !root.showNavigation
        }
        StudioContextMenuItem {
            displayText: qsTr("Previous")
            visible: !root.showNavigation
            enabled: root.hasSelection && root.commands
            onTriggered: if (root.commands)
                root.commands.previousPhoto.trigger()
        }
        StudioContextMenuItem {
            displayText: qsTr("Next")
            visible: !root.showNavigation
            enabled: root.hasSelection && root.commands
            onTriggered: if (root.commands)
                root.commands.nextPhoto.trigger()
        }
    }

    Popup {
        id: compactRatingPopup
        parent: compactRatingButton
        x: 0
        y: compactRatingButton.height + Fonts.size4
        padding: Fonts.size6
        modal: false
        dim: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: Theme.popupSurfaceColor
            border.color: Theme.dividerColor
            border.width: 1
            radius: 4
        }

        Row {
            spacing: Fonts.size4
            Repeater {
                model: 6
                delegate: Item {
                    required property int index
                    width: Fonts.size20
                    height: Fonts.size20
                    Text {
                        anchors.centerIn: parent
                        text: index === 0 ? "☆" : "★"
                        color: index === 0 ? Theme.placeholderTextColor : (index <= root.selectedRating ? Theme.textColor : Theme.placeholderTextColor)
                        font.pixelSize: Fonts.size16
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: root.hasSelection
                        onClicked: {
                            if (root.commands)
                                root.commands.setRating(index);
                            compactRatingPopup.close();
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: compactColorPopup
        parent: compactColorButton
        x: 0
        y: compactColorButton.height + Fonts.size4
        padding: Fonts.size6
        modal: false
        dim: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: Theme.popupSurfaceColor
            border.color: Theme.dividerColor
            border.width: 1
            radius: 4
        }

        Row {
            spacing: Fonts.smallSpacing
            Repeater {
                model: ["none"].concat(root.colorChoices)
                delegate: Rectangle {
                    required property string modelData
                    width: 18
                    height: 18
                    radius: 9
                    color: root.swatchColor(modelData)
                    border.width: root.selectedColorLabel === modelData ? 2 : 1
                    border.color: Theme.textColor
                    MouseArea {
                        anchors.fill: parent
                        enabled: root.hasSelection
                        onClicked: {
                            if (root.commands)
                                root.commands.setColorLabel(modelData);
                            compactColorPopup.close();
                        }
                    }
                }
            }
        }
    }
}
