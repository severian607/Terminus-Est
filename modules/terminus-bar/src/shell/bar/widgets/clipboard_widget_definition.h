#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/clipboard_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<ClipboardWidget::Options>& clipboardWidgetDefinition();
