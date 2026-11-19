#pragma once
// Pizza Boy with the mouse: right button held, left button throws at the customer (pizza.cpp).
void InstallPizza();          // first frame (main.scm is loaded by then)
void PizzaAfterPresent();     // once a frame: the mouse
// Before a script command runs (stuntjump.cpp's ProcessOneCommand hook): true when
// it was the pizza script's button question and has been answered here.
bool PizzaScriptCommand(unsigned char* script);
