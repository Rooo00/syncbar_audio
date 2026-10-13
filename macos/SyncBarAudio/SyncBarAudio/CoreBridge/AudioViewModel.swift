import SwiftUI
import Combine

// ── Modelos de vista ──────────────────────────────────────────────────────────

struct AudioDeviceVM: Identifiable, Equatable {
    let id: String
    var displayName: String
    var nativeName: String
    var volume: Float
    var isMuted: Bool
    var isActive: Bool
    var hasVolumeControl: Bool = true   // false = HDMI/DisplayPort u otro sin volumen
}

struct AudioGroupVM: Identifiable, Equatable {
    let id: Int32
    var name: String
    var masterVolume: Float
    var isMuted: Bool
    var isActive: Bool
    var members: [GroupMemberVM]
    var isOutput: Bool = false   // el grupo es la salida de audio del sistema (ADR-07)
}

struct GroupMemberVM: Identifiable, Equatable {
    let id: String
    var relativeVolume: Float
    var isActive: Bool
}

// ── Variable global para callbacks C (no pueden capturar contexto Swift) ──────
// CoreAudio invoca estos callbacks desde su propio hilo.
// La referencia débil evita retain cycles.
private weak var _sharedViewModel: AudioViewModel?

// ── AudioViewModel ────────────────────────────────────────────────────────────

/// Estado observable de la app. Conecta SwiftUI con el Core C++ vía bridge.
/// Usa ObservableObject (macOS 10.15+) para compatibilidad con macOS 13.
final class AudioViewModel: ObservableObject {

    @Published var devices: [AudioDeviceVM] = []
    @Published var groups:  [AudioGroupVM]  = []
    @Published var isReady: Bool            = false

    /// Último error al activar un grupo como salida (se muestra en su fila)
    @Published var routingError: (groupId: Int32, message: String)?

    var menuBarIcon: String {
        if groups.contains(where: { $0.isOutput }) { return "hifispeaker.2.fill" }
        return groups.contains { $0.isActive && !$0.isMuted }
            ? "speaker.wave.2.fill"
            : "speaker.wave.2"
    }

    // ── Ruta de la base de datos ──────────────────────────────────────────────

