#include "shell/bar/widgets/custom_button_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const terminusbar::bar::WidgetDefinition<CustomButtonWidget::Options>& customButtonWidgetDefinition() {
  using terminusbar::bar::field;
  using Options = CustomButtonWidget::Options;

  static const terminusbar::bar::WidgetDefinition<Options> definition{
      .type = "custom_button",
      .fields = terminusbar::bar::glyphButtonFields<Options>(
          field<&Options::label>({
              .key = "label",
          }),
          field<&Options::tooltip>({
              .key = "tooltip",
          }),
          field<&Options::markup>({
              .key = "markup",
          }),
          field<&Options::activeFile>({
              .key = "active_file",
          }),
          field<&Options::activeLabel>({
              .key = "active_label",
          }),
          field<&Options::glowColor>({
              .key = "glow_color",
          }),
          field<&Options::glowWidth>({
              .key = "glow_width",
              .minValue = 0.0,
              .maxValue = 200.0,
              .step = 1.0,
          }),
          field<&Options::glowOffset>({
              .key = "glow_offset",
              .minValue = -100.0,
              .maxValue = 100.0,
              .step = 1.0,
          })
      ),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
