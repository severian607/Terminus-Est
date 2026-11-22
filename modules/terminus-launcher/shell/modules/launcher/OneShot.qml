import Quickshell.Io

// A process run once for its stdout, created with Component.createObject and gone after it exits.
// `done(text)` gets the whole output; whoever cancels sets `done` to null (and running to false), so a late answer
// from a superseded query never arrives.
Process {
    id: proc

    property var done: null

    stdout: StdioCollector {
        onStreamFinished: {
            const answer = proc.done
            proc.done = null
            if (answer)
                answer(text)
        }
    }
    onExited: Qt.callLater(proc.destroy)
}
