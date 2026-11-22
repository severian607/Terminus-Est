#include "shell/bar/widgets/notification_widget_definition.h"

const terminusbar::bar::WidgetDefinition<NotificationWidget::Options>& notificationWidgetDefinition() {
  using terminusbar::bar::field;
  using Options = NotificationWidget::Options;

  static const terminusbar::bar::WidgetDefinition<Options> definition{
      .type = "notifications",
      .fields = {
          field<&Options::hideWhenNoUnread>({
              .key = "hide_when_no_unread",
          }),
      },
  };
  return definition;
}
