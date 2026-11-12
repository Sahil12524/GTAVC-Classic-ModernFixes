// Camera and object interpolation between game ticks (interp.cpp).
#pragma once

void InterpCapture();            // tick, after CGame::Process: record this tick's camera/objects
void InterpApply(float alpha, bool tick);   // put camera/objects at prev + (cur - prev) * alpha
void InterpPartsCapture();       // tick, after PreRender: record vehicle part matrices
void InterpPartsApply();         // after PreRender: blend vehicle parts (doors, wheels...)
void InterpRestore(bool tick);   // put back what InterpApply changed (tick: keep the frame's own changes)
void InterpReset();              // forget history (menu, load, cut)

// Per-object fields CRenderer::ConstructRenderList writes (alpha fade, visibility
// flags, scan code), for every object in the world pools.
void EntityFlagsSave();
void EntityFlagsRestore();
