#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/workspaces_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<WorkspacesWidget::Options>& workspacesWidgetDefinition();
