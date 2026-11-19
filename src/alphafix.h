// See-through leaves drawn in two passes (alphafix.cpp).
#pragma once
#include <windows.h>
#include <d3d8.h>

// At CreateDevice: hooks DrawPrimitive / DrawIndexedPrimitive through `patch` (the vtable patcher).
void InstallAlphaFix(IDirect3DDevice8* dev, DWORD createFlags, void* (*patch)(void*, int, void*));
void AlphaFixReset();   // after a successful Reset: render states are back at their defaults
void AnisotropyChanged();     // dllmain.cpp: the Modern Fix menu changed [Graphics] Anisotropic
void WaterDrawing(bool on);   // dllmain.cpp: around CWaterLevel::RenderWater (anisotropic filtering off)
void WaterMaskStep(void* fn);   // fixedtick.cpp HookMirror: the wave mask step (0x5BF540), 30 a second
