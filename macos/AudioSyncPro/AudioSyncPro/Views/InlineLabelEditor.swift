import SwiftUI

// ── InlineLabelEditor ─────────────────────────────────────────────────────────
//
// Campo de edición inline reutilizable para nombres de dispositivos y grupos.
// US-12: Enter guarda, Escape cancela, clic fuera guarda.

struct InlineLabelEditor: View {

    let placeholder: String
    @Binding var text: String
    let onCommit: (String) -> Void
    let onCancel: () -> Void

    @FocusState private var isFocused: Bool

    var body: some View {
        TextField(placeholder, text: $text)
            .textFieldStyle(.plain)
            .font(.subheadline)
            .fontWeight(.medium)
            .onSubmit {
                commit()
            }
            .onExitCommand {
                onCancel()
            }
            .focused($isFocused)
            .onChange(of: isFocused) { focused in
                if !focused { commit() }
            }
            .onAppear {
                isFocused = true
            }
    }

    private func commit() {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        onCommit(trimmed.isEmpty ? placeholder : trimmed)
    }
}
