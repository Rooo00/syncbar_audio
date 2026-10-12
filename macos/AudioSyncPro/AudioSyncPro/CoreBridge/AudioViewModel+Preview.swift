import Foundation

// ── Datos de muestra para SwiftUI Previews ────────────────────────────────────
// Solo se compilan en DEBUG. No afectan el binario de producción.

#if DEBUG
extension AudioViewModel {

    /// ViewModel con dispositivos y grupos de muestra (Andrés y Sofía)
    static var preview: AudioViewModel {
        let vm = AudioViewModel(skipBridgeInit: true)
        vm.devices = [
            AudioDeviceVM(
                id:          "UID-BOCINAS-ESCRITORIO",
                displayName: "Bocinas Escritorio",
                nativeName:  "USB Audio Device",
                volume:      0.75,
                isMuted:     false,
                isActive:    true
            ),
            AudioDeviceVM(
                id:          "UID-AIRPODS",
                displayName: "AirPods Pro",
                nativeName:  "AirPods Pro",
                volume:      0.50,
                isMuted:     false,
                isActive:    true
            ),
            AudioDeviceVM(
                id:          "UID-JBL",
                displayName: "Bocina Izquierda",
                nativeName:  "JBL Flip 5",
                volume:      0.80,
                isMuted:     false,
                isActive:    true
            ),
            AudioDeviceVM(
                id:          "UID-SONY",
                displayName: "Bocina Derecha",
                nativeName:  "Sony SRS-XB23",
                volume:      0.60,
                isMuted:     false,
                isActive:    false  // Desconectada — aparece atenuada
            ),
        ]
        vm.groups = [
            AudioGroupVM(
                id:           1,
                name:         "Cuarto",
                masterVolume: 0.70,
                isMuted:      false,
                isActive:     true,
                members: [
                    GroupMemberVM(id: "UID-JBL",  relativeVolume: 1.0, isActive: true),
                    GroupMemberVM(id: "UID-SONY",  relativeVolume: 0.8, isActive: false),
                ]
            )
        ]
        vm.isReady = true
        return vm
    }

    /// ViewModel vacío (sin dispositivos)
    static var previewEmpty: AudioViewModel {
        let vm = AudioViewModel(skipBridgeInit: true)
        vm.isReady = true
        return vm
    }
}
#endif
