#pragma once

class Item;
class InventoryGUI;

#ifdef STACKSORT_ENABLE_ROTATION

void StackSort_InitRotateAPI();
bool StackSort_RotateAvailable();
bool StackSort_CanRotate(Item* item);
bool StackSort_IsRotated(Item* item);
bool StackSort_SetRotated(Item* item, bool rotated);
void StackSort_RefreshVisuals(InventoryGUI* gui);

#else

inline void StackSort_InitRotateAPI() {}
inline bool StackSort_RotateAvailable()
{
    return false;
}
inline bool StackSort_CanRotate(Item*)
{
    return false;
}
inline bool StackSort_IsRotated(Item*)
{
    return false;
}
inline bool StackSort_SetRotated(Item*, bool)
{
    return false;
}
inline void StackSort_RefreshVisuals(InventoryGUI*) {}

#endif

// AK::SoundEngine::PostEvent direct call, resolved from kenshi_x64.exe.
void StackSort_PlayClickSound();