    static var dbPath: String {
        let appSupport = FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("SyncBarAudio", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("syncbar.db").path
    }

    // ── Inicialización ────────────────────────────────────────────────────────

    init() {
        setupBridge()
    }

    /// Para previews — omite sb_init()
    init(skipBridgeInit: Bool) {
        if !skipBridgeInit { setupBridge() }
    }

    deinit {
        sb_shutdown()
    }

    private func setupBridge() {
        // Registrar callbacks antes de sb_init para no perder eventos tempranos
        _sharedViewModel = self
        sb_register_device_callbacks(
            { uid in
                guard let uid else { return }
                let s = String(cString: uid)
                DispatchQueue.main.async { _sharedViewModel?.handleDeviceConnected(uid: s) }
            },
            { uid in
                guard let uid else { return }
                let s = String(cString: uid)
                DispatchQueue.main.async { _sharedViewModel?.handleDeviceDisconnected(uid: s) }
            }
        )

        let result = sb_init(Self.dbPath)
        guard result == SB_OK else {
            print("[SyncBar] sb_init failed: \(result.rawValue)")
            return
        }
        isReady = true
        refreshAll()
    }

    // ── Handlers de hardware ──────────────────────────────────────────────────

    func handleDeviceConnected(uid: String) {
        // Una reconexión también reactiva membresías de grupo (US-05)
        refreshAll()
    }

    func handleDeviceDisconnected(uid: String) {
        // El core pudo reconstruir o desactivar la salida del grupo (ADR-07)
        refreshAll()
    }

    // ── Refresh ───────────────────────────────────────────────────────────────

    func refreshAll() {
        refreshDevices()
        refreshGroups()
    }

    func refreshDevices() {
        var buf = [SB_DeviceInfo](repeating: SB_DeviceInfo(), count: 64)
        var n: Int32 = 0
        guard sb_get_devices(&buf, 64, &n) == SB_OK else { return }
        devices = (0..<Int(n)).map { i in
            let d = buf[i]
            return AudioDeviceVM(
                id:          cStr(d.uid),
                displayName: cStr(d.display_name),
                nativeName:  cStr(d.native_name),
                volume:      d.volume,
                isMuted:     d.is_muted,
                isActive:    d.is_active,
                hasVolumeControl: d.has_volume_control
            )
        }
    }

    func refreshGroups() {
        var gBuf = [SB_GroupInfo](repeating: SB_GroupInfo(), count: 32)
        var gN: Int32 = 0
        guard sb_get_groups(&gBuf, 32, &gN) == SB_OK else { return }
        groups = (0..<Int(gN)).map { i in
            let g = gBuf[i]
            var mBuf = [SB_MemberInfo](repeating: SB_MemberInfo(), count: 4)
            var mN: Int32 = 0
            sb_get_group_members(g.id, &mBuf, &mN)
            let members = (0..<Int(mN)).map { j -> GroupMemberVM in
                let m = mBuf[j]
                return GroupMemberVM(id: cStr(m.device_uid),
                                     relativeVolume: m.relative_volume,
                                     isActive: m.is_active)
            }
            return AudioGroupVM(id: g.id, name: cStr(g.name),
                                masterVolume: g.master_volume,
                                isMuted: g.is_muted, isActive: g.is_active,
                                members: members, isOutput: g.is_output)
        }
    }

    // ── Acciones de dispositivos ──────────────────────────────────────────────

    func setDeviceVolume(uid: String, volume: Float) {
        if sb_set_device_volume(uid, volume) == SB_OK,
           let idx = devices.firstIndex(where: { $0.id == uid }) {
            devices[idx].volume = volume
            refreshGroups()  // el core recalcula el relativo de los grupos que lo contienen
        }
    }

    /// Volumen relativo de un miembro (US-03): el core aplica master × relativo al hardware.
    func setMemberVolume(groupId: Int32, uid: String, relativeVolume: Float) {
        if sb_set_member_volume(groupId, uid, relativeVolume) == SB_OK {
            refreshGroups()
            refreshDevices()
        }
    }

    func setDeviceMute(uid: String, muted: Bool) {
        if sb_set_device_mute(uid, muted) == SB_OK,
           let idx = devices.firstIndex(where: { $0.id == uid }) {
            devices[idx].isMuted = muted
        }
    }

    func setDeviceLabel(uid: String, label: String) {
        if sb_set_device_label(uid, label) == SB_OK,
           let idx = devices.firstIndex(where: { $0.id == uid }) {
            devices[idx].displayName = label.isEmpty ? devices[idx].nativeName : label
        }
    }

    // ── Acciones de grupos ────────────────────────────────────────────────────

    func createGroup(name: String) {
        var gid: Int32 = 0
        if sb_create_group(name, &gid) == SB_OK { refreshGroups() }
    }

    func setGroupVolume(groupId: Int32, volume: Float) {
        if sb_set_group_volume(groupId, volume) == SB_OK,
           let idx = groups.firstIndex(where: { $0.id == groupId }) {
            groups[idx].masterVolume = volume
        }
    }

    func setGroupMute(groupId: Int32, muted: Bool) {
        if sb_set_group_mute(groupId, muted) == SB_OK,
           let idx = groups.firstIndex(where: { $0.id == groupId }) {
            groups[idx].isMuted = muted
        }
    }

    func setGroupName(groupId: Int32, name: String) {
        if sb_set_group_name(groupId, name) == SB_OK,
           let idx = groups.firstIndex(where: { $0.id == groupId }) {
            groups[idx].name = name
        }
    }

    @discardableResult
    func addDeviceToGroup(groupId: Int32, deviceUid: String) -> SB_Result {
        let r = sb_add_device_to_group(groupId, deviceUid)
        if r == SB_OK { refreshGroups() }
        return r
    }

    func removeDeviceFromGroup(groupId: Int32, deviceUid: String) {
        if sb_remove_device_from_group(groupId, deviceUid) == SB_OK { refreshGroups() }
    }

    func deleteGroup(groupId: Int32) {
        if sb_delete_group(groupId) == SB_OK {
            // Borrar el grupo activo restaura la salida previa: releer todo
            refreshGroups()
        }
    }

    // ── Ruteo: grupo como salida del sistema (ADR-07) ─────────────────────────

    func activateGroupOutput(groupId: Int32) {
        let result = sb_activate_group_output(groupId)
        if result == SB_OK {
            routingError = nil
        } else {
            routingError = (groupId: groupId, message: Self.routingMessage(for: result))
        }
        refreshGroups()
    }

    func deactivateGroupOutput() {
        _ = sb_deactivate_group_output()
        routingError = nil
        refreshGroups()
    }

    private static func routingMessage(for result: SB_Result) -> String {
        switch result {
        case SB_ERR_NO_ACTIVE_MEMBERS: return "Ningún dispositivo del grupo está conectado."
        case SB_ERR_ROUTING_FAILURE:   return "macOS no permitió crear la salida del grupo."
        case SB_ERR_GROUP_NOT_FOUND:   return "El grupo ya no existe."
        default:                       return "No se pudo usar el grupo como salida."
        }
    }
}

// ── Helper: leer buffer C como String ────────────────────────────────────────

private func cStr<T>(_ buf: T) -> String {
    withUnsafeBytes(of: buf) { ptr in
        let bytes = ptr.bindMemory(to: CChar.self)
        let end = bytes.firstIndex(of: 0) ?? bytes.endIndex
        return String(bytes: bytes[..<end].map { UInt8(bitPattern: $0) },
                      encoding: .utf8) ?? ""
    }
}
