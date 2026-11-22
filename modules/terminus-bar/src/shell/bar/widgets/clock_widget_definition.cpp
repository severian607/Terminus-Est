#include "shell/bar/widgets/clock_widget_definition.h"

const terminusbar::bar::WidgetDefinition<ClockWidget::Options>& clockWidgetDefinition() {
  using terminusbar::bar::field;
  using Options = ClockWidget::Options;

  static const terminusbar::bar::WidgetDefinition<Options> definition{
      .type = "clock",
      .fields = {
          field<&Options::format>({
              .key = "format",
          }),
          field<&Options::verticalFormat>({
              .key = "vertical_format",
          }),
          field<&Options::tooltipFormat>({
              .key = "tooltip_format",
          }),
          field<&Options::timezone>({
              .key = "timezone",
          }),
      },
  };
  return definition;
}
