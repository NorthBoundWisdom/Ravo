// Test host: production ImportPhotoGrid via Ravo.Studio + GeoControls import roots.
import QtQuick
import Ravo.Studio 1.0

Item {
    id: root
    width: 800
    height: 600
    required property var presenter

    ImportPhotoGrid {
        objectName: "importPhotoGridHost"
        anchors.fill: parent
        presenter: root.presenter
    }
}
