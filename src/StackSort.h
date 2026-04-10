#pragma once

class Item;

// All return false / no-op when KenshiRotate absent. Main thread only.

class InventoryGUI;

void StackSort_InitRotateAPI();
bool StackSort_RotateAvailable();
bool StackSort_CanRotate(Item* item);
bool StackSort_IsRotated(Item* item);
bool StackSort_SetRotated(Item* item, bool rotated);
void StackSort_RefreshVisuals(InventoryGUI* gui);

// Calls AK::SoundEngine::PostEvent("General_Click", 0x6E, ...) directly.
// Resolved at startup from kenshi_x64.exe. No-op if resolution failed.

void StackSort_PlayClickSound();
