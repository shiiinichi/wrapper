import Foundation
import CoreFoundation

nonisolated enum NativeArtistProfile {
    typealias Fields = [String: Any]

    static func metadata(_ data: Data, adam: String, storefront: String) throws -> Data? {
        guard let root = try JSONSerialization.jsonObject(with: data) as? Fields,
              root["errors"] == nil, root["failureType"] == nil, let rows = root["data"] as? [Fields] else {
            throw NativeWrapperError.invalidResponse
        }
        guard let artist = rows.first else { return nil }
        guard artist["type"] as? String == "artists", artist["id"] as? String == adam,
              let attributes = artist["attributes"] as? Fields,
              let name = attributes["name"] as? String, !name.isEmpty else { throw NativeWrapperError.invalidResponse }

        var result: Fields = ["adamId": adam, "storefront": storefront, "name": name,
                              "genreNames": attributes["genreNames"] as? [String] ?? []]
        for key in ["url", "artwork", "artistBio", "origin", "bornOrFormed", "editorialNotes"] {
            result[key] = attributes[key] ?? NSNull()
        }
        let editorial = attributes["editorialArtwork"] as? Fields
        result["artistLogo"] = rendition(editorial?["musicContentColorLogoTrimmed"], crop: "bb", format: "png") as Any? ?? NSNull()
        result["wideArtwork"] = wideArtwork(attributes, editorial: editorial) as Any? ?? NSNull()
        return try JSONSerialization.data(withJSONObject: result)
    }

    private static func wideArtwork(_ artist: Fields, editorial: Fields?) -> Fields? {
        if let dedicated = rendition(editorial?["centeredFullscreenBackground"], crop: "sr", format: "jpg", wide: true) {
            return dedicated
        }
        let hero = (artist["hero"] as? [Fields])?.first
        let content = (hero?["content"] as? [Fields])?.first
        guard let artwork = content?["artwork"] as? Fields,
              (artwork["recommendedCropCodes"] as? [String])?.contains("vf") == true else { return nil }
        return rendition(artwork, crop: "vf", format: "jpg", wide: true)
    }

    private static func dimension(_ value: Any?) -> Int? {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              let dimension = Int(exactly: number), dimension > 0 else { return nil }
        return dimension
    }

    private static func rendition(_ value: Any?, crop: String, format: String, wide: Bool = false) -> Fields? {
        guard let image = value as? Fields, let raw = image["url"] as? String,
              let width = dimension(image["width"]), let height = dimension(image["height"]),
              let url = URL(string: raw), url.scheme == "https", url.host?.isEmpty == false,
              url.user == nil, url.password == nil,
              let suffix = raw.range(of: #"/\{w\}x\{h\}(?:\{c\}|[A-Za-z0-9]*?)\.(?:\{f\}|jpg|jpeg|png|webp|avif)(?=[?#]|$)"#,
                                     options: .regularExpression) else { return nil }
        let template = raw.replacingCharacters(in: suffix, with: "/{w}x{h}" + crop + "." + format)
        let renderedWidth = wide ? 1600 : width
        let renderedHeight = wide ? 900 : height
        let rendered = template.replacingOccurrences(of: "{w}", with: String(renderedWidth))
            .replacingOccurrences(of: "{h}", with: String(renderedHeight))
        return ["url": rendered, "urlTemplate": template, "width": renderedWidth, "height": renderedHeight]
    }
}
