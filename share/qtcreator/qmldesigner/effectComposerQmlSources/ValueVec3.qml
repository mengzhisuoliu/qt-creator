// Copyright (C) 2023 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

import QtQuick
import QtQuick.Layouts
import HelperWidgets as HelperWidgets
import StudioTheme as StudioTheme

RowLayout {
    id: root

    width: parent.width
    spacing: 0

    signal valueChanged()

    HelperWidgets.DoubleSpinBox {
        id: vX

        // value: uniformValue binding can get overwritten by normal operation of the control
        property double resetValue: uniformValue.x
        onResetValueChanged: value = resetValue

        Layout.fillWidth: true
        Layout.minimumWidth: 30
        Layout.maximumWidth: 60

        spinBoxIndicatorVisible: false
        inputHAlignment: Qt.AlignHCenter
        minimumValue: uniformMinValue.x
        maximumValue: uniformMaxValue.x
        value: uniformValue.x
        stepSize: .01
        decimals: 2
        onValueModified: {
            uniformValue.x = value
            root.valueChanged()
        }
    }

    Item { // spacer
        Layout.fillWidth: true
        Layout.minimumWidth: 2
        Layout.maximumWidth: 10
    }

    Text {
        text: qsTr("X")
        color: StudioTheme.Values.themeTextColor
        font.pixelSize: StudioTheme.Values.baseFontSize
        Layout.alignment: Qt.AlignVCenter
    }

    Item { // spacer
        Layout.fillWidth: true
        Layout.minimumWidth: 10
        Layout.maximumWidth: 20
    }

    HelperWidgets.DoubleSpinBox {
        id: vY

        // value: uniformValue binding can get overwritten by normal operation of the control
        property double resetValue: uniformValue.y
        onResetValueChanged: value = resetValue

        Layout.fillWidth: true
        Layout.minimumWidth: 30
        Layout.maximumWidth: 60

        spinBoxIndicatorVisible: false
        inputHAlignment: Qt.AlignHCenter
        minimumValue: uniformMinValue.y
        maximumValue: uniformMaxValue.y
        value: uniformValue.y
        stepSize: .01
        decimals: 2
        onValueModified: {
            uniformValue.y = value
            root.valueChanged()
        }
    }

    Item { // spacer
        Layout.fillWidth: true
        Layout.minimumWidth: 2
        Layout.maximumWidth: 10
    }

    Text {
        text: qsTr("Y")
        color: StudioTheme.Values.themeTextColor
        font.pixelSize: StudioTheme.Values.baseFontSize
        Layout.alignment: Qt.AlignVCenter
    }

    Item { // spacer
        Layout.fillWidth: true
        Layout.minimumWidth: 10
        Layout.maximumWidth: 20
    }

    HelperWidgets.DoubleSpinBox {
        id: vZ

        // value: uniformValue binding can get overwritten by normal operation of the control
        property double resetValue: uniformValue.z
        onResetValueChanged: value = resetValue

        Layout.fillWidth: true
        Layout.minimumWidth: 30
        Layout.maximumWidth: 60

        spinBoxIndicatorVisible: false
        inputHAlignment: Qt.AlignHCenter
        minimumValue: uniformMinValue.z
        maximumValue: uniformMaxValue.z
        value: uniformValue.z
        stepSize: .01
        decimals: 2
        onValueModified: {
            uniformValue.z = value
            root.valueChanged()
        }
    }

    Item { // spacer
        Layout.fillWidth: true
        Layout.minimumWidth: 2
        Layout.maximumWidth: 10
    }

    Text {
        text: qsTr("Z")
        color: StudioTheme.Values.themeTextColor
        font.pixelSize: StudioTheme.Values.baseFontSize
        Layout.alignment: Qt.AlignVCenter
    }

    Item { // spacer
        Layout.fillWidth: true
        Layout.minimumWidth: 10
    }

}
