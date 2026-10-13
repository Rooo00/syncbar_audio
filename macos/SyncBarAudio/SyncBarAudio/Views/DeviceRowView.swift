import SwiftUI

// ── DeviceRowView ─────────────────────────────────────────────────────────────
//
// Fila de un dispositivo de audio individual.
// Cubre US-11 (slider + mute) y US-12 (inline label editor).

struct DeviceRowView: View {

    let device: AudioDeviceVM
    @EnvironmentObject private var vm: AudioViewModel

    // Estado local de edición de nombre
    @State private var isEditing   = false
    @State private var editingName = ""

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {

                // ── Ícono de estado ───────────────────────────────────────────
                deviceIcon

                // ── Nombre + Slider ───────────────────────────────────────────
                VStack(alignment: .leading, spacing: 3) {
                    nameField
                    volumeSlider
                }

                // ── Botón de mute ─────────────────────────────────────────────
                muteButton
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .contentShape(Rectangle())
        }
        .opacity(device.isActive ? 1.0 : 0.38)
    }

    // ── Subvistas ─────────────────────────────────────────────────────────────

    private var deviceIcon: some View {
        ZStack {
            Circle()
                .fill(device.isActive
                      ? Color.accentColor.opacity(0.12)
                      : Color.secondary.opacity(0.08))
                .frame(width: 28, height: 28)
            Image(systemName: iconName)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(device.isActive ? Color.accentColor : Color.secondary)
        }
    }

    private var iconName: String {
        if !device.isActive { return "speaker.slash" }
        if device.isMuted   { return "speaker.slash.fill" }
        switch device.volume {
        case 0:        return "speaker"
        case ..<0.33:  return "speaker.wave.1"
        case ..<0.66:  return "speaker.wave.2"
        default:       return "speaker.wave.3"
        }
    }

    @ViewBuilder
    private var nameField: some View {
        if isEditing {
            // ── Modo edición inline (US-12) ───────────────────────────────────
            TextField("Nombre del dispositivo", text: $editingName)
                .textFieldStyle(.plain)
                .font(.subheadline)
                .fontWeight(.medium)
                .onSubmit { commitEdit() }
                .onExitCommand { cancelEdit() }   // Escape cancela
                .focused($fieldFocused)
                .onChange(of: fieldFocused) { focused in
                    if !focused { commitEdit() }
                }
        } else {
            // ── Modo lectura ──────────────────────────────────────────────────
            HStack(spacing: 4) {
                Text(device.displayName)
                    .font(.subheadline)
                    .fontWeight(.medium)
                    .foregroundStyle(device.isActive ? .primary : .secondary)
                    .lineLimit(1)
                    .truncationMode(.tail)

                if !device.isActive {
                    Text("Desconectado")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(Color.secondary.opacity(0.12), in: Capsule())
                }
            }
            .onTapGesture(count: 2) { startEdit() }  // Doble clic → editar
            .help("Doble clic para renombrar")
        }
    }

    @FocusState private var fieldFocused: Bool

    private var volumeSlider: some View {
        HStack(spacing: 6) {
            // Valor numérico del volumen
            Text("\(Int(device.volume * 100))%")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .monospacedDigit()
                .frame(width: 30, alignment: .trailing)

            Slider(
                value: Binding(
                    get: { device.volume },
                    set: { vm.setDeviceVolume(uid: device.id, volume: $0) }
                ),
                in: 0...1
            )
            .disabled(!device.isActive || device.isMuted)
            .tint(sliderTint)
        }
    }

    private var sliderTint: Color {
        if !device.isActive { return .secondary }
        if device.isMuted   { return .secondary }
        return .accentColor
    }

    private var muteButton: some View {
        Button {
            vm.setDeviceMute(uid: device.id, muted: !device.isMuted)
        } label: {
            Image(systemName: device.isMuted ? "speaker.slash.fill" : "speaker.wave.1")
                .font(.system(size: 13))
                .foregroundStyle(device.isMuted ? Color.orange : Color.secondary)
                .frame(width: 24, height: 24)
                .background(
                    device.isMuted
                        ? Color.orange.opacity(0.12)
                        : Color.clear,
                    in: RoundedRectangle(cornerRadius: 5)
                )
        }
        .buttonStyle(.plain)
        .disabled(!device.isActive)
        .help(device.isMuted ? "Activar sonido" : "Silenciar")
    }

    // ── Lógica de edición ─────────────────────────────────────────────────────

    private func startEdit() {
        guard device.isActive else { return }
        editingName = device.displayName
        isEditing   = true
        fieldFocused = true
    }

    private func commitEdit() {
        let trimmed = editingName.trimmingCharacters(in: .whitespaces)
        if !trimmed.isEmpty && trimmed != device.displayName {
            vm.setDeviceLabel(uid: device.id, label: trimmed)
        }
        isEditing    = false
        fieldFocused = false
    }

    private func cancelEdit() {
        isEditing    = false
        fieldFocused = false
        // No guarda — descarta el cambio (US-12: Escape cancela sin guardar)
    }
}

// ── Previews ──────────────────────────────────────────────────────────────────

#if DEBUG
#Preview("Activo") {
    let vm = AudioViewModel.preview
    return DeviceRowView(device: vm.devices[0])
        .environmentObject(vm)
        .frame(width: 320)
        .background(.regularMaterial)
}

#Preview("Muteado") {
    let vm = AudioViewModel.preview
    var d = vm.devices[1]
    d.isMuted = true
    return DeviceRowView(device: d)
        .environmentObject(vm)
        .frame(width: 320)
        .background(.regularMaterial)
}

#Preview("Desconectado") {
    let vm = AudioViewModel.preview
    return DeviceRowView(device: vm.devices[3]) // Sony — isActive: false
        .environmentObject(vm)
        .frame(width: 320)
        .background(.regularMaterial)
}
#endif
