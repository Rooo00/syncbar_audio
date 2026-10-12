import SwiftUI

// ── GroupRowView ──────────────────────────────────────────────────────────────
//
// Fila de un grupo virtual con slider maestro y mute lógico.
// Implementación completa en F9.
// Este stub permite que PopoverView compile en F7.

struct GroupRowView: View {

    let group: AudioGroupVM
    @EnvironmentObject private var vm: AudioViewModel

    var body: some View {
        VStack(spacing: 0) {
            // ── Fila principal del grupo ──────────────────────────────────────
            HStack(spacing: 12) {
                Image(systemName: "rectangle.3.group.fill")
                    .foregroundStyle(group.isActive ? Color.accentColor : Color.secondary)
                    .frame(width: 20)

                VStack(alignment: .leading, spacing: 2) {
                    Text(group.name)
                        .font(.subheadline)
                        .fontWeight(.medium)
                        .foregroundStyle(group.isActive ? .primary : .secondary)
                        .lineLimit(1)

                    // Slider maestro — implementación completa en F9
                    Slider(value: Binding(
                        get: { group.masterVolume },
                        set: { vm.setGroupVolume(groupId: group.id, volume: $0) }
                    ), in: 0...1)
                    .disabled(!group.isActive || group.isMuted)
                    .tint(group.isMuted ? Color.secondary : Color.accentColor)
                }

                // Botón de mute lógico
                Button {
                    vm.setGroupMute(groupId: group.id, muted: !group.isMuted)
                } label: {
                    Image(systemName: group.isMuted
                          ? "speaker.slash.fill"
                          : "speaker.wave.2.fill")
                        .foregroundStyle(group.isMuted ? .orange : .secondary)
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .opacity(group.isActive ? 1.0 : 0.4)

            // ── Miembros del grupo (colapsados) ───────────────────────────────
            // Implementación completa en F9
            if !group.members.isEmpty {
                HStack(spacing: 4) {
                    ForEach(group.members) { member in
                        Circle()
                            .fill(member.isActive ? Color.accentColor : Color.secondary)
                            .frame(width: 6, height: 6)
                    }
                    Text("\(group.members.count) dispositivo\(group.members.count == 1 ? "" : "s")")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
                .padding(.horizontal, 48)
                .padding(.bottom, 8)
            }
        }
    }
}

#Preview {
    let vm = AudioViewModel.preview
    return GroupRowView(group: vm.groups[0])
        .environmentObject(vm)
        .frame(width: 320)
}
