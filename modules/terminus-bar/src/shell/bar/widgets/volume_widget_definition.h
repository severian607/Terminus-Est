#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/volume_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<VolumeWidget::Options>& volumeWidgetDefinition();
