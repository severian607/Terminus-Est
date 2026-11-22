#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/privacy_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<PrivacyWidget::Options>& privacyWidgetDefinition();
