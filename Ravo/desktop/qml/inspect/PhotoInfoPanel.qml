import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    property var presenter
    property var commands
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    spacing: Fonts.smallSpacing

    function infoRow(label, value) {
        return label + ": " + (value && value.length ? value : "—");
    }

    component MetaField: CustomTextField {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        Layout.preferredHeight: Fonts.inputFieldHeight
        Layout.maximumHeight: Fonts.inputFieldHeight
        showEmptyIndicator: false
        showClipIndicator: false
        alignRightWhenFocused: false
        leftPadding: Fonts.size6
        rightPadding: Fonts.size6
        enabled: root.hasSelection
    }

    CustomLabel {
        Layout.leftMargin: Fonts.standardMargin
        Layout.topMargin: Fonts.size12
        text: qsTr("Photo")
        font.bold: true
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        wrapMode: Text.WrapAnywhere
        elide: Text.ElideMiddle
        text: root.presenter && root.presenter.selectedDisplayName.length ? root.presenter.selectedDisplayName : qsTr("No photo selected")
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        wrapMode: Text.WrapAnywhere
        color: Theme.placeholderTextColor
        text: root.infoRow(qsTr("Folder"), root.presenter ? root.presenter.selectedFolderPath : "")
    }
    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        color: Theme.placeholderTextColor
        text: root.infoRow(qsTr("Type"), root.presenter ? root.presenter.selectedMediaType : "")
    }
    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        color: Theme.placeholderTextColor
        text: root.infoRow(qsTr("Size"), root.presenter ? root.presenter.selectedDimensions : "")
    }
    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        color: Theme.placeholderTextColor
        text: root.infoRow(qsTr("File"), root.presenter ? root.presenter.selectedFileSize : "")
    }
    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        color: Theme.placeholderTextColor
        text: root.presenter && root.presenter.selectedHasEdits ? qsTr("Edited") : qsTr("No edits")
    }
    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        wrapMode: Text.WrapAnywhere
        color: Theme.placeholderTextColor
        font.pixelSize: Fonts.size10
        text: root.presenter ? root.presenter.selectedUri : ""
    }
    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        wrapMode: Text.Wrap
        color: Theme.placeholderTextColor
        text: root.infoRow(qsTr("Capture"), root.presenter ? root.presenter.selectedCaptureSummary : "")
    }

    CustomLabel {
        Layout.leftMargin: Fonts.standardMargin
        Layout.topMargin: Fonts.size8
        text: qsTr("Tags & Metadata")
        font.bold: true
    }

    MetaField {
        placeholderText: qsTr("keywords, comma separated; use | for hierarchy")
        text: root.hasPresenter ? root.presenter.selectedTags : ""
        onEditingFinished: if (root.commands)
            root.commands.setTags(text)
    }
    MetaField {
        placeholderText: qsTr("Title")
        text: root.hasPresenter ? root.presenter.selectedTitle : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("title", text)
    }
    MetaField {
        placeholderText: qsTr("Headline")
        text: root.hasPresenter ? root.presenter.selectedHeadline : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("headline", text)
    }
    MetaField {
        placeholderText: qsTr("Description")
        text: root.hasPresenter ? root.presenter.selectedDescription : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("description", text)
    }
    MetaField {
        placeholderText: qsTr("Creator")
        text: root.hasPresenter ? root.presenter.selectedCreator : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("creator", text)
    }
    MetaField {
        placeholderText: qsTr("Copyright")
        text: root.hasPresenter ? root.presenter.selectedCopyright : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("copyright", text)
    }
    MetaField {
        placeholderText: qsTr("Credit")
        text: root.hasPresenter ? root.presenter.selectedCredit : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("credit", text)
    }
    MetaField {
        placeholderText: qsTr("Source")
        text: root.hasPresenter ? root.presenter.selectedSource : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("source", text)
    }
    MetaField {
        placeholderText: qsTr("Instructions")
        text: root.hasPresenter ? root.presenter.selectedInstructions : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("instructions", text)
    }
    MetaField {
        placeholderText: qsTr("Usage Terms")
        text: root.hasPresenter ? root.presenter.selectedUsageTerms : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("usage_terms", text)
    }
    MetaField {
        placeholderText: qsTr("Job ID")
        text: root.hasPresenter ? root.presenter.selectedJobId : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("job_id", text)
    }
    MetaField {
        placeholderText: qsTr("Country")
        text: root.hasPresenter ? root.presenter.selectedCountry : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("country", text)
    }
    MetaField {
        placeholderText: qsTr("Province / State")
        text: root.hasPresenter ? root.presenter.selectedProvinceState : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("province_state", text)
    }
    MetaField {
        placeholderText: qsTr("City")
        text: root.hasPresenter ? root.presenter.selectedCity : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("city", text)
    }
    MetaField {
        placeholderText: qsTr("Sublocation")
        text: root.hasPresenter ? root.presenter.selectedSublocation : ""
        onEditingFinished: if (root.commands)
            root.commands.setMetadata("sublocation", text)
    }
}
