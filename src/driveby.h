// Free drive-by: Alt takes a gun out in any car, bike or boat, aimed with the camera (driveby.cpp).
#pragma once

void InstallDriveBy();        // first frame, with the other game hooks
void DriveByAfterPresent();   // each frame (dllmain.cpp): the drive-by key
bool DriveByAiming();         // drive-by mode on: FreeCam keeps the camera where it is
bool DriveByWantsWheel();     // drive-by mode on: the mouse wheel switches guns (hunter.cpp's wheel hook)
void DriveByWheel(int delta); // wheel movement taken from the game: up next gun, down previous
