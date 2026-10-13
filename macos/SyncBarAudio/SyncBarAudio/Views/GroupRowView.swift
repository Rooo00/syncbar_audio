import SwiftUI

// ── GroupRowView ──────────────────────────────────────────────────────────────
//
// Fila de un grupo virtual con:
//   - Slider maestro de volumen (US-13)
//   - Mute lógico (ADR-03, US-13)
//   - Usar el grupo como salida de audio del sistema (ADR-07)
//   - Edición inline del nombre (US-12 aplicado a grupos)
//   - Lista expandible de miembros con volumen relativo
//   - Botón de eliminar grupo

struct GroupRowView: View {

    let group: AudioGroupVM
    @EnvironmentObject private var vm: AudioViewModel

    @State private var isExpanded    = false
    @State private var isEditingName = false
    @State private var editingName   = ""
    @State private var showAddSheet  = false
    @State private var showDeleteAlert = false

    var body: some View {
        VStack(spacing: 0) {
            // ── Fila principal ────────────────────────────────────────────────
            HStack(spacing: 10) {
                groupIcon
                VStack(alignment: .leading, spacing: 3) {
                    nameRow
                    masterSlider
                    if let error = vm.routingError, error.groupId == group.id {
                        Text(error.message)
                            .font(.caption2)
                            .foregroundStyle(.orange)
                    }
                }
                outputButton
                controlButtons
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .contentShape(Rectangle())
            .onTapGesture { withAnimation(.easeInOut(duration: 0.18)) { isExpanded.toggle() } }

            // ── Miembros expandibles ──────────────────────────────────────────
            if isExpanded {
                membersList
                    .transition(.opacity.combined(with: .move(edge: .top)))
            }
        }
        .opacity(group.isActive ? 1.0 : 0.45)
        .sheet(isPresented: $showAddSheet) {
            GroupEditorSheet(group: group)
                .environmentObject(vm)
        }
        .alert("Eliminar grupo", isPresented: $showDeleteAlert) {
            Button("Eliminar", role: .destructive) { vm.deleteGroup(groupId: group.id) }
            Button("Cancelar", role: .cancel) {}
        } message: {
            Text("¿Eliminar \"\(group.name)\"? Los dispositivos no se verán afectados.")
        }
    }

    // ── Subvistas ─────────────────────────────────────────────────────────────

    private var groupIcon: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 7)
                .fill(group.isActive
                      ? Color.accentColor.opacity(0.12)
                      : Color.secondary.opacity(0.08))
                .frame(width: 28, height: 28)
            Image(systemName: group.isMuted
                  ? "rectangle.3.group"
                  : "rectangle.3.group.fill")
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(group.isActive ? Color.accentColor : Color.secondary)
        }
    }

    @ViewBuilder
    private var nameRow: some View {
        if isEditingName {
            InlineLabelEditor(
                placeholder: "Nombre del grupo",
                text: $editingName,
                onCommit: { name in
                    vm.setGroupName(groupId: group.id, name: name)
                    isEditingName = false
                },
                onCancel: { isEditingName = false }
            )
        } else {
            HStack(spacing: 6) {
                Text(group.name)
                    .font(.subheadline)
                    .fontWeight(.semibold)
                    .foregroundStyle(group.isActive ? .primary : .secondary)
                    .lineLimit(1)

                // Badge de miembros
                Text("\(group.members.count)/4")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 1)
                    .background(Color.secondary.opacity(0.12), in: Capsule())

                if group.isOutput {
                    Text("Salida")
                        .font(.caption2)
                        .foregroundStyle(Color.accentColor)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(Color.accentColor.opacity(0.12), in: Capsule())
                        .help("Todo el audio del sistema sale por este grupo")
                }

                if !group.isActive {
                    Text("Sin señal")
                        .font(.caption2)
                        .foregroundStyle(.orange)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(Color.orange.opacity(0.12), in: Capsule())
                }

                Spacer()

                // Chevron de expansión
                Image(systemName: isExpanded ? "chevron.up" : "chevron.down")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            .onTapGesture(count: 2) {
                editingName   = group.name
                isEditingName = true
            }
            .help("Doble clic para renombrar")
        }
    }

    private var masterSlider: some View {
        HStack(spacing: 6) {
            Text("\(Int(group.masterVolume * 100))%")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .monospacedDigit()
                .frame(width: 30, alignment: .trailing)

            Slider(
                value: Binding(
                    get: { group.masterVolume },
                    set: { vm.setGroupVolume(groupId: group.id, volume: $0) }
                ),
                in: 0...1
            )
            .disabled(!group.isActive || group.isMuted)
            .tint(group.isMuted ? Color.secondary : Color.accentColor)
        }
    }

    // Usar / dejar de usar el grupo como salida del sistema (ADR-07)
    private var outputButton: some View {
        Button {
            if group.isOutput {
                vm.deactivateGroupOutput()
            } else {
                vm.activateGroupOutput(groupId: group.id)
            }
        } label: {
            Image(systemName: group.isOutput ? "stop.fill" : "play.fill")
                .font(.system(size: 11))
                .foregroundStyle(group.isOutput ? Color.white : Color.accentColor)
                .frame(width: 26, height: 26)
                .background(
                    group.isOutput ? Color.accentColor : Color.accentColor.opacity(0.12),
                    in: Circle()
                )
        }
        .buttonStyle(.plain)
        .disabled(!group.isActive && !group.isOutput)
        .help(group.isOutput ? "Dejar de usar como salida" : "Usar como salida de audio")
    }

    private var controlButtons: some View {
        VStack(spacing: 4) {
            // Mute lógico
            Button {
                vm.setGroupMute(groupId: group.id, muted: !group.isMuted)
            } label: {
                Image(systemName: group.isMuted ? "speaker.slash.fill" : "speaker.wave.2")
                    .font(.system(size: 12))
                    .foregroundStyle(group.isMuted ? Color.orange : Color.secondary)
                    .frame(width: 24, height: 20)
                    .background(
                        group.isMuted ? Color.orange.opacity(0.12) : Color.clear,
                        in: RoundedRectangle(cornerRadius: 5)
                    )
            }
            .buttonStyle(.plain)
            .help(group.isMuted ? "Activar grupo" : "Silenciar grupo (lógico)")

            // Menú contextual
            Menu {
                Button {
                    editingName   = group.name
                    isEditingName = true
                } label: {
                    Label("Renombrar", systemImage: "pencil")
                }
                Button {
                    showAddSheet = true
                } label: {
                    Label("Gestionar dispositivos", systemImage: "slider.horizontal.3")
                }
                Divider()
                Button(role: .destructive) {
                    showDeleteAlert = true
                } label: {
                    Label("Eliminar grupo", systemImage: "trash")
                }
            } label: {
                Image(systemName: "ellipsis")
                    .font(.system(size: 12))
                    .foregroundStyle(Color.secondary)
                    .frame(width: 24, height: 20)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .frame(width: 24)
        }
    }

    // ── Lista de miembros ─────────────────────────────────────────────────────

    private var membersList: some View {
        VStack(spacing: 0) {
            Divider().padding(.leading, 52)

            if group.members.isEmpty {
                HStack {
                    Text("Sin dispositivos — toca ··· para agregar")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .italic()
                    Spacer()
                }
                .padding(.leading, 52)
                .padding(.vertical, 8)
            } else {
                ForEach(group.members) { member in
                    MemberRowView(groupId: group.id, member: member)
                        .environmentObject(vm)
                    if member.id != group.members.last?.id {
                        Divider().padding(.leading, 52)
                    }
                }
            }
        }
        .background(Color.primary.opacity(0.03))
    }
}

