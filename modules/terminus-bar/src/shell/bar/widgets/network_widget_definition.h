#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/network_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<NetworkWidget::Options>& networkWidgetDefinition();
