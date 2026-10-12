import SwiftUI

// ── Punto de entrada de la aplicación ────────────────────────────────────────
//
// LSUIElement = YES en Info.plist garantiza que la app:
//   - No aparece en el Dock
//   - No aparece en el App Switcher (Cmd+Tab)
//   - Vive exclusivamente en la barra de menú (RF-04, US-10)

@main
struct AudioSyncProApp: App {

    // ViewModel compartido — StateObject garantiza una sola instancia
    @StateObject private var viewModel = AudioViewModel()

    var body: some Scene {
        // MenuBarExtra: el único punto de entrada visual de la app (macOS 13+)
        MenuBarExtra {
            PopoverView()
                .environmentObject(viewModel)
        } label: {
            // Ícono en la barra de menú — SF Symbol que cambia si hay grupos activos
            Label("SyncBar Audio", systemImage: viewModel.menuBarIcon)
                .labelStyle(.iconOnly)
        }
        .menuBarExtraStyle(.window) // Pop-over flotante, no menú desplegable
    }
}
