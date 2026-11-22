import Quickshell
import QtQuick
import qs.services
import qs.providers
import qs.modules.launcher

// Launcher-only entry point: the providers, the router that merges them, and the CRT launcher that renders items.
// Nothing else runs here: no bar, dock, lockscreen or notification daemon.
//
// Provider order is list order: a calculation first, then apps and windows, then system commands; files answer
// only to their "/" prefix.
//
//   qs -p <this dir> ipc call applauncher toggle | open | close | search <text>
ShellRoot {
    AppSource {
        id: apps
    }

    ComputusProvider {
        id: computus
    }

    IndexProvider {
        id: index
        appSource: apps
    }

    ImperiumProvider {
        id: imperium
    }

    ArchivumProvider {
        id: archivum
    }

    MemoriaProvider {
        id: memoria
    }

    Router {
        id: router
        providers: [computus, index, imperium, archivum, memoria]
    }

    Launcher {
        router: router
    }
}