// ── MemberRowView ─────────────────────────────────────────────────────────────
// Fila de un miembro dentro del grupo expandido.

private struct MemberRowView: View {

    let groupId: Int32
    let member: GroupMemberVM
    @EnvironmentObject private var vm: AudioViewModel

    // Nombre del dispositivo desde el ViewModel
    private var deviceName: String {
        vm.devices.first(where: { $0.id == member.id })?.displayName ?? member.id
    }

    var body: some View {
        HStack(spacing: 10) {
            // Indicador de estado
            Circle()
                .fill(member.isActive ? Color.accentColor : Color.secondary)
                .frame(width: 6, height: 6)
                .padding(.leading, 52)

            Text(deviceName)
                .font(.caption)
                .foregroundStyle(member.isActive ? .primary : .secondary)
                .lineLimit(1)

            Spacer()

            // Volumen relativo del miembro (US-03)
            Slider(
                value: Binding(
                    get: { Double(member.relativeVolume) },
                    set: { vm.setMemberVolume(groupId: groupId, uid: member.id,
                                              relativeVolume: Float($0)) }
                ),
                in: 0...1
            )
            .controlSize(.mini)
            .frame(width: 80)
            .help("Volumen relativo dentro del grupo")

            Text("\(Int(member.relativeVolume * 100))%")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .monospacedDigit()
                .frame(width: 32, alignment: .trailing)

            // Botón de quitar del grupo
            Button {
                vm.removeDeviceFromGroup(groupId: groupId, deviceUid: member.id)
            } label: {
                Image(systemName: "minus.circle")
                    .font(.caption)
                    .foregroundStyle(Color.secondary)
            }
            .buttonStyle(.plain)
            .help("Quitar del grupo")
            .padding(.trailing, 14)
        }
        .padding(.vertical, 7)
        .opacity(member.isActive ? 1.0 : 0.45)
    }
}

