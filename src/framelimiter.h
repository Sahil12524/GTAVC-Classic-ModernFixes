// Frame limiter (framelimiter.cpp).
#pragma once

void FrameLimiterInit();             // DllMain, after LoadConfig
void FrameLimiterSetRefresh(int hz); // the display's refresh rate, once the device exists
void FrameLimiterSetVsync(bool on);  // fullscreen with vsync: FPSLimit=0 leaves pacing to it
bool FrameLimiterPacing();           // frames held to fixed times (else shown when done)
long long FrameLimiterFrameTime();   // when the frame being started will be shown (QPC)
void FrameLimiterBeforePresent();    // holds the frame until its time
void FrameLimiterAfterPresent();     // after every IDirect3DDevice8::Present
// Frame timing figures for the log (every 10 s of play, the first few minutes).
void FrameStatsPresent(long long qpcBlocked);       // time spent inside Present
void FrameStatsWork(bool tick, long long qpcTaken);  // a game update / an in-between frame
