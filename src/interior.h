// Interiors follow the player: teleporting out of a room or flying into one
// switches the drawn area and loads the scene; the city is drawn from inside
// interiors built into it, and they are drawn from the street (interior.cpp).
#pragma once
#include <windows.h>
#include <d3d8.h>

typedef HRESULT (__stdcall* SetViewport_t)(IDirect3DDevice8*, const D3DVIEWPORT8*);

void InstallInterior();        // first frame, with the other game hooks
void InteriorEndOfTick();      // end of a game update (fixedtick.cpp)
void InteriorAfterPresent();   // after Present (dllmain.cpp): used when FixedTick is off
void InteriorDepthFormat(int d3dFormat);   // the device's depth buffer format (dllmain.cpp)
// IDirect3DDevice8::SetViewport (dllmain.cpp): while two areas are drawn, the
// depth range is shrunk a little for everything but the second area.
HRESULT InteriorSetViewport(IDirect3DDevice8* dev, const D3DVIEWPORT8* vp, SetViewport_t orig);
