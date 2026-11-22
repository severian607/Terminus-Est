#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/control_center_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<ControlCenterWidget::Options>& controlCenterWidgetDefinition();
