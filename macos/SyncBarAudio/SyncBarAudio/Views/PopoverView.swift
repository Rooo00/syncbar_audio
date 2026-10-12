import SwiftUI

// ── PopoverView ───────────────────────────────────────────────────────────────
//
// Contenedor principal del pop-over de la barra de menú.
// Implementación completa de DeviceRowView y GroupRowView en F8/F9.

struct PopoverView: View {

    @EnvironmentObject private var vm: AudioViewModel

    var body: some View {
        VStack(spacing: 0) {
            // ── Header ────────────────────────────────────────────────────────
            HStack {
                Image(systemName: "speaker.wave.2.fill")
                    .foregroundStyle(Color.accentColor)
                Text("SyncBar Audio")
                    .font(.headline)
                Spacer()
                // Botón de quit
                Button {
                    NSApplication.shared.terminate(nil)
                } label: {
                    Image(systemName: "power")
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .help("Salir de SyncBar Audio")
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)

            Divider()

            // ── Contenido principal ───────────────────────────────────────────
            if !vm.isReady {
                // Estado de carga
                VStack(spacing: 8) {
                    ProgressView()
                    Text("Iniciando motor de audio…")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(24)

            } else if vm.devices.isEmpty {
                // Sin dispositivos detectados
                VStack(spacing: 8) {
                    Image(systemName: "speaker.slash")
                        .font(.title2)
                        .foregroundStyle(.secondary)
                    Text("No se detectaron dispositivos de audio")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity)
                .padding(24)

            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        // ── Dispositivos individuales ─────────────────────────
                        if !vm.devices.isEmpty {
                            SectionHeader(title: "Dispositivos")
                            ForEach(vm.devices) { device in
                                DeviceRowView(device: device)
                                if device.id != vm.devices.last?.id {
                                    Divider().padding(.leading, 16)
                                }
                            }
                        }

                        // ── Grupos virtuales ──────────────────────────────────
                        if !vm.groups.isEmpty {
                            Divider().padding(.top, 8)
                            SectionHeader(title: "Grupos")
                            ForEach(vm.groups) { group in
                                GroupRowView(group: group)
                                if group.id != vm.groups.last?.id {
                                    Divider().padding(.leading, 16)
                                }
                            }
                        }
                    }
                }
                .frame(maxHeight: 480)
            }

            Divider()

            // ── Footer: botón de nuevo grupo ──────────────────────────────────
            if vm.isReady {
                Button {
                    vm.createGroup(name: "Nuevo Grupo")
                } label: {
                    Label("Nuevo grupo", systemImage: "plus.circle")
                        .font(.caption)
                        .foregroundStyle(Color.accentColor)
                }
                .buttonStyle(.plain)
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
            }
        }
        .frame(width: 320)
        .background(.regularMaterial)
    }
}

// ── SectionHeader ─────────────────────────────────────────────────────────────

private struct SectionHeader: View {
    let title: String
    var body: some View {
        HStack {
            Text(title.uppercased())
                .font(.caption2)
                .fontWeight(.semibold)
                .foregroundStyle(.secondary)
                .tracking(0.5)
            Spacer()
        }
        .padding(.horizontal, 16)
        .padding(.top, 10)
        .padding(.bottom, 4)
    }
}

// ── Previews ──────────────────────────────────────────────────────────────────

#if DEBUG
#Preview("Con dispositivos") {
    let vm = AudioViewModel.preview
    return PopoverView()
        .environmentObject(vm)
}

#Preview("Sin dispositivos") {
    let vm = AudioViewModel.previewEmpty
    return PopoverView()
        .environmentObject(vm)
}
#endif
