// SyncBarBridge.h — Bridging header para Swift
//
// Importa la C-API del Core Engine para que Swift pueda llamar
// directamente a las funciones del bridge sin wrappers adicionales.
//
// Configurar en Xcode:
//   Build Settings → Swift Compiler - General →
//   Objective-C Bridging Header → AudioSyncPro/CoreBridge/SyncBarBridge.h

#import "bridge_api.h"
