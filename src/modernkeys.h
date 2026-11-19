// Modern keys: the game's key bindings set to a modern layout (modernkeys.cpp).
#pragma once

void InstallModernKeys();
void ModernKeysAfterPresent();   // re-applies them when the game's menu closes
void ModernKeysChanged();        // the Modern Fix menu switched [Controls] ModernKeys
