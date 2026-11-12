// Frame limiter (framelimiter.cpp).
#pragma once

void FrameLimiterInit();             // DllMain, after LoadConfig
void FrameLimiterSetRefresh(int hz); // the display's refresh rate, once the device exists
void FrameLimiterWait();             // after every IDirect3DDevice8::Present
