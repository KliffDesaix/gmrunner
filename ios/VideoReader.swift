// Video on iOS: AVFoundation's decoders (hardware on every device) behind the C functions the
// runner's decoder (src/platform/video/VideoApple.cpp) calls. Frames are read in order as BGRA and
// handed over as RGBA; the audio track is read whole as 16-bit stereo at the mixer's rate.
import AVFoundation
import CoreVideo

final class VideoReader {
    let asset: AVURLAsset
    let track: AVAssetTrack
    var reader: AVAssetReader?
    var output: AVAssetReaderTrackOutput?
    var pending: CMSampleBuffer?  // the next frame, read but not yet due

    init?(path: String) {
        asset = AVURLAsset(url: URL(fileURLWithPath: path))
        guard let t = asset.tracks(withMediaType: .video).first else { return nil }
        track = t
        start(at: 0)
    }

    var size: CGSize { track.naturalSize }
    var duration: Double { CMTimeGetSeconds(asset.duration) }

    func start(at seconds: Double) {
        reader?.cancelReading()
        pending = nil
        guard let r = try? AVAssetReader(asset: asset) else { return }
        let out = AVAssetReaderTrackOutput(track: track, outputSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        ])
        out.alwaysCopiesSampleData = false
        r.timeRange = CMTimeRange(start: CMTime(seconds: max(0, seconds), preferredTimescale: 600), end: .positiveInfinity)
        guard r.canAdd(out) else { return }
        r.add(out)
        r.startReading()
        reader = r
        output = out
    }

    // Writes the frame showing at `seconds` to rgba (width * height * 4) when it changed.
    func frame(at seconds: Double, into rgba: UnsafeMutablePointer<UInt8>, width: Int, height: Int) -> Bool {
        var changed = false
        while true {
            if pending == nil { pending = output?.copyNextSampleBuffer() }
            guard let sample = pending else { break }
            if CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)) > seconds { break }
            if let image = CMSampleBufferGetImageBuffer(sample) { copy(image, into: rgba, width: width, height: height) }
            pending = nil
            changed = true
        }
        return changed
    }

    private func copy(_ image: CVPixelBuffer, into rgba: UnsafeMutablePointer<UInt8>, width: Int, height: Int) {
        CVPixelBufferLockBaseAddress(image, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(image)?.assumingMemoryBound(to: UInt8.self) else { return }
        let stride = CVPixelBufferGetBytesPerRow(image)
        let w = min(width, CVPixelBufferGetWidth(image)), h = min(height, CVPixelBufferGetHeight(image))
        for y in 0..<h {
            let src = base + y * stride, dst = rgba + y * width * 4
            for x in 0..<w {
                dst[x * 4] = src[x * 4 + 2]
                dst[x * 4 + 1] = src[x * 4 + 1]
                dst[x * 4 + 2] = src[x * 4]
                dst[x * 4 + 3] = 255
            }
        }
    }

    // The whole audio track as interleaved 16-bit stereo at `rate`.
    func audio(rate: Int) -> [Int16]? {
        guard let t = asset.tracks(withMediaType: .audio).first, let r = try? AVAssetReader(asset: asset) else { return nil }
        let out = AVAssetReaderTrackOutput(track: t, outputSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: rate, AVNumberOfChannelsKey: 2,
            AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
            AVLinearPCMIsNonInterleaved: false,
        ])
        guard r.canAdd(out) else { return nil }
        r.add(out)
        r.startReading()
        var pcm: [Int16] = []
        while let sample = out.copyNextSampleBuffer() {
            guard let block = CMSampleBufferGetDataBuffer(sample) else { continue }
            let length = CMBlockBufferGetDataLength(block)
            var chunk = [Int16](repeating: 0, count: length / 2)
            chunk.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
            pcm += chunk
        }
        return pcm.isEmpty ? nil : pcm
    }
}

@_cdecl("gmr_av_open")
public func gmrAvOpen(_ path: UnsafePointer<CChar>, _ width: UnsafeMutablePointer<Int32>, _ height: UnsafeMutablePointer<Int32>,
                      _ duration: UnsafeMutablePointer<Double>) -> UnsafeMutableRawPointer? {
    guard let v = VideoReader(path: String(cString: path)) else { return nil }
    width.pointee = Int32(v.size.width)
    height.pointee = Int32(v.size.height)
    duration.pointee = v.duration
    return Unmanaged.passRetained(v).toOpaque()
}

@_cdecl("gmr_av_frame")
public func gmrAvFrame(_ handle: UnsafeMutableRawPointer, _ seconds: Double, _ rgba: UnsafeMutablePointer<UInt8>,
                       _ width: Int32, _ height: Int32) -> Int32 {
    let v = Unmanaged<VideoReader>.fromOpaque(handle).takeUnretainedValue()
    return v.frame(at: seconds, into: rgba, width: Int(width), height: Int(height)) ? 1 : 0
}

@_cdecl("gmr_av_seek")
public func gmrAvSeek(_ handle: UnsafeMutableRawPointer, _ seconds: Double) {
    Unmanaged<VideoReader>.fromOpaque(handle).takeUnretainedValue().start(at: seconds)
}

// The audio as malloc'd samples (freed by the caller with free()); null without a sound track.
@_cdecl("gmr_av_audio")
public func gmrAvAudio(_ handle: UnsafeMutableRawPointer, _ rate: Int32, _ count: UnsafeMutablePointer<Int>) -> UnsafeMutablePointer<Int16>? {
    guard let pcm = Unmanaged<VideoReader>.fromOpaque(handle).takeUnretainedValue().audio(rate: Int(rate)) else { return nil }
    let out = UnsafeMutablePointer<Int16>.allocate(capacity: pcm.count)
    out.initialize(from: pcm, count: pcm.count)
    count.pointee = pcm.count
    return out
}

@_cdecl("gmr_av_free_audio")
public func gmrAvFreeAudio(_ samples: UnsafeMutablePointer<Int16>) { samples.deallocate() }

@_cdecl("gmr_av_close")
public func gmrAvClose(_ handle: UnsafeMutableRawPointer) {
    Unmanaged<VideoReader>.fromOpaque(handle).release()
}
