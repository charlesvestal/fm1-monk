// Exports the twelve characters for MONK-1 (fm1-monk, the FM-1 firmware): each scene at
// 240 x 240, scale 1, as layers (backdrop + body; + face per blink / loudness), reference frames
// with the mouth, and every character's voice and mouth style as JSON. Not an assertion test. Run:
//
//   scripts/test.sh MonkSynthTests/ExportFM1Art   (writes /tmp/fm1-monk-art)
import UIKit
import XCTest
@testable import MonkSynth

final class ExportFM1Art: XCTestCase {
    static let side: CGFloat = 240
    static let out = URL(fileURLWithPath: "/tmp/fm1-monk-art")

    private func render(_ c: Character, face: Expression?, mouth: (Float, CGFloat)?) -> UIImage {
        let fmt = UIGraphicsImageRendererFormat()
        fmt.scale = 1
        fmt.opaque = true
        let rect = CGRect(x: 0, y: 0, width: Self.side, height: Self.side)
        return UIGraphicsImageRenderer(size: rect.size, format: fmt).image { _ in
            c.drawBackdrop(in: rect, stage: rect)
            c.drawBody(in: rect)
            if let e = face { c.drawFace(in: rect, expression: e) }
            if let (v, loud) = mouth {
                c.drawMouth(in: rect, vowel: v, amplitudeBoost: 1 + loud * 0.35)
                c.drawOverMouth(in: rect)
            }
        }
    }

    private func save(_ img: UIImage, _ name: String) throws {
        try XCTUnwrap(img.pngData()).write(to: Self.out.appendingPathComponent(name + ".png"))
    }

    private func hex(_ c: UIColor) -> String {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        c.getRed(&r, green: &g, blue: &b, alpha: &a)
        return String(format: "%02X%02X%02X", Int((r * 255).rounded()), Int((g * 255).rounded()), Int((b * 255).rounded()))
    }

    func testExport() throws {
        try FileManager.default.createDirectory(at: Self.out, withIntermediateDirectories: true)
        var meta: [[String: Any]] = []
        for c in CharacterRegistry.all {
            try save(render(c, face: nil, mouth: nil), "\(c.id)_base")
            for b in 0...1 {
                for l in 0...4 {
                    let e = Expression(blinking: b == 1, loudness: CGFloat(l) / 4, vowel: 0.5)
                    try save(render(c, face: e, mouth: nil), "\(c.id)_face_b\(b)_l\(l)")
                }
            }
            for vi in [0, 3, 6, 9, 12, 15, 17, 20, 23] {
                for l in [0, 2, 4] {
                    let v = Float(vi) / 23
                    let e = Expression(blinking: false, loudness: CGFloat(l) / 4, vowel: v)
                    try save(render(c, face: e, mouth: (v, CGFloat(l) / 4)), "\(c.id)_ref_v\(vi)_l\(l)")
                }
            }
            var entry: [String: Any] = ["id": c.id, "name": c.displayName,
                                        "accent": hex(c.palette.accent), "skyTop": hex(c.palette.skyTop),
                                        "skyBottom": hex(c.palette.skyBottom), "ground": hex(c.palette.ground)]
            if let t = c as? any ToonCharacter {
                let s = t.mouthStyle
                entry["mouth"] = ["x": s.x, "y": s.y, "scale": s.scale,
                                  "variant": s.variant == .lips ? "lips" : s.variant == .muzzle ? "muzzle" : "bare",
                                  "lip": hex(s.lip)]
            }
            var values = [String: Float]()
            for p in Param.allCases { values[p.identifier] = p.defaultValue }
            for (p, v) in c.savedParameters ?? CharacterVoiceTable.voice(for: c) { values[p.identifier] = v }
            entry["params"] = values
            meta.append(entry)
        }
        // The over-mouth shapes' shadow tones (Toon.shape's default: the fill at 80% brightness).
        let shades = ["F4F3EE": hex(UIColor(hex: 0xF4F3EE).adjusted(brightnessScale: 0.8)),
                      "7A3D22": hex(UIColor(hex: 0x7A3D22).adjusted(brightnessScale: 0.8))]
        let doc: [String: Any] = ["characters": meta, "shades": shades,
                                  "paramOrder": Param.allCases.map(\.identifier)]
        let data = try JSONSerialization.data(withJSONObject: doc, options: [.prettyPrinted, .sortedKeys])
        try data.write(to: Self.out.appendingPathComponent("characters.json"))
        print("FM1_ART_WRITTEN \(Self.out.path)")
    }
}
