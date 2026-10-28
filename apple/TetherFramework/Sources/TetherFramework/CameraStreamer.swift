//
//  CameraStreamer.swift
//  TetherFramework
//
//  Streams the camera to tetherd as H.264 over its own pinned TLS connection,
//  which the daemon publishes as a webcam.
//

import AVFoundation
import Foundation
import Network
import Security
import VideoToolbox

@Observable
public final class CameraStreamer: NSObject {
    // True from the connection opening until stop() or a failure.
    public private(set) var isStreaming = false
    // Why the last stream ended, if it failed.
    public private(set) var lastError: String?

    // Feed for a preview layer; runs only while streaming.
    public let session = AVCaptureSession()
    // The camera feeding `session`, for rotating a preview to match.
    public private(set) var device: AVCaptureDevice?

    // Everything below is confined to `queue`.
    private let queue = DispatchQueue(label: "tether.camera")
    @ObservationIgnored private var connection: NWConnection?
    @ObservationIgnored private var encoder: VTCompressionSession?
    // Turns frames upright and pillarboxes portrait ones into the encoder's landscape size.
    @ObservationIgnored private var fitter: FrameFitter?
    @ObservationIgnored private var rotation: AVCaptureDevice.RotationCoordinator?
    @ObservationIgnored private var rotationObservation: NSKeyValueObservation?
    // Clockwise degrees that turn the sensor's landscape frame upright.
    @ObservationIgnored private var captureAngle: Double = 0
    // Frames handed to the encoder whose bytes the network has not taken yet.
    @ObservationIgnored private var pending = 0

    private static let maxPending = 2
    private static let fps = 30
    private static let bitRate = 6_000_000

