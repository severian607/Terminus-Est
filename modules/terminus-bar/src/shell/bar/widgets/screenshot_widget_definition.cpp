#include "shell/bar/widgets/screenshot_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const terminusbar::bar::WidgetDefinition<ScreenshotWidget::Options>& screenshotWidgetDefinition() {
  using Options = ScreenshotWidget::Options;

  static const terminusbar::bar::WidgetDefinition<Options> definition{
      .type = "screenshot",
      .fields = terminusbar::bar::glyphButtonFields<Options>(),
  };
  return definition;
}
