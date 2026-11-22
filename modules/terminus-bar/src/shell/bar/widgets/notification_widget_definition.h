#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/notification_widget.h"

[[nodiscard]] const terminusbar::bar::WidgetDefinition<NotificationWidget::Options>& notificationWidgetDefinition();
