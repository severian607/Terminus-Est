#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/clock_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<ClockWidget::Options>& clockWidgetDefinition();
