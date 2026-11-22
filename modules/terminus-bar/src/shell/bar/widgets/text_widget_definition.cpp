#include "shell/bar/widgets/text_widget_definition.h"

const terminusbar::bar::WidgetDefinition<TextWidget::Options>& textWidgetDefinition() {
  using terminusbar::bar::field;
  using Options = TextWidget::Options;

  static const terminusbar::bar::WidgetDefinition<Options> definition{
      .type = "text",
      .fields = {
          field<&Options::text>({
              .key = "text",
          }),
      },
  };
  return definition;
}
