import QtQuick

// Every dialog's panel: dark, a quiet border, and the corners Hyprland gives
// windows (sharp on Omarchy's default).
Rectangle {
    color: "#1c1c1e"
    border.width: 1
    border.color: "#3a3a3e"
    radius: theme.radius
}
