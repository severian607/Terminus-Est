#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/bluetooth_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<BluetoothWidget::Options>& bluetoothWidgetDefinition();
