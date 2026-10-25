import CoreGraphics
let pid = Int(CommandLine.arguments[1])!
let windows = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as? [[String:Any]] ?? []
for w in windows where (w[kCGWindowOwnerPID as String] as? Int) == pid && (w[kCGWindowLayer as String] as? Int) == 0 {
    print(w[kCGWindowNumber as String]!)
}