    // Streams the camera to the daemon at host:port. The connection is refused
    // unless the server presents `pinnedFingerprint`.
    public func start(
        host: String,
        port: UInt16,
        pinnedFingerprint: String,
        identity: SecIdentity,
        position: AVCaptureDevice.Position = .back
    ) {
        stop()
        lastError = nil
        isStreaming = true

        let params = TetherConnection.tlsParameters(identity: identity) { fingerprint in
            !pinnedFingerprint.isEmpty && fingerprint == pinnedFingerprint
        }
        let conn = NWConnection(
            to: .hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!),
            using: params
        )
        conn.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.sendStart(on: conn)
                self.startCapture(position: position)
            case .failed(let error):
                self.fail(error.localizedDescription)
            case .waiting(let error):
                // Unreachable now; the stream is pointless if it starts minutes later.
                self.fail(error.localizedDescription)
            default:
                break
            }
        }
        queue.async {
            self.connection = conn
            self.pending = 0
            conn.start(queue: self.queue)
        }
    }

    public func stop() {
        isStreaming = false
        queue.async { self.teardown() }
    }

    // MARK: - Private

    private func fail(_ message: String) {
        teardown()
        DispatchQueue.main.async {
            self.lastError = message
            self.isStreaming = false
        }
    }

    private func teardown() {
        if session.isRunning { session.stopRunning() }
        rotationObservation = nil
        rotation = nil
        if let encoder {
            VTCompressionSessionInvalidate(encoder)
        }
        encoder = nil
        fitter = nil
        DispatchQueue.main.async { self.device = nil }
        connection?.stateUpdateHandler = nil
        connection?.cancel()
        connection = nil
    }

    private func sendStart(on conn: NWConnection) {
        let start: [String: Any] = ["command": "camera_start", "codec": "h264", "fps": Self.fps]
        guard var line = try? JSONSerialization.data(withJSONObject: start) else { return }
        line.append(0x0A)
        conn.send(content: line, completion: .contentProcessed { _ in })
    }

    private func startCapture(position: AVCaptureDevice.Position) {
        session.beginConfiguration()
        session.inputs.forEach { session.removeInput($0) }
        session.outputs.forEach { session.removeOutput($0) }

        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: position),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input)
        else {
            session.commitConfiguration()
            fail("Camera unavailable")
            return
        }
        session.addInput(input)
        // Older front cameras top out below 1080p.
        session.sessionPreset = session.canSetSessionPreset(.hd1920x1080) ? .hd1920x1080 : .high

        let output = AVCaptureVideoDataOutput()
        output.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        ]
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: queue)
        session.addOutput(output)
        // The sensor's own landscape, always. Rotating the connection while running
        // rebuilds the capture graph, which stalls the feed and the UI; FrameFitter
        // turns each frame upright instead.
        if let videoConnection = output.connection(with: .video), videoConnection.isVideoRotationAngleSupported(0) {
            videoConnection.videoRotationAngle = 0
        }
        session.commitConfiguration()

        captureAngle = 0
        let rotation = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: nil)
        rotationObservation = rotation.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.initial, .new]) {
            [weak self] coordinator, _ in
            let angle = coordinator.videoRotationAngleForHorizonLevelCapture
            self?.queue.async { self?.captureAngle = angle }
        }
        self.rotation = rotation
        session.startRunning()
        DispatchQueue.main.async { self.device = device }
    }

    private func makeEncoder(width: Int32, height: Int32) -> VTCompressionSession? {
        var session: VTCompressionSession?
        guard VTCompressionSessionCreate(
            allocator: nil,
            width: width,
            height: height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,
            refcon: nil,
            compressionSessionOut: &session
        ) == noErr, let session else { return nil }

        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel,
                             value: kVTProfileLevel_H264_Main_AutoLevel)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        // A viewer that joins mid-stream waits at most this long for a picture.
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                             value: NSNumber(value: 2))
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ExpectedFrameRate,
                             value: NSNumber(value: Self.fps))
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate,
                             value: NSNumber(value: Self.bitRate))
        VTCompressionSessionPrepareToEncodeFrames(session)
        return session
    }

    // `conn` is the connection the frame was encoded for; frames finishing after
    // it was torn down are dropped.
    private func encoded(_ status: OSStatus, _ sample: CMSampleBuffer?, for conn: NWConnection) {
        guard status == noErr, let sample, let payload = Self.annexB(sample) else {
            queue.async { if self.connection === conn { self.pending -= 1 } }
            return
        }
        let keyframe = Self.isKeyframe(sample)
        let pts = CMSampleBufferGetPresentationTimeStamp(sample)
        let ptsUs = pts.isValid ? UInt64(max(0, CMTimeGetSeconds(pts) * 1_000_000)) : 0

        var frame = Data(capacity: 13 + payload.count)
        frame.appendBigEndian(UInt32(9 + payload.count))
        frame.append(keyframe ? 1 : 0)
        frame.appendBigEndian(ptsUs)
        frame.append(payload)

        queue.async {
            guard self.connection === conn else { return }
            conn.send(content: frame, completion: .contentProcessed { [weak self] error in
                // Cancelling a connection fails its queued sends; that is not an error.
                guard let self, self.connection === conn else { return }
                self.pending -= 1
                if let error { self.fail(error.localizedDescription) }
            })
        }
    }

    private static func isKeyframe(_ sample: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false)
                as? [[CFString: Any]],
              let first = attachments.first
        else { return true }
        return !(first[kCMSampleAttachmentKey_NotSync] as? Bool ?? false)
    }

    // Converts VideoToolbox's AVCC output to Annex B, with SPS/PPS ahead of keyframes.
    static func annexB(_ sample: CMSampleBuffer) -> Data? {
        let startCode: [UInt8] = [0, 0, 0, 1]
        var out = Data()

        if isKeyframe(sample), let format = CMSampleBufferGetFormatDescription(sample) {
            var count = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            for i in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format, parameterSetIndex: i, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let pointer
                {
                    out.append(contentsOf: startCode)
                    out.append(pointer, count: size)
                }
            }
        }

        guard let block = CMSampleBufferGetDataBuffer(sample) else { return nil }
        let length = CMBlockBufferGetDataLength(block)
        var avcc = Data(count: length)
        let copied = avcc.withUnsafeMutableBytes {
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!)
        }
        guard copied == kCMBlockBufferNoErr else { return nil }

        // ponytail: assumes 4-byte NAL lengths, which VideoToolbox always emits for H.264
        var offset = 0
        while offset + 4 <= avcc.count {
            let nalLength = avcc[offset..<offset + 4].reduce(0) { Int($0) << 8 | Int($1) }
            offset += 4
            guard nalLength > 0, offset + nalLength <= avcc.count else { return nil }
            out.append(contentsOf: startCode)
            out.append(avcc[offset..<offset + nalLength])
            offset += nalLength
        }
        return out
    }
}

extension CameraStreamer: AVCaptureVideoDataOutputSampleBufferDelegate {
    public func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        guard let conn = self.connection, let image = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        // Skip frames before encoding while the network is behind, so no
        // encoded frame is ever lost and the decoder never needs a fresh keyframe.
        guard pending < Self.maxPending else { return }

        if encoder == nil {
            // Always landscape, even when the stream starts in portrait.
            let w = CVPixelBufferGetWidth(image), h = CVPixelBufferGetHeight(image)
            fitter = FrameFitter(width: max(w, h), height: min(w, h))
            encoder = makeEncoder(width: Int32(max(w, h)), height: Int32(min(w, h)))
        }
        guard let encoder, let fitter else {
            fail("H.264 encoder unavailable")
            return
        }
        guard let fitted = fitter.fit(image, rotation: captureAngle) else { return }

        pending += 1
        let status = VTCompressionSessionEncodeFrame(
            encoder,
            imageBuffer: fitted,
            presentationTimeStamp: CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
            duration: .invalid,
            frameProperties: nil,
            infoFlagsOut: nil
        ) { [weak self] status, _, sample in
            self?.encoded(status, sample, for: conn)
        }
        if status != noErr { pending -= 1 }
    }
}

private extension Data {
    mutating func appendBigEndian<T: FixedWidthInteger>(_ value: T) {
        Swift.withUnsafeBytes(of: value.bigEndian) { append(contentsOf: $0) }
    }
}
