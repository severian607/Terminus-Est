#include "shell/bar/widgets/caffeine_widget_definition.h"

const terminusbar::bar::WidgetDefinition<std::monostate>& caffeineWidgetDefinition() {
  static const terminusbar::bar::WidgetDefinition<std::monostate> definition{
      .type = "caffeine",
  };
  return definition;
}
