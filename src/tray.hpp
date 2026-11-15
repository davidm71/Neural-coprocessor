// ---------------------------------------------------------------------------
// tray.hpp - TR1: the bridge's status as a notification-area (tray) icon.
//
// Replaces the status box (the idle screen on the bridge window) on rigs where
// that window sits on top of the game: the box is hidden until the first
// picture (HW1), and the icon says what it would have said.
//
// The icon (TR2): a blue rounded square with a white graphics card, and a
// status dot in the corner:
//   grey   waiting (nothing armed yet, or AutoArm is off)
//   amber  arming - waiting for depth / gameplay
//   green  running (tooltip: frames per second shown)
//   red    an error (tooltip: the error; a notification once per error)
//
// Right-click or left-click: a menu - show/hide the status window (before the
// first picture), hide/show the picture (after it), arm now, open the log
// folder.
//
// mgpu.ini: TrayIcon=1 (default) / 0 = no icon.
//
// Threads: install, tick, remove and on_message all run on the bridge thread,
// which owns the bridge window the icon reports to.
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>

namespace mgpu
{
namespace tray
{
void install(HWND owner);   // after the bridge window is created
void remove();              // before it is destroyed
void tick();                // ~1x a second: icon colour and tooltip
// From the bridge window's procedure: true = handled (*res is the result).
bool on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res);
}
}
