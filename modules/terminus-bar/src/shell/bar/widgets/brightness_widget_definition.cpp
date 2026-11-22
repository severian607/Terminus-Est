#include "shell/bar/widgets/brightness_widget_definition.h"

const terminusbar::bar::WidgetDefinition<BrightnessWidget::Options>& brightnessWidgetDefinition() {
  using terminusbar::bar::field;
  using Options = BrightnessWidget::Options;

  static const terminusbar::bar::WidgetDefinition<Options> definition{
      .type = "brightness",
      .fields = {
          field<&Options::showLabel>({
              .key = "show_label",
          }),
          field<&Options::showWhenUnavailable>({
              .key = "show_when_unavailable",
          }),
      },
  };
  return definition;
}
