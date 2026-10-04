#pragma once
bool register_websocket_vendor();
void unregister_websocket_vendor();
// Module-unload safety valve: forget the vendor handle without calling into obs-websocket.
// Safe to call while libobs is tearing modules down.
void detach_websocket_vendor();
