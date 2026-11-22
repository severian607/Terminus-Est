#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/weather_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<WeatherWidget::Options>& weatherWidgetDefinition();
