// Streaming memory and draw distance (drawdist.cpp).
#pragma once

void PatchStreamingEarly();    // DllMain: before CStreaming::Init
void InstallDrawDistance();    // first frame, with the other game hooks
void DrawDistanceFrame();      // every frame (Present): follow the menu's Draw Distance