// ── GroupEditorSheet ──────────────────────────────────────────────────────────
// Sheet para agregar/quitar dispositivos de un grupo (US-13).

struct GroupEditorSheet: View {

    let group: AudioGroupVM
    @EnvironmentObject private var vm: AudioViewModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text("Gestionar \"\(group.name)\"")
                    .font(.headline)
                Spacer()
                Button("Listo") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(16)

            Divider()

            // Lista de todos los dispositivos con toggle
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(vm.devices) { device in
                        let isMember = group.members.contains { $0.id == device.id }
                        let atLimit  = group.members.count >= 4 && !isMember

                        HStack(spacing: 12) {
                            Image(systemName: device.isActive
                                  ? "speaker.fill" : "speaker.slash")
                                .foregroundStyle(device.isActive
                                                 ? Color.accentColor : Color.secondary)
                                .frame(width: 20)

                            VStack(alignment: .leading, spacing: 1) {
                                Text(device.displayName)
                                    .font(.subheadline)
                                    .foregroundStyle(device.isActive ? .primary : .secondary)
                                if !device.isActive {
                                    Text("Desconectado")
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                }
                            }

                            Spacer()

                            if atLimit {
                                Text("Límite 4")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            } else {
                                Toggle("", isOn: Binding(
                                    get: { isMember },
                                    set: { add in
                                        if add {
                                            vm.addDeviceToGroup(groupId: group.id,
                                                                deviceUid: device.id)
                                        } else {
                                            vm.removeDeviceFromGroup(groupId: group.id,
                                                                     deviceUid: device.id)
                                        }
                                    }
                                ))
                                .labelsHidden()
                            }
                        }
                        .padding(.horizontal, 16)
                        .padding(.vertical, 10)
                        .opacity(atLimit ? 0.4 : 1.0)

                        Divider().padding(.leading, 48)
                    }
                }
            }
            .frame(minHeight: 200, maxHeight: 400)

            // Footer informativo
            HStack {
                Image(systemName: "info.circle")
                    .foregroundStyle(.secondary)
                Text("Máximo 4 dispositivos por grupo")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
            }
            .padding(12)
            .background(Color.secondary.opacity(0.06))
        }
        .frame(width: 340)
    }
}

// ── Previews ──────────────────────────────────────────────────────────────────

#if DEBUG
#Preview("Grupo activo") {
    let vm = AudioViewModel.preview
    return GroupRowView(group: vm.groups[0])
        .environmentObject(vm)
        .frame(width: 320)
        .background(.regularMaterial)
}

#Preview("Grupo como salida") {
    let vm = AudioViewModel.preview
    var g = vm.groups[0]
    g.isOutput = true
    return GroupRowView(group: g)
        .environmentObject(vm)
        .frame(width: 320)
        .background(.regularMaterial)
}

#Preview("Grupo expandido") {
    let vm = AudioViewModel.preview
    return GroupRowView(group: vm.groups[0])
        .environmentObject(vm)
        .frame(width: 320)
        .background(.regularMaterial)
        .onAppear { }
}

#Preview("Editor de grupo") {
    let vm = AudioViewModel.preview
    return GroupEditorSheet(group: vm.groups[0])
        .environmentObject(vm)
}
#endif
