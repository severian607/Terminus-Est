-- # Laptop input (loaded after input.lua)

hl.config({
  input = {
    touchpad = { natural_scroll = true, tap_to_click = true, disable_while_typing = true },
  },
})

-- One device by name (from `hyprctl devices`): faster adaptive pointer motion for the touchpad only.
hl.device({
  name = "synps/2-synaptics-touchpad",
  sensitivity = 0.3,
  accel_profile = "adaptive",
})
